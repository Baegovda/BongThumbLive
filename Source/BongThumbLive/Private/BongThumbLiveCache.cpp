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
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
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
#include <limits>
#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"

namespace BongCacheConst
{
	static constexpr uint32 Magic = 0x434C5442; // "BTLC"
	static constexpr int32 FileVersion = 1;
	/** Bump when the rendering changes in a way that makes old cached pictures wrong. */
	static constexpr int32 KeyVersion = 2; // corrected sampling for clips whose effective cache FPS is below one
	static constexpr int32 MaxConcurrentLoads = 4;
	static constexpr int32 MaxUploadsPerTick = 2;
	static const TCHAR* IndexFileName = TEXT("index.txt");
}

/** Byte leases survive entry eviction, worker jobs and render-command uploads. */
struct FBongMemoryAccount : TSharedFromThis<FBongMemoryAccount, ESPMode::ThreadSafe>
{
	mutable FCriticalSection Mutex;
	int64 GPU = 0;
	int64 CPU = 0;
	int64 Total() const { FScopeLock Lock(&Mutex); return GPU + CPU; }
	int64 Cpu() const { FScopeLock Lock(&Mutex); return CPU; }
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> Acquire(int64 Bytes, int64 Budget, bool bGPU);
};

struct FBongMemoryLease
{
	TSharedPtr<FBongMemoryAccount, ESPMode::ThreadSafe> Account;
	int64 Bytes;
	bool bGPU;
	FBongMemoryLease(TSharedPtr<FBongMemoryAccount, ESPMode::ThreadSafe> InAccount, int64 InBytes, bool bInGPU)
		: Account(MoveTemp(InAccount)), Bytes(InBytes), bGPU(bInGPU) {}
	~FBongMemoryLease()
	{
		FScopeLock Lock(&Account->Mutex);
		(bGPU ? Account->GPU : Account->CPU) -= Bytes;
	}
};

TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> FBongMemoryAccount::Acquire(int64 Bytes, int64 Budget, bool bGPU)
{
	{
		FScopeLock Lock(&Mutex);
		if (Bytes <= 0 || Bytes > Budget || GPU + CPU > Budget - Bytes) { return nullptr; }
		(bGPU ? GPU : CPU) += Bytes;
	}
	return MakeShared<FBongMemoryLease, ESPMode::ThreadSafe>(AsShared(), Bytes, bGPU);
}

struct FBongJobStamp
{
	uint64 Generation = 0;
	uint64 AssetGeneration = 0;
};

/** Validate length prefixes before UE's FString/TArray serializers allocate memory. Format remains v1. */
static void BongSerializeString(FArchive& Ar, FString& Value)
{
	if (Ar.IsError()) { return; }
	if (Ar.IsLoading())
	{
		const int64 Pos = Ar.Tell();
		int32 Count = 0;
		Ar << Count;
		const int64 Chars = Count < 0 ? -(int64)Count : Count;
		const int64 Bytes = Chars * (Count < 0 ? 2 : 1);
		if (Ar.IsError() || Chars > 4096 || Bytes > Ar.TotalSize() - Ar.Tell()) { Ar.SetError(); return; }
		Ar.Seek(Pos);
	}
	Ar << Value;
}

static void BongSerializeDeps(FArchive& Ar, TArray<FString>& Values)
{
	if (Ar.IsError()) { return; }
	int32 Count = Values.Num();
	Ar << Count;
	if (Count < 0 || Count > 2 || Ar.IsError()) { Ar.SetError(); return; }
	if (Ar.IsLoading()) { Values.SetNum(Count); }
	for (FString& Value : Values) { BongSerializeString(Ar, Value); }
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
		BongSerializeString(Ar, H.Key);
		BongSerializeString(Ar, H.AssetPath);
		BongSerializeString(Ar, H.AnimSig);
		if (Ar.IsError()) { return Ar; }
		Ar << H.CellW << H.CellH << H.Cols << H.Rows << H.NumFrames << H.Fps << H.Length;
		BongSerializeDeps(Ar, H.DepPackages);
		BongSerializeDeps(Ar, H.DepSigs);
		if (!Ar.IsError()) { Ar << H.RawSize << H.CompSize; }
		return Ar;
	}
};

struct FBongDiskLoadResult
{
	FBongJobStamp Stamp;
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> Memory;
	FSoftObjectPath Path;
	FString FileName;
	FString Key;
	bool bOk = false;
	bool bStale = false;
	bool bOverBudget = false;
	FBongFileHeader Header;
	TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> Pixels;
	double DecodeMs = 0.0;
};

struct FBongDiskWriteResult
{
	FBongJobStamp Stamp;
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
	FBongJobStamp Stamp;
	TArray<FBongScanIndexEntry> Entries;
	int32 Deleted = 0;
	int32 Missing = 0;
	int32 Stale = 0;
	double Ms = 0.0;
};

