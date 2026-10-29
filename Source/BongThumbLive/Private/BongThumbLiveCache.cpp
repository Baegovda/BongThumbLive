#include "BongThumbLiveCache.h"
#include "BongThumbLiveModule.h"
#include "BongThumbLiveRenderer.h"
#include "BongThumbLiveSettings.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TextureResource.h"
#include "CanvasTypes.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "RHICommandList.h"
#include "Slate/SlateTextures.h"
#include "AssetThumbnail.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Compression/OodleDataCompression.h"
#include "Hash/CityHash.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Containers/Queue.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "ImageUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include <atomic>

namespace BongCacheConst
{
	static constexpr uint32 Magic = 0x434C5442; // "BTLC"
	static constexpr int32 FileVersion = 1;
	/** Bump when the rendering changes in a way that makes old cached pictures wrong. */
	static constexpr int32 KeyVersion = 1;
	static constexpr int32 MaxConcurrentLoads = 4;
	static constexpr int32 MaxUploadsPerTick = 2;
	static const TCHAR* IndexFileName = TEXT("index.txt");
}

// ------------------------------------------------------------------------------------------------------------------
// Async plumbing
// ------------------------------------------------------------------------------------------------------------------

struct FBongFileHeader
{
	uint32 Magic = BongCacheConst::Magic;
	int32 Version = BongCacheConst::FileVersion;
	FString Key;
	FString AssetPath;
	FString AnimSig;
	int32 CellW = 0, CellH = 0, Cols = 0, Rows = 0, NumFrames = 0;
	float Fps = 0.f, Length = 0.f;
	TArray<FString> DepPackages;
	TArray<FString> DepSigs;
	int64 RawSize = 0;
	int64 CompSize = 0;

	friend FArchive& operator<<(FArchive& Ar, FBongFileHeader& H)
	{
		Ar << H.Magic << H.Version;
		if (H.Magic != BongCacheConst::Magic || H.Version != BongCacheConst::FileVersion)
		{
			Ar.SetError();
			return Ar;
		}
		Ar << H.Key << H.AssetPath << H.AnimSig << H.CellW << H.CellH << H.Cols << H.Rows << H.NumFrames << H.Fps << H.Length;
		Ar << H.DepPackages << H.DepSigs << H.RawSize << H.CompSize;
		return Ar;
	}
};

struct FBongDiskLoadResult
{
	FSoftObjectPath Path;
	FString FileName;
	FString Key;
	bool bOk = false;
	bool bStale = false;
	FBongFileHeader Header;
	TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> Pixels;
	double DecodeMs = 0.0;
};

struct FBongDiskWriteResult
{
	FString FileName;
	FString AssetPath;
	FString AnimSig;
	FString Deps;
	int64 Bytes = 0;
	bool bOk = false;
};

struct FBongScanIndexEntry
{
	FString FileName;
	FString AssetPath;
	FString AnimSig;
	FString Deps;
	int64 Bytes = 0;
	int64 LastUse = 0;
};

struct FBongScanResult
{
	TArray<FBongScanIndexEntry> Entries;
	int32 Deleted = 0;
	int32 Missing = 0;
	int32 Stale = 0;
	double Ms = 0.0;
};

struct FBongAsyncShared
{
	TQueue<FBongDiskLoadResult*, EQueueMode::Mpsc> Loads;
	TQueue<FBongDiskWriteResult*, EQueueMode::Mpsc> Writes;
	TQueue<FBongScanResult*, EQueueMode::Mpsc> Scans;
	std::atomic<int32> Outstanding{ 0 };
	std::atomic<bool> bShutdown{ false };
};

struct FBongReadbackJob
{
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	FBongAtlasRef Atlas;
	FBongFileHeader Header;
	FString FileName;
	FString Deps;
	TArray<uint8> Pixels;
	/** 0 = created, 1 = copy enqueued, 2 = pixels ready, 3 = failed */
	std::atomic<int32> State{ 0 };
};

/** Package file signature: "<timestamp ticks>:<size>" of its .uasset, or empty when it has no file. Thread safe. */
static FString BongComputePackageSig(const FString& PackageName)
{
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
	{
		return FString();
	}
	const FFileStatData Stat = IFileManager::Get().GetStatData(*Filename);
	if (!Stat.bIsValid || Stat.bIsDirectory)
	{
		return FString();
	}
	return FString::Printf(TEXT("%lld:%lld"), Stat.ModificationTime.GetTicks(), Stat.FileSize);
}

static bool BongDepsStillValid(const FString& Deps)
{
	// "pkg|sig;pkg|sig"
	TArray<FString> Parts;
	Deps.ParseIntoArray(Parts, TEXT(";"), true);
	for (const FString& Part : Parts)
	{
		FString Pkg, Sig;
		if (Part.Split(TEXT("|"), &Pkg, &Sig) && BongComputePackageSig(Pkg) != Sig)
		{
			return false;
		}
	}
	return true;
}

static FString BongJoinDeps(const FBongFileHeader& H)
{
	FString Out;
	for (int32 i = 0; i < H.DepPackages.Num() && i < H.DepSigs.Num(); ++i)
	{
		if (!Out.IsEmpty())
		{
			Out += TEXT(";");
		}
		Out += H.DepPackages[i] + TEXT("|") + H.DepSigs[i];
	}
	return Out;
}

static bool BongReadHeader(const FString& File, FBongFileHeader& OutHeader, TArray<uint8>* OutPayload)
{
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*File));
	if (!Reader)
	{
		return false;
	}
	*Reader << OutHeader;
	if (Reader->IsError() || OutHeader.CompSize <= 0 || OutHeader.RawSize <= 0 || OutHeader.RawSize > (int64)512 * 1024 * 1024)
	{
		return false;
	}
	if (OutPayload)
	{
		if (Reader->TotalSize() - Reader->Tell() < OutHeader.CompSize)
		{
			return false;
		}
		OutPayload->SetNumUninitialized(OutHeader.CompSize);
		Reader->Serialize(OutPayload->GetData(), OutHeader.CompSize);
		if (Reader->IsError())
		{
			return false;
		}
	}
	return Reader->Close();
}

static double BongLoopTime(float Length, bool bHovered)
{
	if (Length <= UE_KINDA_SMALL_NUMBER)
	{
		return 0.0;
	}
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const double Fps = bHovered ? (double)FMath::Clamp(Settings->HoverFPS, 1, 120) : (double)FMath::Clamp(Settings->MaxFPS, 1, 60);
	const double Rate = (double)FMath::Clamp(Settings->PlaybackRate, 0.05f, 10.f);
	const double Now = FMath::FloorToDouble(FPlatformTime::Seconds() * Fps) / Fps;
	return FMath::Clamp(FMath::Fmod(Now * Rate, (double)Length), 0.0, (double)Length);
}

static void BongRegisterRT(FRDGBuilder& GraphBuilder, TMap<FRHITexture*, FRDGTextureRef>& Map, FRHITexture* Tex, FRDGTextureRef& Out, const TCHAR* Name)
{
	if (FRDGTextureRef* Found = Map.Find(Tex))
	{
		Out = *Found;
		return;
	}
	Out = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Tex, Name));
	Map.Add(Tex, Out);
}

// ------------------------------------------------------------------------------------------------------------------
// Entry helpers
// ------------------------------------------------------------------------------------------------------------------

int32 FBongCacheEntry::FrameIndexAt(float Time) const
{
	if (NumFrames <= 0)
	{
		return INDEX_NONE;
	}
	return FMath::Clamp(FMath::FloorToInt(Time * Fps + 1e-3f), 0, NumFrames - 1);
}

FIntRect FBongCacheEntry::CellRect(int32 Frame) const
{
	const int32 C = Frame % FMath::Max(1, Cols);
	const int32 R = Frame / FMath::Max(1, Cols);
	return FIntRect(C * CellW, R * CellH, C * CellW + CellW, R * CellH + CellH);
}

int32 FBongCacheEntry::NearestValid(int32 Frame) const
{
	if (NumValid <= 0 || NumFrames <= 0)
	{
		return INDEX_NONE;
	}
	for (int32 Back = 0; Back < NumFrames; ++Back)
	{
		const int32 Index = (Frame - Back + NumFrames * 2) % NumFrames;
		if (Valid.IsValidIndex(Index) && Valid[Index])
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

// ------------------------------------------------------------------------------------------------------------------
// Singleton
// ------------------------------------------------------------------------------------------------------------------

static FBongThumbCache* GBongThumbCache = nullptr;

FBongThumbCache& FBongThumbCache::Get()
{
	if (!GBongThumbCache)
	{
		GBongThumbCache = new FBongThumbCache();
		GBongThumbCache->Startup();
	}
	return *GBongThumbCache;
}

FBongThumbCache* FBongThumbCache::GetPtr()
{
	return GBongThumbCache;
}

void FBongThumbCache::Shutdown()
{
	if (GBongThumbCache)
	{
		GBongThumbCache->ShutdownInternal();
		delete GBongThumbCache;
		GBongThumbCache = nullptr;
	}
}

FBongThumbCache::FBongThumbCache()
{
	Shared = MakeShared<FBongAsyncShared, ESPMode::ThreadSafe>();
}

FBongThumbCache::~FBongThumbCache()
{
}

FString FBongThumbCache::GetCacheDir() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BongThumbLive") / TEXT("Cache"));
}

void FBongThumbCache::Startup()
{
	const FString Dir = GetCacheDir();
	IFileManager::Get().MakeDirectory(*Dir, true);
	// Background scan: read the index (and headers of files missing from it), delete files whose animation no longer
	// exists or changed (package signature) or whose mesh / skeleton changed, and leftovers of interrupted writes.
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Dir]()
	{
		const double T0 = FPlatformTime::Seconds();
		FBongScanResult* Result = new FBongScanResult();
		TMap<FString, FBongScanIndexEntry> FromIndex;
		FString IndexText;
		if (FFileHelper::LoadFileToString(IndexText, *(Dir / BongCacheConst::IndexFileName)))
		{
			TArray<FString> Lines;
			IndexText.ParseIntoArrayLines(Lines, true);
			for (const FString& Line : Lines)
			{
				TArray<FString> Cols;
				Line.ParseIntoArray(Cols, TEXT("\t"), false);
				if (Cols.Num() >= 5)
				{
					FBongScanIndexEntry E;
					E.FileName = Cols[0];
					E.AssetPath = Cols[1];
					E.AnimSig = Cols[2];
					LexFromString(E.Bytes, *Cols[3]);
					LexFromString(E.LastUse, *Cols[4]);
					E.Deps = Cols.Num() > 5 ? Cols[5] : FString();
					FromIndex.Add(E.FileName, E);
				}
			}
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.tmp")), true, false);
		for (const FString& F : Files)
		{
			IFileManager::Get().Delete(*(Dir / F), false, true, true);
		}
		Files.Reset();
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.btlc")), true, false);
		const int64 NowUnix = FDateTime::UtcNow().ToUnixTimestamp();
		for (const FString& F : Files)
		{
			if (S->bShutdown)
			{
				break;
			}
			const FString Full = Dir / F;
			FBongScanIndexEntry E;
			if (const FBongScanIndexEntry* Found = FromIndex.Find(F))
			{
				E = *Found;
			}
			else
			{
				FBongFileHeader H;
				if (!BongReadHeader(Full, H, nullptr))
				{
					IFileManager::Get().Delete(*Full, false, true, true);
					++Result->Deleted;
					continue;
				}
				E.FileName = F;
				E.AssetPath = H.AssetPath;
				E.AnimSig = H.AnimSig;
				E.Deps = BongJoinDeps(H);
				E.LastUse = NowUnix;
			}
			E.Bytes = IFileManager::Get().FileSize(*Full);
			const FString PackageName = FSoftObjectPath(E.AssetPath).GetLongPackageName();
			const FString CurrentSig = PackageName.IsEmpty() ? FString() : BongComputePackageSig(PackageName);
			if (CurrentSig.IsEmpty())
			{
				IFileManager::Get().Delete(*Full, false, true, true); // animation deleted / moved while the editor was closed
				++Result->Deleted;
				++Result->Missing;
				continue;
			}
			if (CurrentSig != E.AnimSig || !BongDepsStillValid(E.Deps))
			{
				IFileManager::Get().Delete(*Full, false, true, true); // animation, mesh or skeleton changed: stale
				++Result->Deleted;
				++Result->Stale;
				continue;
			}
			Result->Entries.Add(E);
		}
		Result->Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		S->Scans.Enqueue(Result);
		S->Outstanding--;
	});
}

void FBongThumbCache::ShutdownInternal()
{
	Shared->bShutdown = true;
	const double Start = FPlatformTime::Seconds();
	while (Shared->Outstanding.load() > 0 && FPlatformTime::Seconds() - Start < 10.0)
	{
		FPlatformProcess::Sleep(0.01f);
	}
	ProcessAsyncResults(FPlatformTime::Seconds());
	if (bIndexReady && bIndexDirty)
	{
		WriteIndex(true);
	}
	PushTiles.Reset();
	FlushRenderingCommands(); // pending copies reference the atlases
	Readbacks.Reset();
	for (FBongDiskLoadResult* R : PendingUploads)
	{
		delete R;
	}
	PendingUploads.Reset();
	FBongDiskLoadResult* L = nullptr;
	while (Shared->Loads.Dequeue(L)) { delete L; }
	FBongDiskWriteResult* W = nullptr;
	while (Shared->Writes.Dequeue(W)) { delete W; }
	FBongScanResult* Sc = nullptr;
	while (Shared->Scans.Dequeue(Sc)) { delete Sc; }
	Entries.Reset();
	MemBytes = 0;
	FillTarget = nullptr;
}

void FBongThumbCache::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(FillTarget);
}

UTextureRenderTarget2D* FBongThumbCache::GetFillTarget()
{
	if (!FillTarget)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		RT->RenderTargetFormat = RTF_RGBA8;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		// Same as the pool's tile render target: non-sRGB B8G8R8A8 with the engine display gamma.
		RT->TargetGamma = GEngine ? GEngine->GetDisplayGamma() : 2.2f;
		RT->InitCustomFormat(TileSize, TileSize, PF_B8G8R8A8, /*bInForceLinearGamma*/ true);
		RT->UpdateResourceImmediate(true);
		FillTarget = RT;
	}
	return FillTarget;
}

// ------------------------------------------------------------------------------------------------------------------
// Keys, geometry, budget
// ------------------------------------------------------------------------------------------------------------------

FString FBongThumbCache::GetPackageSig(FName PackageName, double Now)
{
	if (TPair<FString, double>* Found = SigCache.Find(PackageName))
	{
		if (Now - Found->Value < 10.0)
		{
			return Found->Key;
		}
	}
	const FString Sig = BongComputePackageSig(PackageName.ToString());
	if (SigCache.Num() > 16384)
	{
		SigCache.Reset();
	}
	SigCache.Add(PackageName, TPair<FString, double>(Sig, Now));
	return Sig;
}

void FBongThumbCache::OnPackageSaved(FName PackageName)
{
	SigCache.Remove(PackageName);
	for (TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		if (Pair.Value->PackageName == PackageName || Pair.Value->MeshPackage == PackageName || Pair.Value->SkeletonPackage == PackageName)
		{
			Pair.Value->LastKeyCheck = 0.0; // re-validate on the next tick
		}
	}
}

bool FBongThumbCache::ComputeKey(const FSoftObjectPath& Path, FName PackageName, FString& OutKey, FString& OutAnimSig, FString& OutFileName)
{
	OutAnimSig = GetPackageSig(PackageName, FPlatformTime::Seconds());
	if (OutAnimSig.IsEmpty())
	{
		return false; // never saved: memory only
	}
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	OutKey = FString::Printf(TEXT("BTLC%d|%s|%s|res=%d|fps=%d|max=%d|stable=%d|tile=%d"), BongCacheConst::KeyVersion, *Path.ToString(), *OutAnimSig,
		FMath::Clamp(S->CacheResolution, 32, 256), FMath::Clamp(S->CacheFPS, 1, 120), FMath::Clamp(S->MaxFramesPerAsset, 8, 1024), S->bStableFraming ? 1 : 0, TileSize);
	const FTCHARToUTF8 Utf8(*OutKey);
	const uint64 Hash = CityHash64(Utf8.Get(), (uint32)Utf8.Length());
	FString Name = Path.GetAssetName();
	for (TCHAR& C : Name)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
		{
			C = TEXT('_');
		}
	}
	OutFileName = FString::Printf(TEXT("%s_%016llx.btlc"), *Name.Left(64), (unsigned long long)Hash);
	return true;
}