struct FBongAsyncShared
{
	TSharedPtr<FBongMemoryAccount, ESPMode::ThreadSafe> Memory = MakeShared<FBongMemoryAccount, ESPMode::ThreadSafe>();
	FCriticalSection FileMutex;
	uint64 Generation = 1;
	TMap<FSoftObjectPath, uint64> AssetGenerations;
	TMap<FString, uint64> FileRevisions;
	std::atomic<int32> Saves{0};
	TQueue<FBongDiskLoadResult*, EQueueMode::Mpsc> Loads;
	TQueue<FBongDiskWriteResult*, EQueueMode::Mpsc> Writes;
	TQueue<FBongScanResult*, EQueueMode::Mpsc> Scans;
	std::atomic<int32> Outstanding{ 0 };
	std::atomic<bool> bShutdown{ false };
	FBongJobStamp Stamp(const FSoftObjectPath& Path)
	{
		FScopeLock Lock(&FileMutex); return {Generation, AssetGenerations.FindRef(Path)};
	}
	bool CurrentLocked(const FSoftObjectPath& Path, FBongJobStamp S) const
	{
		return !bShutdown && S.Generation == Generation && S.AssetGeneration == AssetGenerations.FindRef(Path);
	}
	bool Current(const FSoftObjectPath& Path, FBongJobStamp S)
	{
		FScopeLock Lock(&FileMutex); return CurrentLocked(Path, S);
	}
	void Invalidate(const FSoftObjectPath& Path)
	{
		FScopeLock Lock(&FileMutex); ++AssetGenerations.FindOrAdd(Path);
	}
	void InvalidateAll()
	{
		FScopeLock Lock(&FileMutex); ++Generation; AssetGenerations.Reset();
	}
	uint64 NextFileRevision(const FString& File)
	{
		FScopeLock Lock(&FileMutex); return ++FileRevisions.FindOrAdd(File);
	}
	uint64 FileRevision(const FString& File)
	{
		FScopeLock Lock(&FileMutex); return FileRevisions.FindRef(File);
	}
	bool Commit(const FSoftObjectPath& Path, FBongJobStamp S, const FString& File, uint64 Revision, TFunctionRef<bool()> Work)
	{
		FScopeLock Lock(&FileMutex);
		return CurrentLocked(Path, S) && FileRevisions.FindRef(File) == Revision && Work();
	}
};

struct FBongReadbackJob
{
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> Owner;
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> Memory;
	FBongJobStamp Stamp;
	uint64 FileRevision = 0;
	~FBongReadbackJob() { if (Owner) { --Owner->Saves; } }
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	FBongAtlasRef Atlas;
	FBongFileHeader Header;
	FString FileName;
	FString Deps;
	TArray<uint8> Pixels;
	/** 0 = created, 1 = copy enqueued, 2 = pixels ready, 3 = failed */
	std::atomic<int32> State{ 0 };
};

static bool BongValidateHeader(const FBongFileHeader& H)
{
	if (H.CellW < 32 || H.CellW > 256 || H.CellH < 32 || H.CellH > 256 || H.Cols < 1 || H.Cols > 32 || H.Rows < 1 || H.Rows > 32
		|| H.NumFrames < 1 || H.NumFrames > 1024 || H.NumFrames > H.Cols * H.Rows
		|| !FMath::IsFinite(H.Fps) || H.Fps <= 0.f || H.Fps > 120.f || !FMath::IsFinite(H.Length) || H.Length < 0.f
		|| H.DepPackages.Num() != H.DepSigs.Num() || H.Key.IsEmpty() || !FSoftObjectPath(H.AssetPath).IsValid()) { return false; }
	const int64 W = (int64)H.Cols * H.CellW, Height = (int64)H.Rows * H.CellH;
	const int64 Expected = W * Height * 4;
	return W <= 8192 && Height <= 8192 && H.RawSize == Expected && H.CompSize > 0
		&& H.CompSize <= FOodleDataCompression::CompressedBufferSizeNeeded(Expected);
}

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