void FBongThumbCache::SetupGeometry(FBongCacheEntry& E, float Length) const
{
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	const int32 Res = FMath::Clamp(S->CacheResolution, 32, 256);
	const int32 Fps = FMath::Clamp(S->CacheFPS, 1, 120);
	const int32 MaxFit = FMath::Square(8192 / Res);
	const int32 MaxFrames = FMath::Min(FMath::Clamp(S->MaxFramesPerAsset, 8, 1024), MaxFit);
	E.Length = FMath::Max(Length, 0.f);
	const int32 Wanted = FMath::Max(1, FMath::CeilToInt(E.Length * Fps));
	E.NumFrames = FMath::Clamp(Wanted, 1, MaxFrames);
	// Long clips: fewer frames per second so the whole loop fits in the per-asset frame cap.
	E.Fps = (Wanted <= MaxFrames || E.Length <= UE_KINDA_SMALL_NUMBER) ? (float)Fps : (float)E.NumFrames / E.Length;
	E.CellW = Res;
	E.CellH = Res;
	E.Cols = FMath::CeilToInt(FMath::Sqrt((float)E.NumFrames));
	E.Rows = FMath::DivideAndRoundUp(E.NumFrames, E.Cols);
	E.Valid.Init(false, E.NumFrames);
	E.NumValid = 0;
	E.Bytes = (int64)E.Cols * E.CellW * E.Rows * E.CellH * 4;
}

bool FBongThumbCache::Reserve(int64 Bytes, const TSet<FSoftObjectPath>* Keep, double Now)
{
	const int64 Budget = (int64)FMath::Clamp(GetDefault<UBongThumbLiveSettings>()->CacheMemoryMB, 64, 8192) * 1024 * 1024;
	while (MemBytes + InFlightCpuBytes + Bytes > Budget)
	{
		const FSoftObjectPath* Victim = nullptr;
		double Oldest = TNumericLimits<double>::Max();
		for (const TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
		{
			if (Keep && Keep->Contains(Pair.Key))
			{
				continue; // on screen: evicting it would only thrash
			}
			if (Pair.Value->LastUse < Oldest)
			{
				Oldest = Pair.Value->LastUse;
				Victim = &Pair.Key;
			}
		}
		if (!Victim)
		{
			return false;
		}
		const FSoftObjectPath VictimPath = *Victim;
		DropEntry(VictimPath);
		++Evictions;
	}
	return true;
}

FBongAtlasRef FBongThumbCache::CreateAtlas(int32 W, int32 H, TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> InitialData)
{
	FBongAtlasRef Atlas = MakeShared<FBongAtlasGPU, ESPMode::ThreadSafe>();
	Atlas->Size = FIntPoint(W, H);
	ENQUEUE_RENDER_COMMAND(BongThumbLiveCreateAtlas)([Atlas, W, H, InitialData](FRHICommandListImmediate& RHICmdList)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("BongThumbLive.Atlas"), W, H, PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
			.SetInitialState(ERHIAccess::SRVMask)
			.SetClearValue(FClearValueBinding::Black);
		Atlas->Texture = RHICmdList.CreateTexture(Desc);
		if (Atlas->Texture && InitialData.IsValid() && InitialData->Num() == W * H * 4)
		{
			RHICmdList.UpdateTexture2D(Atlas->Texture, 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * 4, InitialData->GetData());
		}
	});
	return Atlas;
}

void FBongThumbCache::DropEntry(const FSoftObjectPath& Path)
{
	if (TUniquePtr<FBongCacheEntry>* Found = Entries.Find(Path))
	{
		MemBytes -= (*Found)->Bytes;
		Entries.Remove(Path);
	}
	if (FPushTile* Tile = PushTiles.Find(Path))
	{
		Tile->LastPushed = INDEX_NONE;
	}
	if (FillTargetPath == Path)
	{
		FillTargetPath.Reset();
	}
}

FBongCacheEntry* FBongThumbCache::Find(const FSoftObjectPath& Path)
{
	TUniquePtr<FBongCacheEntry>* Found = Entries.Find(Path);
	return Found ? Found->Get() : nullptr;
}

int32 FBongThumbCache::GetNumComplete() const
{
	int32 N = 0;
	for (const TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		N += Pair.Value->IsComplete() ? 1 : 0;
	}
	return N;
}

bool FBongThumbCache::IsDiskReady() const
{
	return bIndexReady || !GetDefault<UBongThumbLiveSettings>()->bEnableDiskCache;
}

FBongCacheEntry* FBongThumbCache::GetForAnim(UAnimSequenceBase* Anim, bool bCreate, double Now)
{
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	if (!Anim || !S->bUseFrameCache)
	{
		return nullptr;
	}
	const FSoftObjectPath Path(Anim);
	if (FBongCacheEntry* E = Find(Path))
	{
		E->Anim = Anim;
		return E;
	}
	if (!bCreate || LoadsInFlight.Contains(Path))
	{
		return nullptr;
	}
	UPackage* Package = Anim->GetPackage();
	const bool bDirty = Package && Package->IsDirty();
	FString Key, Sig, File;
	const bool bHasKey = !bDirty && Package && ComputeKey(Path, Package->GetFName(), Key, Sig, File);
	if (bHasKey && S->bEnableDiskCache)
	{
		if (!bIndexReady)
		{
			return nullptr; // wait for the startup scan: the frames may be on disk
		}
		if (Index.Contains(File) && !DiskFailed.Contains(Path))
		{
			return nullptr; // Tick loads it from disk (no render needed)
		}
	}
	TUniquePtr<FBongCacheEntry> New = MakeUnique<FBongCacheEntry>();
	SetupGeometry(*New, Anim->GetPlayLength());
	if (!Reserve(New->Bytes, &ActiveSet, Now))
	{
		++OverBudget;
		return nullptr;
	}
	New->Path = Path;
	New->PackageName = Package ? Package->GetFName() : NAME_None;
	New->Anim = Anim;
	New->KeyString = bHasKey ? Key : FString();
	New->AnimSig = Sig;
	New->FileName = bHasKey ? File : FString();
	New->bNoDisk = !bHasKey || !S->bEnableDiskCache;
	New->LastUse = Now;
	New->LastKeyCheck = Now;
	New->Atlas = CreateAtlas(New->Cols * New->CellW, New->Rows * New->CellH, nullptr);
	MemBytes += New->Bytes;
	if (bHasKey)
	{
		DeleteFilesForAsset(Path.ToString(), File); // older files of this animation (changed since) are stale
	}
	FBongCacheEntry* Result = New.Get();
	Entries.Add(Path, MoveTemp(New));
	return Result;
}

void FBongThumbCache::StoreFrame(FBongCacheEntry& E, int32 Frame, FRenderTarget* Src, const FIntRect& SrcRect, USkeletalMesh* Mesh)
{
	if (!E.Atlas.IsValid() || !Src || !E.Valid.IsValidIndex(Frame) || E.Valid[Frame])
	{
		return;
	}
	const FIntRect Cell = E.CellRect(Frame);
	FBongAtlasRef Atlas = E.Atlas;
	ENQUEUE_RENDER_COMMAND(BongThumbLiveStoreFrame)([Src, SrcRect, Atlas, Cell](FRHICommandListImmediate& RHICmdList)
	{
		FRHITexture* SrcTex = Src->GetRenderTargetTexture();
		if (!SrcTex || !Atlas->Texture)
		{
			return;
		}
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("BongThumbLiveStoreFrame"));
		FRDGTextureRef SrcRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SrcTex, TEXT("BongThumbLive.Thumbnail")));
		FRDGTextureRef DstRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Atlas->Texture, TEXT("BongThumbLive.Atlas")));
		AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(GMaxRHIFeatureLevel), SrcRDG, DstRDG, SrcRect.Min, SrcRect.Size(), Cell.Min, Cell.Size());
		GraphBuilder.Execute();
	});
	E.Valid[Frame] = true;
	++E.NumValid;
	E.LastShown = Frame;
	if (Mesh)
	{
		E.MeshPackage = Mesh->GetOutermost()->GetFName();
		if (const USkeleton* Skeleton = Mesh->GetSkeleton())
		{
			E.SkeletonPackage = Skeleton->GetOutermost()->GetFName();
		}
	}
	if (E.IsComplete())
	{
		MaybeStartSave(E);
	}
}

void FBongThumbCache::ShowFrame(FBongCacheEntry& E, int32 Frame, FCanvas* Canvas, FRenderTarget* Dst, const FIntRect& DstRect, double Now)
{
	if (!E.Atlas.IsValid() || !Dst || !E.Valid.IsValidIndex(Frame) || !E.Valid[Frame])
	{
		return;
	}
	if (Canvas)
	{
		Canvas->Flush_GameThread(); // the pool's clear must land before our copy
	}
	const FIntRect Cell = E.CellRect(Frame);
	FBongAtlasRef Atlas = E.Atlas;
	ENQUEUE_RENDER_COMMAND(BongThumbLiveShowFrame)([Dst, DstRect, Atlas, Cell](FRHICommandListImmediate& RHICmdList)
	{
		FRHITexture* DstTex = Dst->GetRenderTargetTexture();
		if (!DstTex || !Atlas->Texture)
		{
			return;
		}
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("BongThumbLiveShowFrame"));
		FRDGTextureRef SrcRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Atlas->Texture, TEXT("BongThumbLive.Atlas")));
		FRDGTextureRef DstRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DstTex, TEXT("BongThumbLive.Thumbnail")));
		AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(GMaxRHIFeatureLevel), SrcRDG, DstRDG, Cell.Min, Cell.Size(), DstRect.Min, DstRect.Size());
		GraphBuilder.Execute();
	});
	E.LastShown = Frame;
	E.LastUse = Now;
	++DrawHits;
}

void FBongThumbCache::Invalidate(const FSoftObjectPath& Path)
{
	DropEntry(Path);
	DiskFailed.Remove(Path);
	MissCounted.Remove(Path);
	NextDiskCheck.Remove(Path);
}

void FBongThumbCache::InvalidateObject(const UObject* Object)
{
	if (Object)
	{
		Invalidate(FSoftObjectPath(Object));
	}
}

void FBongThumbCache::ClearMemory()
{
	Entries.Reset();
	MemBytes = 0;
	FillTargetPath.Reset();
	for (TPair<FSoftObjectPath, FPushTile>& Pair : PushTiles)
	{
		Pair.Value.LastPushed = INDEX_NONE;
	}
	DiskFailed.Reset();
	MissCounted.Reset();
	NextDiskCheck.Reset();
}

void FBongThumbCache::TrimToVisible()
{
	TArray<FSoftObjectPath> Drop;
	for (const TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		if (!ActiveSet.Contains(Pair.Key))
		{
			Drop.Add(Pair.Key);
		}
	}
	for (const FSoftObjectPath& P : Drop)
	{
		DropEntry(P);
		++Evictions;
	}
}

void FBongThumbCache::ClearDisk()
{
	ClearMemory();
	const FString Dir = GetCacheDir();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.btlc")), true, false);
	int32 Deleted = 0;
	for (const FString& F : Files)
	{
		Deleted += IFileManager::Get().Delete(*(Dir / F), false, true, true) ? 1 : 0;
	}
	IFileManager::Get().Delete(*(Dir / BongCacheConst::IndexFileName), false, true, true);
	Index.Reset();
	FilesByAsset.Reset();
	bIndexDirty = false;
	// Saves still in flight would re-create files: forget them.
	Readbacks.Reset();
	InFlightCpuBytes = 0;
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: disk cache cleared (%d files deleted) in %s."), Deleted, *Dir);
}

// ------------------------------------------------------------------------------------------------------------------
// Disk: save (GPU readback -> worker compress + write), load (worker read + decompress -> one upload), index, LRU
// ------------------------------------------------------------------------------------------------------------------

void FBongThumbCache::MaybeStartSave(FBongCacheEntry& E)
{
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	if (!S->bEnableDiskCache || E.bNoDisk || E.bFromDisk || E.bSaveStarted || E.KeyString.IsEmpty() || E.FileName.IsEmpty() || !E.Atlas.IsValid())
	{
		return;
	}
	E.bSaveStarted = true;
	TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe> Job = MakeShared<FBongReadbackJob, ESPMode::ThreadSafe>();
	FBongFileHeader& H = Job->Header;
	H.Key = E.KeyString;
	H.AssetPath = E.Path.ToString();
	H.AnimSig = E.AnimSig;
	H.CellW = E.CellW;
	H.CellH = E.CellH;
	H.Cols = E.Cols;
	H.Rows = E.Rows;
	H.NumFrames = E.NumFrames;
	H.Fps = E.Fps;
	H.Length = E.Length;
	for (const FName Dep : { E.MeshPackage, E.SkeletonPackage })
	{
		if (Dep.IsNone())
		{
			continue;
		}
		const UPackage* DepPackage = FindPackage(nullptr, *Dep.ToString());
		const FString Sig = BongComputePackageSig(Dep.ToString());
		if ((DepPackage && DepPackage->IsDirty()) || Sig.IsEmpty())
		{
			E.bNoDisk = true; // rendered with unsaved mesh / skeleton data: memory only
			return;
		}
		H.DepPackages.Add(Dep.ToString());
		H.DepSigs.Add(Sig);
	}
	Job->Deps = BongJoinDeps(H);
	Job->FileName = E.FileName;
	Job->Atlas = E.Atlas;
	Job->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("BongThumbLive.Save"));
	InFlightCpuBytes += E.Bytes;
	ENQUEUE_RENDER_COMMAND(BongThumbLiveSaveReadback)([Job](FRHICommandListImmediate& RHICmdList)
	{
		if (!Job->Atlas->Texture)
		{
			Job->State = 3;
			return;
		}
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("BongThumbLiveSaveReadback"));
		FRDGTextureRef Tex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Job->Atlas->Texture, TEXT("BongThumbLive.Atlas")));
		AddEnqueueCopyPass(GraphBuilder, Job->Readback.Get(), Tex);
		GraphBuilder.Execute();
		Job->State = 1;
	});
	Readbacks.Add(Job);
}

void FBongThumbCache::PollReadbacks()
{
	if (Readbacks.IsEmpty())
	{
		return;
	}
	bool bAnyWaiting = false;
	const FString Dir = GetCacheDir();
	for (int32 i = Readbacks.Num() - 1; i >= 0; --i)
	{
		TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe> Job = Readbacks[i];
		const int32 State = Job->State.load();
		if (State == 1)
		{
			bAnyWaiting = true;
			continue;
		}
		if (State == 0)
		{
			continue;
		}
		Readbacks.RemoveAt(i);
		const int64 Bytes = (int64)Job->Atlas->Size.X * Job->Atlas->Size.Y * 4;
		if (State != 2)
		{
			InFlightCpuBytes = FMath::Max<int64>(0, InFlightCpuBytes - Bytes);
			++DiskWriteFails;
			continue;
		}
		InFlightCpuBytes = FMath::Max<int64>(0, InFlightCpuBytes - Bytes);
		TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
		S->Outstanding++;
		Async(EAsyncExecution::ThreadPool, [S, Job, Dir]()
		{
			FBongDiskWriteResult* Result = new FBongDiskWriteResult();
			Result->FileName = Job->FileName;
			Result->AssetPath = Job->Header.AssetPath;
			Result->AnimSig = Job->Header.AnimSig;
			Result->Deps = Job->Deps;
			FBongFileHeader H = Job->Header;
			const TArray<uint8>& Raw = Job->Pixels;
			const int64 Bound = FOodleDataCompression::CompressedBufferSizeNeeded(Raw.Num());
			TArray<uint8> Comp;
			Comp.SetNumUninitialized(Bound);
			const int64 CompSize = FOodleDataCompression::Compress(Comp.GetData(), Bound, Raw.GetData(), Raw.Num(),
				FOodleDataCompression::ECompressor::Kraken, FOodleDataCompression::ECompressionLevel::Fast);
			bool bOk = CompSize > 0 && !S->bShutdown.load();
			const FString Final = Dir / Job->FileName;
			const FString Tmp = Final + TEXT(".tmp");
			if (bOk)
			{
				H.RawSize = Raw.Num();
				H.CompSize = CompSize;
				TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Tmp));
				bOk = Writer.IsValid();
				if (bOk)
				{
					*Writer << H;
					Writer->Serialize(Comp.GetData(), CompSize);
					bOk = !Writer->IsError();
					bOk = Writer->Close() && bOk;
				}
				Writer.Reset();
				bOk = bOk && IFileManager::Get().Move(*Final, *Tmp, true, true);
				if (!bOk)
				{
					IFileManager::Get().Delete(*Tmp, false, true, true);
				}
			}
			Result->bOk = bOk;
			Result->Bytes = bOk ? IFileManager::Get().FileSize(*Final) : 0;
			Job->Pixels.Empty();
			S->Writes.Enqueue(Result);
			S->Outstanding--;
		});
	}
	if (bAnyWaiting)
	{
		TArray<TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe>> Waiting;
		for (const TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe>& Job : Readbacks)
		{
			if (Job->State.load() == 1)
			{
				Waiting.Add(Job);
			}
		}
		ENQUEUE_RENDER_COMMAND(BongThumbLivePollReadback)([Waiting](FRHICommandListImmediate& RHICmdList)
		{
			for (const TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe>& Job : Waiting)
			{
				if (Job->State.load() != 1 || !Job->Readback->IsReady())
				{
					continue;
				}
				const int32 W = Job->Atlas->Size.X;
				const int32 H = Job->Atlas->Size.Y;
				int32 PitchPixels = 0;
				int32 BufferHeight = 0;
				const uint8* Data = static_cast<const uint8*>(Job->Readback->Lock(PitchPixels, &BufferHeight));
				if (Data && PitchPixels >= W)
				{
					Job->Pixels.SetNumUninitialized(W * H * 4);
					for (int32 Y = 0; Y < H; ++Y)
					{
						FMemory::Memcpy(Job->Pixels.GetData() + (int64)Y * W * 4, Data + (int64)Y * PitchPixels * 4, W * 4);
					}
					Job->Readback->Unlock();
					Job->State = 2;
				}
				else
				{
					if (Data)
					{
						Job->Readback->Unlock();
					}
					Job->State = 3;
				}
			}
		});
	}
}