static bool BongReadHeader(const FString& File, FBongFileHeader& OutHeader, TArray<uint8>* OutPayload,
	TSharedPtr<FBongMemoryAccount, ESPMode::ThreadSafe> Account = nullptr, int64 Budget = 0,
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe>* OutMemory = nullptr, bool* bOverBudget = nullptr)
{
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*File));
	if (!Reader)
	{
		return false;
	}
	if (Reader->TotalSize() > (int64)512 * 1024 * 1024 || Reader->TotalSize() < 8) { return false; }
	*Reader << OutHeader;
	if (Reader->IsError() || Reader->Tell() > 64 * 1024 || !BongValidateHeader(OutHeader)
		|| Reader->TotalSize() - Reader->Tell() != OutHeader.CompSize)
	{
		return false;
	}
	if (OutPayload)
	{
		if (Account && OutMemory)
		{
			*OutMemory = Account->Acquire(OutHeader.RawSize + OutHeader.CompSize, Budget, false);
			if (!OutMemory->IsValid()) { if (bOverBudget) { *bOverBudget = true; } return false; }
		}
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

float FBongCacheEntry::SampleTimeAt(int32 Frame) const
{
	return FMath::Clamp((float)Frame / FMath::Max(Fps, UE_SMALL_NUMBER), 0.f, FMath::Max(0.f, Length - UE_KINDA_SMALL_NUMBER));
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
#if WITH_DEV_AUTOMATION_TESTS
	if (!TestCacheDir.IsEmpty()) { return TestCacheDir; }
#endif
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BongThumbLive") / TEXT("Cache"));
}

void FBongThumbCache::Startup()
{
	const FString Dir = GetCacheDir();
	IFileManager::Get().MakeDirectory(*Dir, true);
	// Background scan: read the index (and headers of files missing from it), delete files whose animation no longer
	// exists or changed (package signature) or whose mesh / skeleton changed, and leftovers of interrupted writes.
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
	const FBongJobStamp ScanStamp = S->Stamp(FSoftObjectPath());
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Dir, ScanStamp]()
	{
		const double T0 = FPlatformTime::Seconds();
		FBongScanResult* Result = new FBongScanResult();
		Result->Stamp = ScanStamp;
		auto DeleteScannedFile = [&](const FString& Full)
		{
			FScopeLock Lock(&S->FileMutex);
			if (S->CurrentLocked(FSoftObjectPath(), ScanStamp)) { IFileManager::Get().Delete(*Full, false, true, true); }
		};
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
			DeleteScannedFile(Dir / F);
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
					DeleteScannedFile(Full);
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
				DeleteScannedFile(Full); // animation deleted / moved while the editor was closed
				++Result->Deleted;
				++Result->Missing;
				continue;
			}
			if (CurrentSig != E.AnimSig || !BongDepsStillValid(E.Deps))
			{
				DeleteScannedFile(Full); // animation, mesh or skeleton changed: stale
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
	while (Shared->Outstanding.load() > 0)
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
	InvalidatePackage(PackageName);
}

void FBongThumbCache::InvalidatePackage(FName PackageName)
{
	SigCache.Remove(PackageName);
	TSet<FSoftObjectPath> Affected;
	for (TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		if (Pair.Value->PackageName == PackageName || Pair.Value->MeshPackage == PackageName || Pair.Value->SkeletonPackage == PackageName)
		{
			Affected.Add(Pair.Key);
		}
	}
	for (const TPair<FString, FIndexEntry>& Pair : Index)
	{
		const FSoftObjectPath Path(Pair.Value.AssetPath);
		const FString DepPrefix = PackageName.ToString() + TEXT("|");
		if (Path.GetLongPackageFName() == PackageName || Pair.Value.Deps.StartsWith(DepPrefix)
			|| Pair.Value.Deps.Contains(TEXT(";") + DepPrefix)) { Affected.Add(Path); }
	}
	for (const FSoftObjectPath& Path : Affected)
	{
		Invalidate(Path);
		DeleteFilesForAsset(Path.ToString(), TEXT("*"));
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
	const int64 Budget = MemoryBudget();
	while (GetMemoryBytes() + Bytes > Budget)
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

int64 FBongThumbCache::MemoryBudget() const
{
	return (int64)FMath::Clamp(GetDefault<UBongThumbLiveSettings>()->CacheMemoryMB, 64, 8192) * 1024 * 1024;
}

int64 FBongThumbCache::GetMemoryBytes() const { return Shared->Memory->Total(); }

FBongAtlasRef FBongThumbCache::CreateAtlas(int32 W, int32 H, TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> InitialData,
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> UploadMemory)
{
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> Memory = Shared->Memory->Acquire((int64)W * H * 4, MemoryBudget(), true);
	if (!Memory) { return nullptr; }
	FBongAtlasRef Atlas = MakeShared<FBongAtlasGPU, ESPMode::ThreadSafe>();
	Atlas->Memory = MoveTemp(Memory);
	Atlas->Size = FIntPoint(W, H);
	ENQUEUE_RENDER_COMMAND(BongThumbLiveCreateAtlas)([Atlas, W, H, InitialData, UploadMemory](FRHICommandListImmediate& RHICmdList)
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
		Tile->ResetPushed();
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
	if (!New->Atlas) { ++OverBudget; return nullptr; }
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
		if (E.Deps.IsEmpty())
		{
			for (const FName Dep : {E.MeshPackage, E.SkeletonPackage})
			{
				if (Dep.IsNone()) { continue; }
				if (!E.Deps.IsEmpty()) { E.Deps += TEXT(";"); }
				E.Deps += Dep.ToString() + TEXT("|") + BongComputePackageSig(Dep.ToString());
				if (const UPackage* Package = FindPackage(nullptr, *Dep.ToString())) { E.bNoDisk |= Package->IsDirty(); }
			}
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
	Shared->Invalidate(Path);
	DropEntry(Path);
	DiskFailed.Remove(Path);
	MissCounted.Remove(Path);
	NextDiskCheck.Remove(Path);
}

void FBongThumbCache::InvalidateObject(const UObject* Object)
{
	if (Object)
	{
		InvalidatePackage(Object->GetOutermost()->GetFName());
		Invalidate(FSoftObjectPath(Object));
	}
}

void FBongThumbCache::ClearMemory()
{
	Shared->InvalidateAll();
	Readbacks.Reset();
	for (FBongDiskLoadResult* R : PendingUploads) { LoadsInFlight.Remove(R->Path); delete R; }
	PendingUploads.Reset();
	bIndexDirty = bIndexReady;
	Entries.Reset();
	MemBytes = 0;
	FillTargetPath.Reset();
	for (TPair<FSoftObjectPath, FPushTile>& Pair : PushTiles)
	{
		Pair.Value.ResetPushed();
	}
	DiskFailed.Reset();
	MissCounted.Reset();
	NextDiskCheck.Reset();
	if (!bIndexReady) { Startup(); } // a cancelled startup scan must not leave auto-load waiting forever
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
	FScopeLock FileLock(&Shared->FileMutex); // workers validate and publish under the same lock
	++Shared->Generation;
	bIndexReady = true; // empty is already a complete index; reject any earlier scan result
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
	if (!BongDepsStillValid(E.Deps)) { return; } // the periodic key check will discard this partial/old capture
	if (Shared->Saves.load() >= 2) { return; }
	const int64 W = E.Cols * E.CellW, Height = E.Rows * E.CellH;
	const int64 StagingBytes = Align(W * 4, (int64)256) * Height;
	const int64 SaveBytes = StagingBytes + E.Bytes + FOodleDataCompression::CompressedBufferSizeNeeded(E.Bytes);
	TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe> Memory = Shared->Memory->Acquire(SaveBytes, MemoryBudget(), false);
	if (!Memory) { return; } // retry once other readbacks/uploads finish
	TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe> Job = MakeShared<FBongReadbackJob, ESPMode::ThreadSafe>();
	Job->Memory = MoveTemp(Memory);
	Job->Owner = Shared;
	++Shared->Saves;
	Job->Stamp = Shared->Stamp(E.Path);
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
	Job->FileRevision = Shared->NextFileRevision(E.FileName);
	Job->Atlas = E.Atlas;
	Job->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("BongThumbLive.Save"));
	E.bSaveStarted = true;
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
		if (!Shared->Current(FSoftObjectPath(Job->Header.AssetPath), Job->Stamp)) { Readbacks.RemoveAt(i); continue; }
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
		if (State != 2)
		{
			++DiskWriteFails;
			continue;
		}
		TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
		S->Outstanding++;
		Async(EAsyncExecution::ThreadPool, [S, Job, Dir]()
		{
			FBongDiskWriteResult* Result = new FBongDiskWriteResult();
			Result->Stamp = Job->Stamp;
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
			bool bOk = CompSize > 0;
			const FString Final = Dir / Job->FileName;
			const FString Tmp = Final + TEXT(".tmp");
			bOk = bOk && S->Commit(FSoftObjectPath(H.AssetPath), Job->Stamp, Job->FileName, Job->FileRevision, [&]()
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
				return bOk;
			});
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
				if (Data && PitchPixels >= W && BufferHeight >= H)
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
	const FBongJobStamp Stamp = S->Stamp(Path);
	const uint64 Revision = S->FileRevision(FileName);
	const int64 Budget = MemoryBudget();
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Full, Path, FileName, Key, Stamp, Revision, Budget]()
	{
		const double T0 = FPlatformTime::Seconds();
		FBongDiskLoadResult* R = new FBongDiskLoadResult();
		R->Path = Path; R->FileName = FileName; R->Key = Key; R->Stamp = Stamp;
		TArray<uint8> Payload;
		if (S->Current(Path, Stamp))
		{
			if (!BongReadHeader(Full, R->Header, &Payload, S->Memory, Budget, &R->Memory, &R->bOverBudget)
				|| R->Header.Key != Key || R->Header.AssetPath != Path.ToString())
			{
				R->bStale = !R->bOverBudget;
			}
			else if (!BongDepsStillValid(BongJoinDeps(R->Header))) { R->bStale = true; }
			else
			{
				R->Pixels = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
				R->Pixels->SetNumUninitialized(R->Header.RawSize);
				R->bOk = FOodleDataCompression::Decompress(R->Pixels->GetData(), R->Header.RawSize, Payload.GetData(), Payload.Num());
				if (!R->bOk) { R->Pixels.Reset(); R->bStale = true; }
			}
			if (R->bStale)
			{
				S->Commit(Path, Stamp, FileName, Revision, [&]() { return IFileManager::Get().Delete(*Full, false, true, true); });
			}
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
		if (!Shared->Current(FSoftObjectPath(), Scan->Stamp)) { delete Scan; continue; }
		int64 Total = 0;
		for (const FBongScanIndexEntry& E : Scan->Entries)
		{
			if (!Shared->Current(FSoftObjectPath(E.AssetPath), Scan->Stamp)) { continue; }
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
		if (!Shared->Current(FSoftObjectPath(Write->AssetPath), Write->Stamp)) { delete Write; continue; }
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
			if (FBongCacheEntry* E = Find(FSoftObjectPath(Write->AssetPath)))
			{
				if (E->FileName == Write->FileName) { E->bSaveStarted = false; }
			}
		}
		delete Write;
	}

	FBongDiskLoadResult* Load = nullptr;
	while (Shared->Loads.Dequeue(Load))
	{
		if (!Shared->Current(Load->Path, Load->Stamp))
		{
			LoadsInFlight.Remove(Load->Path); delete Load; continue;
		}
		if (!Load->bOk)
		{
			LoadsInFlight.Remove(Load->Path);
			if (Load->bOverBudget) { ++OverBudget; } else { DiskFailed.Add(Load->Path); }
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
		FString CurrentKey, CurrentSig, CurrentFile;
		const UPackage* Package = FindPackage(nullptr, *R->Path.GetLongPackageName());
		bool bFresh = Shared->Current(R->Path, R->Stamp) && !(Package && Package->IsDirty())
			&& ComputeKey(R->Path, R->Path.GetLongPackageFName(), CurrentKey, CurrentSig, CurrentFile) && CurrentKey == R->Key;
		for (const FString& Dep : H.DepPackages)
		{
			const UPackage* DepPackage = FindPackage(nullptr, *Dep);
			if (DepPackage && DepPackage->IsDirty()) { bFresh = false; }
		}
		const bool bWanted = bFresh && ActiveSet.Contains(R->Path) && !Entries.Contains(R->Path);
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
			E->Deps = BongJoinDeps(H);
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
			E->Atlas = CreateAtlas(H.Cols * H.CellW, H.Rows * H.CellH, R->Pixels, R->Memory);
			if (E->Atlas)
			{
				MemBytes += Bytes;
				Entries.Add(R->Path, MoveTemp(E));
				++DiskHits;
				DiskDecodeMsTotal += R->DecodeMs;
				++Uploads;
			}
			else { ++OverBudget; }
		}
		LoadsInFlight.Remove(R->Path);
		PendingUploads.RemoveAt(i);
		delete R; // the render command keeps the pixels alive until uploaded
	}
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongSamplingRegressionTest, "BongThumbLive.Cache.Sampling", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongSamplingRegressionTest::RunTest(const FString&)
{
	FBongCacheEntry E;
	E.Length = 60.f; E.NumFrames = 30; E.Fps = 0.5f;
	TestEqual(TEXT("Long clip reaches its final two-second interval"), E.SampleTimeAt(29), 58.f);
	for (int32 F = 0; F < E.NumFrames; ++F) { TestEqual(TEXT("Sample and playback use the same time base"), E.FrameIndexAt(E.SampleTimeAt(F)), F); }
	E.Length = 2.f; E.NumFrames = 120; E.Fps = 60.f;
	TestTrue(TEXT("Normal clips retain sub-second samples"), FMath::IsNearlyEqual(E.SampleTimeAt(30), 0.5f));
	E.Length = 0.f; E.Fps = 0.f;
	TestEqual(TEXT("Zero-length clip is finite"), E.SampleTimeAt(1), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongHeaderRegressionTest, "BongThumbLive.Cache.HeaderValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongHeaderRegressionTest::RunTest(const FString&)
{
	FBongFileHeader Good;
	Good.Key = TEXT("test"); Good.AssetPath = TEXT("/Game/BTLTest.Anim");
	Good.CellW = Good.CellH = 64; Good.Cols = Good.Rows = Good.NumFrames = 1;
	Good.Fps = 10.f; Good.Length = 0.1f; Good.RawSize = 64 * 64 * 4; Good.CompSize = 100;
	TestTrue(TEXT("Normal dimensions accepted"), BongValidateHeader(Good));
	FBongFileHeader Bad = Good; Bad.Cols = Bad.Rows = -1;
	TestFalse(TEXT("Negative dimensions whose product looks valid are rejected"), BongValidateHeader(Bad));
	Bad = Good; Bad.Cols = MAX_int32;
	TestFalse(TEXT("Overflowing dimensions are rejected before multiplication"), BongValidateHeader(Bad));
	Bad = Good; Bad.Fps = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("NaN FPS rejected"), BongValidateHeader(Bad));
	Bad = Good; Bad.Length = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("Infinite length rejected"), BongValidateHeader(Bad));
	Bad = Good; Bad.CompSize = MAX_int64;
	TestFalse(TEXT("Unbounded compressed allocation rejected"), BongValidateHeader(Bad));
	Bad = Good; Bad.DepPackages.Add(TEXT("/Game/Mesh"));
	TestFalse(TEXT("Mismatched dependency arrays rejected"), BongValidateHeader(Bad));
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	uint32 Magic = BongCacheConst::Magic; int32 Version = BongCacheConst::FileVersion, Count = MAX_int32;
	Writer << Magic << Version << Count;
	FMemoryReader Reader(Bytes); FBongFileHeader Loaded; Reader << Loaded;
	TestTrue(TEXT("Oversized string prefix rejected without allocating it"), Reader.IsError());
	Bytes.Reset(); FMemoryWriter RoundTripWriter(Bytes); RoundTripWriter << Good;
	FMemoryReader RoundTripReader(Bytes); RoundTripReader << Loaded;
	TestFalse(TEXT("Normal header still deserializes"), RoundTripReader.IsError());
	TestTrue(TEXT("Round-trip header remains valid"), BongValidateHeader(Loaded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongMemoryRegressionTest, "BongThumbLive.Cache.MemoryBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongMemoryRegressionTest::RunTest(const FString&)
{
	auto Account = MakeShared<FBongMemoryAccount, ESPMode::ThreadSafe>();
	TArray<TSharedPtr<FBongMemoryLease, ESPMode::ThreadSafe>> Leases;
	Leases.SetNum(32);
	ParallelFor(Leases.Num(), [&](int32 I) { Leases[I] = Account->Acquire(128, 1024, false); });
	TestEqual(TEXT("Concurrent worker reservations cannot exceed the cap"), Account->Total(), (int64)1024);
	TestFalse(TEXT("GPU allocation also sees CPU reservations"), Account->Acquire(1, 1024, true).IsValid());
	Leases.Reset();
	TestEqual(TEXT("Worker completion returns all bytes"), Account->Total(), (int64)0);
	FBongAtlasRef Atlas = MakeShared<FBongAtlasGPU, ESPMode::ThreadSafe>();
	Atlas->Memory = Account->Acquire(512, 1024, true);
	auto InFlightAtlas = Atlas;
	Atlas.Reset();
	TestEqual(TEXT("Evicted atlas remains charged while a job holds it"), Account->Total(), (int64)512);
	InFlightAtlas.Reset();
	TestEqual(TEXT("Final job/render reference releases GPU charge"), Account->Total(), (int64)0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongCacheRegressionTest, "BongThumbLive.Cache.Invalidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongCacheRegressionTest::RunTest(const FString&)
{
	FBongThumbCache* C = new FBongThumbCache();
	C->TestCacheDir = FPaths::ProjectSavedDir() / TEXT("BongThumbLiveTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IFileManager::Get().MakeDirectory(*C->TestCacheDir, true);
	C->bIndexReady = true;
	const FSoftObjectPath Path(TEXT("/Game/BTLTest.Anim")), Other(TEXT("/Game/BTLOther.Anim"));
	const FName Dep(TEXT("/Game/BTLSkeleton"));
	TUniquePtr<FBongCacheEntry> E = MakeUnique<FBongCacheEntry>();
	E->Path = Path; E->SkeletonPackage = Dep; C->Entries.Add(Path, MoveTemp(E));
	C->Entries.Add(Other, MakeUnique<FBongCacheEntry>());
	FBongThumbCache::FIndexEntry I; I.AssetPath = Path.ToString(); I.Deps = Dep.ToString() + TEXT("|old");
	C->Index.Add(TEXT("old.btlc"), I); C->FilesByAsset.Add(Path.ToString(), TEXT("old.btlc"));
	const FBongJobStamp BeforeEdit = C->Shared->Stamp(Path);
	C->OnPackageSaved(Dep);
	TestNull(TEXT("Skeleton save evicts dependent memory frames"), C->Find(Path));
	TestNotNull(TEXT("Unrelated animation survives dependency invalidation"), C->Find(Other));
	TestFalse(TEXT("Dependency file removed from lookup immediately"), C->Index.Contains(TEXT("old.btlc")));
	TestFalse(TEXT("Previous asset jobs become obsolete"), C->Shared->Current(Path, BeforeEdit));
	const FBongJobStamp BeforeClear = C->Shared->Stamp(Path);
	const uint64 Revision = C->Shared->NextFileRevision(TEXT("late.btlc"));
	FEvent* Started = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Continue = FPlatformProcess::GetSynchEventFromPool(true);
	auto S = C->Shared;
	const FString LateFile = C->TestCacheDir / TEXT("late.btlc");
	TFuture<bool> LateWrite = Async(EAsyncExecution::ThreadPool, [S, Path, BeforeClear, Revision, Started, Continue, LateFile]()
	{
		Started->Trigger();
		if (!Continue->Wait(5000)) { return false; }
		return S->Commit(Path, BeforeClear, TEXT("late.btlc"), Revision, [&]() { return FFileHelper::SaveStringToFile(TEXT("old"), *LateFile); });
	});
	TestTrue(TEXT("Old worker reached its publication barrier"), Started->Wait(2000));
	C->ClearDisk();
	Continue->Trigger();
	TestFalse(TEXT("A worker that finishes after clear cannot publish"), LateWrite.Get());
	TestFalse(TEXT("Cancelled worker did not recreate the physical file"), IFileManager::Get().FileExists(*LateFile));
	FPlatformProcess::ReturnSynchEventToPool(Started); FPlatformProcess::ReturnSynchEventToPool(Continue);
	bool bPublished = false;
	C->Shared->Commit(Path, BeforeClear, TEXT("late.btlc"), Revision, [&]() { bPublished = true; return true; });
	TestFalse(TEXT("Old save cannot publish after clear"), bPublished);
	FBongDiskLoadResult* Load = new FBongDiskLoadResult(); Load->Path = Path; Load->Stamp = BeforeClear; Load->bOk = true;
	C->LoadsInFlight.Add(Path); C->Shared->Loads.Enqueue(Load);
	FBongDiskWriteResult* Write = new FBongDiskWriteResult(); Write->Stamp = BeforeClear; Write->AssetPath = Path.ToString(); Write->FileName = TEXT("late.btlc"); Write->bOk = true;
	C->Shared->Writes.Enqueue(Write);
	FBongScanResult* Scan = new FBongScanResult(); Scan->Stamp = BeforeClear; C->Shared->Scans.Enqueue(Scan);
	C->ProcessAsyncResults(FPlatformTime::Seconds());
	TestEqual(TEXT("Old read cannot refill memory"), C->Entries.Num(), 0);
	TestEqual(TEXT("Old write/scan cannot refill the cleared index"), C->Index.Num(), 0);
	TestEqual(TEXT("Cancelled reads release queue slots"), C->LoadsInFlight.Num(), 0);
	const FBongJobStamp Fresh = C->Shared->Stamp(Path);
	const uint64 OldDelete = C->Shared->NextFileRevision(TEXT("same.btlc"));
	const uint64 NewSave = C->Shared->NextFileRevision(TEXT("same.btlc"));
	TestFalse(TEXT("Old delete cannot remove a newer save"), C->Shared->Commit(Path, Fresh, TEXT("same.btlc"), OldDelete, []() { return true; }));
	TestTrue(TEXT("Current save can publish"), C->Shared->Commit(Path, Fresh, TEXT("same.btlc"), NewSave, []() { return true; }));
	C->ShutdownInternal();
	const FString Dir = C->TestCacheDir;
	delete C;
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}
#endif

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
	const FBongJobStamp Stamp = S->Stamp(FSoftObjectPath());
	const uint64 Revision = S->NextFileRevision(FileName);
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Full, FileName, Stamp, Revision]()
	{
		S->Commit(FSoftObjectPath(), Stamp, FileName, Revision, [&]() { return IFileManager::Get().Delete(*Full, false, true, true); });
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
		FScopeLock Lock(&Shared->FileMutex);
		FFileHelper::SaveStringToFile(Text, *Final, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return;
	}
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> S = Shared;
	const FBongJobStamp Stamp = S->Stamp(FSoftObjectPath());
	const uint64 Revision = S->NextFileRevision(BongCacheConst::IndexFileName);
	S->Outstanding++;
	Async(EAsyncExecution::ThreadPool, [S, Text = MoveTemp(Text), Final, Stamp, Revision]()
	{
		S->Commit(FSoftObjectPath(), Stamp, BongCacheConst::IndexFileName, Revision, [&]()
		{
			const FString Tmp = Final + TEXT(".tmp");
			return FFileHelper::SaveStringToFile(Text, *Tmp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
				&& IFileManager::Get().Move(*Final, *Tmp, true, true);
		});
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
		Tile->ResetPushed();
	}
}

void FBongThumbCache::Tick(const FTickParams& Params)
{
	const double Now = Params.Now;
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	ActiveSet = TSet<FSoftObjectPath>(Params.ActivePaths);
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

	// Validate keys (settings changed, asset saved / modified) at most once per second per entry.
	TArray<FSoftObjectPath> Stale;
	TSet<FSoftObjectPath> StaleDeps;
	for (TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		FBongCacheEntry& E = *Pair.Value;
		if (Now - E.LastKeyCheck < 1.0)
		{
			continue;
		}
		E.LastKeyCheck = Now;
		if (!E.Deps.IsEmpty() && !BongDepsStillValid(E.Deps))
		{
			Stale.Add(Pair.Key); StaleDeps.Add(Pair.Key); continue;
		}
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
		Invalidate(P);
		if (StaleDeps.Contains(P)) { DeleteFilesForAsset(P.ToString(), TEXT("*")); }
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
	for (TPair<FSoftObjectPath, TUniquePtr<FBongCacheEntry>>& Pair : Entries)
	{
		if (Pair.Value->IsComplete()) { MaybeStartSave(*Pair.Value); }
	}
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

static std::atomic<int32> GBongTileTexturesCreated{0};

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
		// Push into the pool texture(s) the tile widgets really show. The pool keys textures by (asset, size), and the
		// Content Browser creates tile thumbnails either at 256 (tile widget generated first) or at its current
		// thumbnail size (thumbnails prefetched by the relevancy pass, i.e. tiles that appear by scrolling). Pushing
		// only into a fixed 256 texture left scrolled-in tiles frozen on the pool's first render until hovered.
		TArray<FIntPoint, TInlineAllocator<2>> Sizes;
		if (const TArray<FIntPoint, TInlineAllocator<2>>* Found = Params.TileSizes.Find(Path))
		{
			Sizes = *Found;
		}
		if (Sizes.IsEmpty())
		{
			Sizes.Add(FIntPoint(TileSize, TileSize)); // hovered tile outside the scanned views, or no viewport found
		}
		for (int32 t = Tile.Targets.Num() - 1; t >= 0; --t)
		{
			if (!Tile.Targets[t].Thumb.IsValid() || !Sizes.Contains(Tile.Targets[t].Thumb->GetSize()))
			{
				Tile.Targets.RemoveAtSwap(t);
			}
		}
		const bool bHovered = Path == Params.Hovered;
		int32 Frame = E->FrameIndexAt((float)BongLoopTime(E->Length, bHovered));
		if (!E->Valid.IsValidIndex(Frame) || !E->Valid[Frame])
		{
			Frame = E->NearestValid(Frame);
		}
		if (Frame == INDEX_NONE)
		{
			continue;
		}
		for (const FIntPoint& Size : Sizes)
		{
			FPushTarget* Target = Tile.Targets.FindByPredicate([&](const FPushTarget& T) { return T.Thumb->GetSize() == Size; });
			if (!Target)
			{
				FAssetData Data;
				if (!IsAnimPath(Path, &Data) || Size.X <= 0 || Size.Y <= 0 || Size.X > 1024 || Size.Y > 1024)
				{
					continue;
				}
				Target = &Tile.Targets.AddDefaulted_GetRef();
				Target->Thumb = MakeShared<FAssetThumbnail>(Data, Size.X, Size.Y, Params.Pool);
				++PushSizeCounts.FindOrAdd(Size);
			}
			if (Frame == Target->LastPushed)
			{
				continue;
			}
			FSlateTexture2DRHIRef* Dst = static_cast<FSlateTexture2DRHIRef*>(Target->Thumb->GetViewportRenderTargetTexture());
			if (!Dst)
			{
				// No GPU texture yet: the pool never rendered this tile successfully (e.g. it tried while the asset was
				// unloaded and had no saved thumbnail, or before a preview mesh was known). Without a pool render the tile
				// keeps the class icon (or stays black) forever and only a mouse hover fixed it. Now that the cache has
				// frames, the renderer can serve them, so ask the pool to render the tile again (asset loaded), or load
				// the asset first (read-only async load; the pool re-renders tiles of freshly loaded assets).
				const FAssetData& Data = Target->Thumb->GetAssetData();
				if (Now - Target->LastRefresh >= 1.0)
				{
					Target->LastRefresh = Now;
					if (Data.IsAssetLoaded())
					{
						Params.Pool->RefreshThumbnail(Target->Thumb);
						++TileRefreshes;
					}
					else if (FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr())
					{
						if (Module->QueueAssetLoad(Data.GetSoftObjectPath(), true)) { ++TileLoads; }
					}
				}
				continue;
			}
			Target->LastPushed = Frame;
			Copies.Add({ E->Atlas, E->CellRect(Frame), Dst });
		}
		E->LastShown = Frame;
		E->LastUse = Now;
		TouchIndex(*E, Now);
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
			if (!DstTex && Copy.Atlas->Texture && Copy.Dst->GetWidth() > 0 && Copy.Dst->GetHeight() > 0)
			{
				// The pool never got a successful render for this tile (no saved package thumbnail, or it was not
				// renderable when the pool tried): its texture has no GPU resource at all, so the tile stayed black or
				// on the class icon. Give it one (render-targetable, like the pool's own render target) to push into.
				// The pool replaces it with its render target if it renders the tile later.
				const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("BongThumbLive.TileTexture"),
					(int32)Copy.Dst->GetWidth(), (int32)Copy.Dst->GetHeight(), PF_B8G8R8A8)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
					.SetInitialState(ERHIAccess::SRVMask);
				FTextureRHIRef NewTex = RHICmdList.CreateTexture(Desc);
				Copy.Dst->SetRHIRef(NewTex, Copy.Dst->GetWidth(), Copy.Dst->GetHeight());
				DstTex = NewTex.GetReference();
				++GBongTileTexturesCreated;
			}
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

static int32 GBongFirstFrameFirst = 0;
static FAutoConsoleVariableRef CVarBongFirstFrameFirst(TEXT("BongThumbLive.FirstFrameFirst"), GBongFirstFrameFirst,
	TEXT("1 = render one frame for every visible tile with none before coarse/refine passes."));

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
	// Visible tiles that have (almost) nothing yet come first, fewest frames first: every newly shown tile (scrolling,
	// cold cache) gets a coarse set of frames right away instead of waiting until earlier tiles are complete.
	{
		TArray<TPair<int32, FSoftObjectPath>, TInlineAllocator<64>> Starving;
		for (const FSoftObjectPath& P : Params.ActivePaths)
		{
			const FBongCacheEntry* Existing = Find(P);
			const int32 Have = Existing ? Existing->NumValid : 0;
			if ((!Existing || !Existing->IsComplete()) && Have < CoarseFrames && !LoadsInFlight.Contains(P) && P.ResolveObject())
			{
				Starving.Add({ Have, P });
			}
		}
		Starving.StableSort([](const TPair<int32, FSoftObjectPath>& A, const TPair<int32, FSoftObjectPath>& B) { return A.Key < B.Key; });
		for (const TPair<int32, FSoftObjectPath>& S : Starving)
		{
			Order.AddUnique(S.Value);
		}
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
		if (Attempts >= 6 || FPlatformTime::Seconds() >= Params.FillDeadline)
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
		const bool bCoarse = E->NumValid < CoarseFrames && E->NumFrames > CoarseFrames;
		const bool bFirstOnly = GBongFirstFrameFirst != 0 && E->NumValid == 0 && E->NumFrames > 1;
		const int32 Done = UBongThumbLiveRenderer::FillFrames(Anim, *E, Params.FillDeadline, Start, bFirstOnly ? 1 : (bCoarse ? CoarseFrames : 0));
		if (bCoarse)
		{
			CoarseRenders += Done;
		}
		else
		{
			FillTargetPath = E->IsComplete() ? FSoftObjectPath() : Path;
		}
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
	FString SizesStr;
	for (const TPair<FIntPoint, int32>& Pair : PushSizeCounts)
	{
		SizesStr += FString::Printf(TEXT("%s%dx%d:%d"), SizesStr.IsEmpty() ? TEXT("") : TEXT(","), Pair.Key.X, Pair.Key.Y, Pair.Value);
	}
	return FString::Printf(TEXT("memory cache: %s entries=%d (complete %d, from disk %d) frames=%lld GPU=%.1f MB (+cpu in flight %.1f MB) / %d MB, evictions=%lld, playing tiles=%d, pushed frames=%lld, draw hits=%lld, renders=%lld, notStored(compiling)=%lld, overBudget=%lld, tileTexturesCreated=%d, tileRefreshes=%d, tileLoads=%d, pushTargetSizes=%s, coarseRenders=%lld\n")
		TEXT("disk cache: %s %s files=%d %.1f / %d MB, hits=%lld misses=%lld stale=%lld writes=%lld writeFails=%lld loading=%d avgDecode=%.1f ms, folder=%s"),
		S->bUseFrameCache ? TEXT("on") : TEXT("OFF"), Entries.Num(), Complete, FromDisk, Frames, (GetMemoryBytes() - Shared->Memory->Cpu()) / (1024.0 * 1024.0), Shared->Memory->Cpu() / (1024.0 * 1024.0),
		S->CacheMemoryMB, Evictions, PushTiles.Num(), PushedFrames, DrawHits, Renders, NotStoredBusy, OverBudget, GBongTileTexturesCreated.load(), TileRefreshes, TileLoads, SizesStr.IsEmpty() ? TEXT("-") : *SizesStr, CoarseRenders,
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

	void Track(FName Root, const FSoftObjectPath& AssetPath, bool bMesh, bool bAnimation)
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
		New.Info.bAnimation = bAnimation;
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
			N += Pair.Value.Info.bMesh == bMesh && (bMesh || Pair.Value.Info.bAnimation) ? 1 : 0;
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