void FBongThumbCache::StartDiskLoad(const FSoftObjectPath& Path, const FString& FileName, const FString& Key)
{
	LoadsInFlight.Add(Path);
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
	const FString Full = GetCacheDir() / FileName;
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Full, Path, FileName, Key]()
	{
		const double T0 = FPlatformTime::Seconds();
		FBongDiskLoadResult* R = new FBongDiskLoadResult();
		R->Path = Path;
		R->FileName = FileName;
		R->Key = Key;
		TArray<uint8> Payload;
		if (S->bShutdown.load())
		{
			// nothing
		}
		else if (!BongReadHeader(Full, R->Header, &Payload) || R->Header.Key != Key)
		{
			R->bStale = true;
		}
		else if (!BongDepsStillValid(BongJoinDeps(R->Header)))
		{
			R->bStale = true; // preview mesh / skeleton changed since it was rendered
		}
		else
		{
			const FBongFileHeader& H = R->Header;
			const int64 Expected = (int64)H.Cols * H.CellW * H.Rows * H.CellH * 4;
			if (Expected == H.RawSize && H.NumFrames > 0 && H.NumFrames <= H.Cols * H.Rows)
			{
				R->Pixels = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
				R->Pixels->SetNumUninitialized(H.RawSize);
				R->bOk = FOodleDataCompression::Decompress(R->Pixels->GetData(), H.RawSize, Payload.GetData(), Payload.Num());
				if (!R->bOk)
				{
					R->Pixels.Reset();
					R->bStale = true;
				}
			}
			else
			{
				R->bStale = true;
			}
		}
		if (R->bStale)
		{
			IFileManager::Get().Delete(*Full, false, true, true);
		}
		R->DecodeMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		S->Loads.Enqueue(R);
		S->Outstanding--;
	});
}

void FBongThumbCache::ProcessAsyncResults(double Now)
{
	FBongScanResult* Scan = nullptr;
	while (Shared->Scans.Dequeue(Scan))
	{
		int64 Total = 0;
		for (const FBongScanIndexEntry& E : Scan->Entries)
		{
			FIndexEntry& I = Index.FindOrAdd(E.FileName);
			I.AssetPath = E.AssetPath;
			I.AnimSig = E.AnimSig;
			I.Bytes = E.Bytes;
			I.LastUse = E.LastUse;
			I.Deps = E.Deps;
			FilesByAsset.AddUnique(E.AssetPath, E.FileName);
			Total += E.Bytes;
		}
		bIndexReady = true;
		bIndexDirty = true;
		UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: disk cache ready: %d files, %.1f MB (deleted %d: %d missing assets, %d changed) in %.0f ms (%s)."),
			Scan->Entries.Num(), Total / (1024.0 * 1024.0), Scan->Deleted, Scan->Missing, Scan->Stale, Scan->Ms, *GetCacheDir());
		delete Scan;
		EnforceDiskLimit();
	}

	FBongDiskWriteResult* Write = nullptr;
	while (Shared->Writes.Dequeue(Write))
	{
		if (Write->bOk)
		{
			FIndexEntry& I = Index.FindOrAdd(Write->FileName);
			I.AssetPath = Write->AssetPath;
			I.AnimSig = Write->AnimSig;
			I.Bytes = Write->Bytes;
			I.LastUse = FDateTime::UtcNow().ToUnixTimestamp();
			I.Deps = Write->Deps;
			FilesByAsset.AddUnique(Write->AssetPath, Write->FileName);
			bIndexDirty = true;
			++DiskWrites;
			UE_LOG(LogBongThumbLive, Verbose, TEXT("BongThumbLive: saved %s (%.1f KB)."), *Write->FileName, Write->Bytes / 1024.0);
			EnforceDiskLimit();
		}
		else
		{
			++DiskWriteFails;
		}
		delete Write;
	}

	FBongDiskLoadResult* Load = nullptr;
	while (Shared->Loads.Dequeue(Load))
	{
		if (!Load->bOk)
		{
			LoadsInFlight.Remove(Load->Path);
			DiskFailed.Add(Load->Path);
			if (Load->bStale)
			{
				++DiskStale;
				if (Index.Remove(Load->FileName) > 0)
				{
					FilesByAsset.Remove(Load->Path.ToString(), Load->FileName);
					bIndexDirty = true;
				}
			}
			delete Load;
			continue;
		}
		PendingUploads.Add(Load);
	}

	// One upload per atlas (a few per frame): the decoded pixels become a GPU texture, the CPU copy is freed.
	int32 Uploads = 0;
	for (int32 i = 0; i < PendingUploads.Num() && Uploads < BongCacheConst::MaxUploadsPerTick;)
	{
		FBongDiskLoadResult* R = PendingUploads[i];
		const FBongFileHeader& H = R->Header;
		const int64 Bytes = (int64)H.Cols * H.CellW * H.Rows * H.CellH * 4;
		const bool bWanted = ActiveSet.Contains(R->Path) && !Entries.Contains(R->Path);
		if (bWanted && !Reserve(Bytes, &ActiveSet, Now))
		{
			++OverBudget;
		}
		else if (bWanted)
		{
			TUniquePtr<FBongCacheEntry> E = MakeUnique<FBongCacheEntry>();
			E->Path = R->Path;
			E->PackageName = FName(*R->Path.GetLongPackageName());
			E->Anim = Cast<UAnimSequenceBase>(R->Path.ResolveObject());
			E->KeyString = R->Key;
			E->FileName = R->FileName;
			E->AnimSig = H.AnimSig;
			E->CellW = H.CellW;
			E->CellH = H.CellH;
			E->Cols = H.Cols;
			E->Rows = H.Rows;
			E->NumFrames = H.NumFrames;
			E->Fps = H.Fps;
			E->Length = H.Length;
			E->Bytes = Bytes;
			E->Valid.Init(true, H.NumFrames);
			E->NumValid = H.NumFrames;
			E->bFromDisk = true;
			E->bSaveStarted = true;
			for (int32 d = 0; d < H.DepPackages.Num(); ++d)
			{
				(d == 0 ? E->MeshPackage : E->SkeletonPackage) = FName(*H.DepPackages[d]);
			}
			E->LastUse = Now;
			E->LastKeyCheck = Now;
			E->Atlas = CreateAtlas(H.Cols * H.CellW, H.Rows * H.CellH, R->Pixels);
			MemBytes += Bytes;
			Entries.Add(R->Path, MoveTemp(E));
			++DiskHits;
			DiskDecodeMsTotal += R->DecodeMs;
			++Uploads;
		}
		LoadsInFlight.Remove(R->Path);
		PendingUploads.RemoveAt(i);
		delete R; // the render command keeps the pixels alive until uploaded
	}
}

void FBongThumbCache::TouchIndex(FBongCacheEntry& E, double Now)
{
	if (E.FileName.IsEmpty() || Now - E.LastIndexTouch < 60.0)
	{
		return; // at most once per minute per entry
	}
	if (FIndexEntry* I = Index.Find(E.FileName))
	{
		E.LastIndexTouch = Now;
		I->LastUse = FDateTime::UtcNow().ToUnixTimestamp();
		bIndexDirty = true;
	}
}

void FBongThumbCache::EnforceDiskLimit()
{
	const int64 Max = (int64)FMath::Clamp(GetDefault<UBongThumbLiveSettings>()->MaxDiskCacheMB, 64, 1024 * 1024) * 1024 * 1024;
	int64 Total = 0;
	for (const TPair<FString, FIndexEntry>& Pair : Index)
	{
		Total += Pair.Value.Bytes;
	}
	if (Total <= Max)
	{
		return;
	}
	TArray<FString> Order;
	Index.GetKeys(Order);
	Order.Sort([this](const FString& A, const FString& B) { return Index[A].LastUse < Index[B].LastUse; });
	int32 Deleted = 0;
	for (const FString& File : Order)
	{
		if (Total <= Max)
		{
			break;
		}
		const FIndexEntry I = Index[File];
		if (ActiveSet.Contains(FSoftObjectPath(I.AssetPath)))
		{
			continue;
		}
		DeleteFileAsync(File);
		Total -= I.Bytes;
		Index.Remove(File);
		FilesByAsset.Remove(I.AssetPath, File);
		++Deleted;
	}
	bIndexDirty = true;
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: disk cache over %d MB: deleted %d least recently shown files (now %.1f MB)."),
		GetDefault<UBongThumbLiveSettings>()->MaxDiskCacheMB, Deleted, Total / (1024.0 * 1024.0));
}

void FBongThumbCache::DeleteFileAsync(const FString& FileName)
{
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
	const FString Full = GetCacheDir() / FileName;
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Full]()
	{
		IFileManager::Get().Delete(*Full, false, true, true);
		S->Outstanding--;
	});
}

void FBongThumbCache::DeleteFilesForAsset(const FString& AssetPath, const FString& KeepFile)
{
	TArray<FString> Files;
	FilesByAsset.MultiFind(AssetPath, Files);
	for (const FString& File : Files)
	{
		if (File == KeepFile)
		{
			continue;
		}
		// Same animation, other key: delete only when the asset itself changed (other settings keys stay valid).
		const FIndexEntry* I = Index.Find(File);
		const FString CurrentSig = BongComputePackageSig(FSoftObjectPath(AssetPath).GetLongPackageName());
		if (I && !CurrentSig.IsEmpty() && I->AnimSig == CurrentSig && KeepFile != TEXT("*"))
		{
			continue;
		}
		DeleteFileAsync(File);
		Index.Remove(File);
		FilesByAsset.Remove(AssetPath, File);
		bIndexDirty = true;
	}
}

void FBongThumbCache::OnAssetRemoved(const FAssetData& Asset)
{
	const FSoftObjectPath Path = Asset.GetSoftObjectPath();
	Invalidate(Path);
	DeleteFilesForAsset(Path.ToString(), TEXT("*"));
	AnimInfo.Remove(Path);
	NotAnim.Remove(Path);
}

void FBongThumbCache::OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
{
	const FSoftObjectPath OldPath(OldObjectPath);
	Invalidate(OldPath);
	DeleteFilesForAsset(OldPath.ToString(), TEXT("*")); // the key contains the path: re-rendered under the new name
	AnimInfo.Remove(OldPath);
	NotAnim.Remove(OldPath);
	NotAnim.Remove(Asset.GetSoftObjectPath());
}

void FBongThumbCache::WriteIndex(bool bSync)
{
	FString Text;
	Text.Reserve(Index.Num() * 160);
	for (const TPair<FString, FIndexEntry>& Pair : Index)
	{
		Text += FString::Printf(TEXT("%s\t%s\t%s\t%lld\t%lld\t%s\n"), *Pair.Key, *Pair.Value.AssetPath, *Pair.Value.AnimSig, Pair.Value.Bytes, Pair.Value.LastUse, *Pair.Value.Deps);
	}
	bIndexDirty = false;
	LastIndexWrite = FPlatformTime::Seconds();
	const FString Final = GetCacheDir() / BongCacheConst::IndexFileName;
	if (bSync)
	{
		FFileHelper::SaveStringToFile(Text, *Final, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return;
	}
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Text = MoveTemp(Text), Final]()
	{
		const FString Tmp = Final + TEXT(".tmp");
		if (FFileHelper::SaveStringToFile(Text, *Tmp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			IFileManager::Get().Move(*Final, *Tmp, true, true);
		}
		S->Outstanding--;
	});
}

// ------------------------------------------------------------------------------------------------------------------
// Tick: disk requests, push (playback), fill (rendering of missing frames)
// ------------------------------------------------------------------------------------------------------------------

bool FBongThumbCache::IsAnimPath(const FSoftObjectPath& Path, FAssetData* OutData)
{
	if (Path.IsNull() || NotAnim.Contains(Path))
	{
		return false;
	}
	if (const FAssetData* Found = AnimInfo.Find(Path))
	{
		if (OutData)
		{
			*OutData = *Found;
		}
		return true;
	}
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData Data = AR.GetAssetByObjectPath(Path);
	const UClass* Class = Data.IsValid() ? Data.GetClass() : nullptr;
	if (!Class || !Class->IsChildOf(UAnimSequenceBase::StaticClass()))
	{
		if (!AR.IsLoadingAssets())
		{
			NotAnim.Add(Path);
		}
		return false;
	}
	if (AnimInfo.Num() > 32768)
	{
		AnimInfo.Reset();
	}
	AnimInfo.Add(Path, Data);
	if (OutData)
	{
		*OutData = Data;
	}
	return true;
}

bool FBongThumbCache::CanPlayWithoutLoading(const FSoftObjectPath& Path, FName PackageName)
{
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	if (!S->bUseFrameCache)
	{
		return false;
	}
	if (const FBongCacheEntry* E = Find(Path))
	{
		if (E->IsComplete())
		{
			return true;
		}
	}
	if (!S->bEnableDiskCache || !bIndexReady || DiskFailed.Contains(Path))
	{
		return false;
	}
	FString Key, Sig, File;
	return ComputeKey(Path, PackageName, Key, Sig, File) && Index.Contains(File);
}

void FBongThumbCache::OnPoolRendered(const FSoftObjectPath& Path)
{
	if (FPushTile* Tile = PushTiles.Find(Path))
	{
		Tile->LastPushed = INDEX_NONE;
	}
}

void FBongThumbCache::Tick(const FTickParams& Params)
{
	const double Now = Params.Now;
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	ProcessAsyncResults(Now);
	PollReadbacks();
	if (!S->bUseFrameCache)
	{
		if (Entries.Num() > 0)
		{
			ClearMemory();
		}
		PushTiles.Reset();
		ActiveSet.Reset();
		return;
	}
	ActiveSet = TSet<FSoftObjectPath>(Params.ActivePaths);

	// Validate keys (settings changed, asset saved / modified) at most once per second per entry.
	TArray<FSoftObjectPath> Stale;
	for (TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		FBongCacheEntry& E = *Pair.Value;
		if (Now - E.LastKeyCheck < 1.0)
		{
			continue;
		}
		E.LastKeyCheck = Now;
		const UPackage* Package = FindPackage(nullptr, *E.PackageName.ToString());
		const bool bDirty = Package && Package->IsDirty();
		FString Key, Sig, File;
		const bool bHasKey = !bDirty && ComputeKey(E.Path, E.PackageName, Key, Sig, File);
		if (bDirty)
		{
			if (!E.KeyString.IsEmpty())
			{
				Stale.Add(Pair.Key); // modified in memory: re-render (not a "use" for the LRU)
			}
			continue;
		}
		if (bHasKey ? (Key != E.KeyString) : !E.KeyString.IsEmpty())
		{
			Stale.Add(Pair.Key);
			continue;
		}
		if (!bHasKey && E.KeyString.IsEmpty() && Package && !Package->IsDirty() && !E.AnimSig.IsEmpty())
		{
			Stale.Add(Pair.Key); // was dirty, now saved: re-render once so it can go to disk
		}
	}
	for (const FSoftObjectPath& P : Stale)
	{
		DropEntry(P);
		DiskFailed.Remove(P);
	}

	// Disk loads for on-screen animations without an entry (they do not need to be loaded as assets).
	if (S->bEnableDiskCache && bIndexReady)
	{
		for (const FSoftObjectPath& Path : Params.ActivePaths)
		{
			if (LoadsInFlight.Num() >= BongCacheConst::MaxConcurrentLoads)
			{
				break;
			}
			if (Entries.Contains(Path) || LoadsInFlight.Contains(Path) || DiskFailed.Contains(Path))
			{
				continue;
			}
			if (const double* Next = NextDiskCheck.Find(Path))
			{
				if (Now < *Next)
				{
					continue;
				}
			}
			NextDiskCheck.Add(Path, Now + 1.0);
			const UPackage* Package = FindPackage(nullptr, *Path.GetLongPackageName());
			if (Package && Package->IsDirty())
			{
				continue;
			}
			FString Key, Sig, File;
			if (!IsAnimPath(Path, nullptr) || !ComputeKey(Path, FName(*Path.GetLongPackageName()), Key, Sig, File))
			{
				continue;
			}
			if (Index.Contains(File))
			{
				NextDiskCheck.Add(Path, Now + 5.0); // over budget / failed upload: do not re-read every frame
				StartDiskLoad(Path, File, Key);
			}
			else if (!MissCounted.Contains(Path))
			{
				MissCounted.Add(Path);
				++DiskMisses;
			}
		}
	}

	Reserve(0, &ActiveSet, Now); // settings lowered the budget
	Push(Params, ActiveSet);
	Fill(Params);

	for (auto It = PushTiles.CreateIterator(); It; ++It)
	{
		if (Now - It.Value().LastActive > 1.0)
		{
			It.RemoveCurrent();
		}
	}
	if (bIndexReady && bIndexDirty && Now - LastIndexWrite > 10.0)
	{
		WriteIndex(false);
	}
	if (bIndexReady && Now - LastLruCheck > 30.0)
	{
		LastLruCheck = Now;
		EnforceDiskLimit();
	}
}

void FBongThumbCache::Push(const FTickParams& Params, const TSet<FSoftObjectPath>& Active)
{
	struct FCopy
	{
		FBongAtlasRef Atlas;
		FIntRect Cell;
		FSlateTexture2DRHIRef* Dst = nullptr;
	};
	TArray<FCopy> Copies;
	const double Now = Params.Now;
	for (const FSoftObjectPath& Path : Params.ActivePaths)
	{
		FBongCacheEntry* E = Find(Path);
		if (!E || E->NumValid <= 0 || !E->Atlas.IsValid() || !Params.Pool.IsValid())
		{
			continue;
		}
		FPushTile& Tile = PushTiles.FindOrAdd(Path);
		Tile.LastActive = Now;
		if (!Tile.Thumb.IsValid())
		{
			FAssetData Data;
			if (!IsAnimPath(Path, &Data))
			{
				continue;
			}
			Tile.Thumb = MakeShared<FAssetThumbnail>(Data, TileSize, TileSize, Params.Pool);
		}
		const bool bHovered = Path == Params.Hovered;
		int32 Frame = E->FrameIndexAt((float)BongLoopTime(E->Length, bHovered));
		if (!E->Valid.IsValidIndex(Frame) || !E->Valid[Frame])
		{
			Frame = E->NearestValid(Frame);
		}
		if (Frame == INDEX_NONE || Frame == Tile.LastPushed)
		{
			continue;
		}
		FSlateTexture2DRHIRef* Dst = static_cast<FSlateTexture2DRHIRef*>(Tile.Thumb->GetViewportRenderTargetTexture());
		if (!Dst)
		{
			continue; // the pool has not produced the tile texture yet
		}
		Tile.LastPushed = Frame;
		E->LastShown = Frame;
		E->LastUse = Now;
		TouchIndex(*E, Now);
		Copies.Add({ E->Atlas, E->CellRect(Frame), Dst });
	}
	if (Copies.IsEmpty())
	{
		return;
	}
	PushedFrames += Copies.Num();
	ENQUEUE_RENDER_COMMAND(BongThumbLivePush)([Copies = MoveTemp(Copies)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("BongThumbLivePush"));
		TMap<FRHITexture*, FRDGTextureRef> Registered;
		const FScreenPassViewInfo ViewInfo(GMaxRHIFeatureLevel);
		for (const FCopy& Copy : Copies)
		{
			FRHITexture* DstTex = Copy.Dst->GetTypedResource().GetReference();
			if (!DstTex || !Copy.Atlas->Texture || DstTex->GetFormat() != PF_B8G8R8A8)
			{
				continue;
			}
			FRDGTextureRef Src = nullptr;
			BongRegisterRT(GraphBuilder, Registered, Copy.Atlas->Texture, Src, TEXT("BongThumbLive.Atlas"));
			const FIntPoint DstSize = DstTex->GetDesc().Extent;
			if (EnumHasAnyFlags(DstTex->GetFlags(), ETextureCreateFlags::RenderTargetable))
			{
				FRDGTextureRef Dst = nullptr;
				BongRegisterRT(GraphBuilder, Registered, DstTex, Dst, TEXT("BongThumbLive.Tile"));
				AddDrawTexturePass(GraphBuilder, ViewInfo, Src, Dst, Copy.Cell.Min, Copy.Cell.Size(), FIntPoint::ZeroValue, DstSize);
			}
			else
			{
				// Thumbnail loaded from the package (plain texture): draw scaled into a temp target, then copy.
				FRDGTextureRef Temp = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(DstSize, PF_B8G8R8A8, FClearValueBinding::Black,
					ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource), TEXT("BongThumbLive.PushTemp"));
				AddDrawTexturePass(GraphBuilder, ViewInfo, Src, Temp, Copy.Cell.Min, Copy.Cell.Size(), FIntPoint::ZeroValue, DstSize);
				FRDGTextureRef Dst = nullptr;
				if (FRDGTextureRef* Found = Registered.Find(DstTex))
				{
					Dst = *Found;
				}
				else
				{
					Dst = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DstTex, TEXT("BongThumbLive.TileTex")));
					Registered.Add(DstTex, Dst);
				}
				AddCopyTexturePass(GraphBuilder, Temp, Dst);
			}
		}
		GraphBuilder.Execute();
	});
}

void FBongThumbCache::Fill(const FTickParams& Params)
{
	if (!Params.bAllowFill || FPlatformTime::Seconds() >= Params.FillDeadline)
	{
		return;
	}
	TArray<FSoftObjectPath, TInlineAllocator<64>> Order;
	if (!Params.Hovered.IsNull() && ActiveSet.Contains(Params.Hovered))
	{
		Order.Add(Params.Hovered);
	}
	if (!FillTargetPath.IsNull() && ActiveSet.Contains(FillTargetPath))
	{
		Order.AddUnique(FillTargetPath); // finish one animation before starting the next
	}
	for (const FSoftObjectPath& P : Params.ActivePaths)
	{
		Order.AddUnique(P);
	}
	int32 Attempts = 0;
	for (const FSoftObjectPath& Path : Order)
	{
		if (Attempts >= 3 || FPlatformTime::Seconds() >= Params.FillDeadline)
		{
			break;
		}
		if (const FBongCacheEntry* Existing = Find(Path))
		{
			if (Existing->IsComplete())
			{
				continue;
			}
		}
		else if (LoadsInFlight.Contains(Path))
		{
			continue;
		}
		UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Path.ResolveObject());
		if (!Anim || !FBongThumbLiveModule::IsRefreshableNow(Anim))
		{
			continue;
		}
		FBongCacheEntry* E = GetForAnim(Anim, true, Params.Now);
		if (!E)
		{
			continue;
		}
		++Attempts;
		const int32 Start = E->FrameIndexAt((float)BongLoopTime(E->Length, Path == Params.Hovered));
		const int32 Done = UBongThumbLiveRenderer::FillFrames(Anim, *E, Params.FillDeadline, Start);
		FillTargetPath = E->IsComplete() ? FSoftObjectPath() : Path;
		if (Done > 0)
		{
			Renders += Done;
		}
	}
}

// ------------------------------------------------------------------------------------------------------------------
// Stats / debug
// ------------------------------------------------------------------------------------------------------------------

FString FBongThumbCache::GetStats() const
{
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	int64 DiskBytes = 0;
	for (const TPair<FString, FIndexEntry>& Pair : Index)
	{
		DiskBytes += Pair.Value.Bytes;
	}
	int32 Complete = 0;
	int64 Frames = 0;
	int32 FromDisk = 0;
	for (const TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		Complete += Pair.Value->IsComplete() ? 1 : 0;
		Frames += Pair.Value->NumValid;
		FromDisk += Pair.Value->bFromDisk ? 1 : 0;
	}
	return FString::Printf(TEXT("memory cache: %s entries=%d (complete %d, from disk %d) frames=%lld GPU=%.1f MB (+cpu in flight %.1f MB) / %d MB, evictions=%lld, playing tiles=%d, pushed frames=%lld, draw hits=%lld, renders=%lld, notStored(compiling)=%lld, overBudget=%lld\n")
		TEXT("disk cache: %s %s files=%d %.1f / %d MB, hits=%lld misses=%lld stale=%lld writes=%lld writeFails=%lld loading=%d avgDecode=%.1f ms, folder=%s"),
		S->bUseFrameCache ? TEXT("on") : TEXT("OFF"), Entries.Num(), Complete, FromDisk, Frames, MemBytes / (1024.0 * 1024.0), InFlightCpuBytes / (1024.0 * 1024.0),
		S->CacheMemoryMB, Evictions, PushTiles.Num(), PushedFrames, DrawHits, Renders, NotStoredBusy, OverBudget,
		S->bEnableDiskCache ? TEXT("on") : TEXT("OFF"), bIndexReady ? TEXT("ready") : TEXT("scanning"), Index.Num(), DiskBytes / (1024.0 * 1024.0), S->MaxDiskCacheMB,
		DiskHits, DiskMisses, DiskStale, DiskWrites, DiskWriteFails, LoadsInFlight.Num() + PendingUploads.Num(),
		DiskHits > 0 ? DiskDecodeMsTotal / DiskHits : 0.0, *GetCacheDir());
}

FString FBongThumbCache::GetDiskSummary() const
{
	if (!bIndexReady)
	{
		return FString(TEXT("확인 중..."));
	}
	int64 Bytes = 0;
	for (const TPair<FString, FIndexEntry>& Pair : Index)
	{
		Bytes += Pair.Value.Bytes;
	}
	return FString::Printf(TEXT("파일 %d개, %.1f MB / %d MB"), Index.Num(), Bytes / (1024.0 * 1024.0), GetDefault<UBongThumbLiveSettings>()->MaxDiskCacheMB);
}

void FBongThumbCache::Dump(const FString& Dir)
{
	IFileManager::Get().MakeDirectory(*Dir, true);
	int32 Dumped = 0;
	for (const TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		const FBongCacheEntry& E = *Pair.Value;
		if (!E.IsComplete() || !E.Atlas.IsValid() || Dumped >= 4)
		{
			continue;
		}
		++Dumped;
		TArray<FColor> Pixels;
		FBongAtlasRef Atlas = E.Atlas;
		ENQUEUE_RENDER_COMMAND(BongThumbLiveDump)([Atlas, &Pixels](FRHICommandListImmediate& RHICmdList)
		{
			if (Atlas->Texture)
			{
				RHICmdList.ReadSurfaceData(Atlas->Texture, FIntRect(FIntPoint::ZeroValue, Atlas->Size), Pixels, FReadSurfaceDataFlags());
			}
		});
		FlushRenderingCommands();
		if (Pixels.Num() != Atlas->Size.X * Atlas->Size.Y)
		{
			continue;
		}
		FString Hashes;
		TSet<uint64> Unique;
		for (int32 F = 0; F < E.NumFrames; ++F)
		{
			const FIntRect R = E.CellRect(F);
			TArray<FColor> Cell;
			Cell.Reserve(E.CellW * E.CellH);
			for (int32 Y = R.Min.Y; Y < R.Max.Y; ++Y)
			{
				Cell.Append(&Pixels[Y * Atlas->Size.X + R.Min.X], E.CellW);
			}
			const uint64 H = CityHash64((const char*)Cell.GetData(), Cell.Num() * sizeof(FColor));
			Unique.Add(H);
			if (F == 0 || F == E.NumFrames / 3 || F == (2 * E.NumFrames) / 3)
			{
				Hashes += FString::Printf(TEXT(" f%d=%016llx"), F, (unsigned long long)H);
				for (FColor& C : Cell)
				{
					C.A = 255;
				}
				TArray64<uint8> Png;
				FImageUtils::PNGCompressImageArray(E.CellW, E.CellH, TArrayView64<const FColor>(Cell.GetData(), Cell.Num()), Png);
				FFileHelper::SaveArrayToFile(Png, *(Dir / FString::Printf(TEXT("%s_f%03d.png"), *E.Path.GetAssetName(), F)));
			}
		}
		UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive.Dump: %s frames=%d fps=%.1f cell=%dx%d atlas=%dx%d fromDisk=%d unique frames=%d%s"),
			*E.Path.ToString(), E.NumFrames, E.Fps, E.CellW, E.CellH, Atlas->Size.X, Atlas->Size.Y, E.bFromDisk ? 1 : 0, Unique.Num(), *Hashes);
	}
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive.Dump: %d entries written to %s"), Dumped, *Dir);
}

// ------------------------------------------------------------------------------------------------------------------
// Owned packages (loaded by the plugin) -> release
// ------------------------------------------------------------------------------------------------------------------

namespace BongOwned
{
	struct FRoot
	{
		FInfo Info;
		TArray<FName> Packages;
	};
	static TMap<FName, FRoot> GRoots;
	static bool GGCPending = false;
	static bool GGCForce = false;
	static double GLastGC = 0.0;

	void Track(FName Root, const FSoftObjectPath& AssetPath, bool bMesh)
	{
		const double Now = FPlatformTime::Seconds();
		if (FRoot* Existing = GRoots.Find(Root))
		{
			Existing->Info.LastUse = Now;
			return;
		}
		if (Root.IsNone() || FindPackage(nullptr, *Root.ToString()))
		{
			return; // already in memory: loaded by the user / something else, not ours to release
		}
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FName> Queue;
		TSet<FName> Seen;
		Queue.Add(Root);
		Seen.Add(Root);
		for (int32 i = 0; i < Queue.Num() && Seen.Num() < 4096; ++i)
		{
			TArray<FName> Deps;
			AR.GetDependencies(Queue[i], Deps, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);
			for (const FName Dep : Deps)
			{
				if (Seen.Contains(Dep))
				{
					continue;
				}
				const FString Name = Dep.ToString();
				if (Name.StartsWith(TEXT("/Script/")) || Name.StartsWith(TEXT("/Engine/")) || Name.StartsWith(TEXT("/Temp/")))
				{
					continue;
				}
				Seen.Add(Dep);
				Queue.Add(Dep);
			}
		}
		FRoot& New = GRoots.Add(Root);
		New.Info.Root = Root;
		New.Info.AssetPath = AssetPath;
		New.Info.bMesh = bMesh;
		New.Info.TrackedAt = Now;
		New.Info.LastUse = Now;
		for (const FName P : Queue)
		{
			if (!FindPackage(nullptr, *P.ToString()))
			{
				New.Packages.Add(P);
			}
		}
	}

	void Touch(FName Root, double Now)
	{
		if (FRoot* Found = GRoots.Find(Root))
		{
			Found->Info.LastUse = Now;
		}
	}

	static int32 ReleaseRoot(const FRoot& Root)
	{
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		int32 Released = 0;
		for (const FName PackageName : Root.Packages)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName.ToString());
			if (!Package || Package->IsDirty() || Package == GetTransientPackage())
			{
				continue;
			}
			TArray<UObject*> Objects;
			GetObjectsWithPackage(Package, Objects, true);
			bool bKeep = false;
			for (UObject* Object : Objects)
			{
				if (Object->IsA<UWorld>() || (Editors && Object->IsAsset() && Editors->FindEditorForAsset(Object, false)))
				{
					bKeep = true; // a level, or open in an editor: leave alone
					break;
				}
			}
			if (bKeep)
			{
				continue;
			}
			for (UObject* Object : Objects)
			{
				Object->ClearFlags(RF_Standalone);
			}
			++Released;
		}
		return Released;
	}

	int32 ReleaseIf(TFunctionRef<bool(const FInfo&)> ShouldRelease)
	{
		int32 Roots = 0;
		for (auto It = GRoots.CreateIterator(); It; ++It)
		{
			if (ShouldRelease(It.Value().Info))
			{
				ReleaseRoot(It.Value());
				It.RemoveCurrent();
				++Roots;
			}
		}
		if (Roots > 0)
		{
			GGCPending = true;
		}
		return Roots;
	}

	void ReleaseAll()
	{
		ReleaseIf([](const FInfo&) { return true; });
	}

	int32 Num(bool bMesh)
	{
		int32 N = 0;
		for (const TPair<FName, FRoot>& Pair : GRoots)
		{
			N += Pair.Value.Info.bMesh == bMesh ? 1 : 0;
		}
		return N;
	}

	void RequestGC(bool bForce)
	{
		GGCPending = true;
		GGCForce |= bForce;
	}

	void TickGC(double Now)
	{
		if (!GGCPending || !GEngine || (!GGCForce && Now - GLastGC < 15.0))
		{
			return;
		}
		if (IsAsyncLoading() && !GGCForce)
		{
			return;
		}
		// v2.2: the purge is incremental (spread over frames) unless memory is low.
		const bool bFullPurge = GGCForce;
		GGCPending = false;
		GGCForce = false;
		GLastGC = Now;
		GEngine->ForceGarbageCollection(bFullPurge);
	}
}
