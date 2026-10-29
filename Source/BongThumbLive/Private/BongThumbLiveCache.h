// BongThumbLive - animation thumbnail frame cache.
//
// * GPU atlas per animation: every frame (quantised to Cache FPS, at most "MaxFramesPerAsset" frames, Cache Resolution
//   px per cell) lives in ONE GPU texture. Playback of a complete entry is a per-tile GPU copy of the one cell that is
//   shown into the Content Browser tile texture (no scene render, no CPU readback, not counted in FrameBudgetMs).
// * Hard memory cap (Cache Memory MB, default 1024): least recently shown entries are evicted (they stay on disk).
// * Persistent disk cache in <Project>/Saved/BongThumbLive/Cache: one Oodle-compressed file per animation + settings
//   key, loaded / decoded on worker threads and uploaded once; LRU by last display in a visible tile (index file).
#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"
#include "UObject/GCObject.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/WeakObjectPtr.h"
#include "Containers/BitArray.h"
#include "AssetRegistry/AssetData.h"

class UAnimSequenceBase;
class USkeletalMesh;
class UTextureRenderTarget2D;
class FRenderTarget;
class FCanvas;
class FAssetThumbnail;
class FAssetThumbnailPool;
struct FBongAsyncShared;
struct FBongDiskLoadResult;
struct FBongReadbackJob;

/** GPU atlas texture (created / used on the render thread; shared with in-flight render commands). */
struct FBongAtlasGPU
{
	FTextureRHIRef Texture;
	FIntPoint Size = FIntPoint::ZeroValue;
};
using FBongAtlasRef = TSharedPtr<FBongAtlasGPU, ESPMode::ThreadSafe>;

struct FBongCacheEntry
{
	FSoftObjectPath Path;
	FName PackageName;
	TWeakObjectPtr<UAnimSequenceBase> Anim;
	/** Full cache key (settings + package change signal). Empty for entries built from unsaved (dirty) data. */
	FString KeyString;
	FString FileName;
	FString AnimSig;
	int32 CellW = 0;
	int32 CellH = 0;
	int32 Cols = 0;
	int32 Rows = 0;
	int32 NumFrames = 0;
	float Fps = 0.f;
	float Length = 0.f;
	FBongAtlasRef Atlas;
	int64 Bytes = 0;
	TBitArray<> Valid;
	int32 NumValid = 0;
	bool bNoDisk = false;      // dirty package / disk cache off: memory only
	bool bFromDisk = false;
	bool bSaveStarted = false;
	/** Preview mesh / skeleton package names used to render it (stale check of the disk file). */
	FName MeshPackage;
	FName SkeletonPackage;
	double LastUse = 0.0;
	double LastKeyCheck = 0.0;
	double LastIndexTouch = -1.0e9;
	int32 LastShown = INDEX_NONE;

	bool IsComplete() const { return NumFrames > 0 && NumValid >= NumFrames; }
	int32 FrameIndexAt(float Time) const;
	FIntRect CellRect(int32 Frame) const;
	/** Valid frame closest before Frame (wrapping), or INDEX_NONE. */
	int32 NearestValid(int32 Frame) const;
};

class FBongThumbCache : public FGCObject
{
public:
	/** Content Browser tile thumbnails are 256 px (SAssetView); fills render at this size. */
	static constexpr int32 TileSize = 256;

	static FBongThumbCache& Get();
	static FBongThumbCache* GetPtr();
	static void Shutdown();

	// --- entries (game thread) ---
	FBongCacheEntry* Find(const FSoftObjectPath& Path);
	/**
	 * Entry for a loaded animation, validated against the current key. Creates one (allocating its atlas within the
	 * memory budget) when bCreate and no disk load is pending/possible for it; null when over budget / waiting.
	 */
	FBongCacheEntry* GetForAnim(UAnimSequenceBase* Anim, bool bCreate, double Now);
	/** GPU-copy Src[SrcRect] into the frame's cell (render thread, after the render enqueued before it). */
	void StoreFrame(FBongCacheEntry& Entry, int32 Frame, FRenderTarget* Src, const FIntRect& SrcRect, USkeletalMesh* Mesh);
	/** GPU-copy a cached frame into Dst[DstRect] (flushes the canvas first so its clear lands before the copy). */
	void ShowFrame(FBongCacheEntry& Entry, int32 Frame, FCanvas* Canvas, FRenderTarget* Dst, const FIntRect& DstRect, double Now);

	void Invalidate(const FSoftObjectPath& Path);
	void InvalidateObject(const UObject* Object);
	/** Drops every in-memory entry (settings change). The disk cache is kept (other keys stay valid). */
	void ClearMemory();
	/** Deletes the whole disk cache (and memory). */
	void ClearDisk();
	/** Memory pressure: drop entries not on screen. */
	void TrimToVisible();

	// --- per frame (game thread) ---
	struct FTickParams
	{
		double Now = 0.0;
		/** Animation tiles that should play right now (on screen + allowed by the mode). */
		TArray<FSoftObjectPath> ActivePaths;
		FSoftObjectPath Hovered;
		TSharedPtr<FAssetThumbnailPool> Pool;
		bool bAllowFill = true;
		double FillDeadline = 0.0;
	};
	/** Disk loads/uploads, LRU, index, playback push of complete entries and fill of missing frames. */
	void Tick(const FTickParams& Params);
	/** The pool re-rendered this asset's tile (overwrote our pushed frame): push again. */
	void OnPoolRendered(const FSoftObjectPath& Path);
	/** True when an animation does not need to be loaded to play (complete in memory, or a disk file for its key). */
	bool CanPlayWithoutLoading(const FSoftObjectPath& Path, FName PackageName);
	/** False until the startup disk scan finished (auto-load waits for it so hits do not load the asset). */
	bool IsDiskReady() const;

	/** Asset-registry check (cached, no load): Path is an animation asset. */
	bool IsAnimPath(const FSoftObjectPath& Path, FAssetData* OutData);

	// --- disk ---
	FString GetCacheDir() const;
	void OnAssetRemoved(const FAssetData& Asset);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath);
	void OnPackageSaved(FName PackageName);

	// --- stats ---
	FString GetStats() const;
	/** "N개 파일, X MB / Y MB" (settings page). */
	FString GetDiskSummary() const;
	int64 GetMemoryBytes() const { return MemBytes + InFlightCpuBytes; }
	int32 GetNumPushTiles() const { return PushTiles.Num(); }
	int32 GetNumComplete() const;
	/** Debug: write a few frames of the first complete entries (PNG) and log per-frame hashes. */
	void Dump(const FString& Dir);

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FBongThumbCache"); }

	UTextureRenderTarget2D* GetFillTarget();

	int64 Renders = 0;
	int64 DrawHits = 0;
	int64 NotStoredBusy = 0;
	int64 OverBudget = 0;

private:
	FBongThumbCache();
	~FBongThumbCache();
	void Startup();
	void ShutdownInternal();

	bool ComputeKey(const FSoftObjectPath& Path, FName PackageName, FString& OutKey, FString& OutAnimSig, FString& OutFileName);
	FString GetPackageSig(FName PackageName, double Now);
	void SetupGeometry(FBongCacheEntry& E, float Length) const;
	bool Reserve(int64 Bytes, const TSet<FSoftObjectPath>* Keep, double Now);
	FBongAtlasRef CreateAtlas(int32 W, int32 H, TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> InitialData);
	void DropEntry(const FSoftObjectPath& Path);
	void MaybeStartSave(FBongCacheEntry& E);
	void StartDiskLoad(const FSoftObjectPath& Path, const FString& FileName, const FString& Key);
	void ProcessAsyncResults(double Now);
	void PollReadbacks();
	void TouchIndex(FBongCacheEntry& E, double Now);
	void EnforceDiskLimit();
	void DeleteFileAsync(const FString& FileName);
	void DeleteFilesForAsset(const FString& AssetPath, const FString& KeepFile);
	void WriteIndex(bool bSync);
	void Push(const FTickParams& Params, const TSet<FSoftObjectPath>& Active);
	void Fill(const FTickParams& Params);

	TMap<FSoftObjectPath, TUniquePtr<FBongCacheEntry>> Entries;
	int64 MemBytes = 0;
	int64 InFlightCpuBytes = 0;
	int64 Evictions = 0;
	int64 PushedFrames = 0;
	int64 DiskHits = 0;
	int64 DiskMisses = 0;
	int64 DiskStale = 0;
	int64 DiskWrites = 0;
	int64 DiskWriteFails = 0;
	double DiskDecodeMsTotal = 0.0;
	TObjectPtr<UTextureRenderTarget2D> FillTarget = nullptr;

	struct FIndexEntry
	{
		FString AssetPath;
		FString AnimSig;
		int64 Bytes = 0;
		int64 LastUse = 0; // unix seconds
		FString Deps;      // "pkg|sig;pkg|sig" (preview mesh, skeleton)
	};
	TMap<FString, FIndexEntry> Index;
	TMultiMap<FString, FString> FilesByAsset;
	TSet<FSoftObjectPath> ActiveSet;
	TSet<FSoftObjectPath> MissCounted;
	TMap<FSoftObjectPath, double> NextDiskCheck;
	bool bIndexReady = false;
	bool bIndexDirty = false;
	double LastIndexWrite = 0.0;
	TSet<FSoftObjectPath> LoadsInFlight;
	TSet<FSoftObjectPath> DiskFailed; // stale / unreadable this session: fill instead
	TArray<FBongDiskLoadResult*> PendingUploads;
	TArray<TSharedPtr<FBongReadbackJob, ESPMode::ThreadSafe>> Readbacks;
	TSharedPtr<FBongAsyncShared, ESPMode::ThreadSafe> Shared;

	struct FPushTile
	{
		TSharedPtr<FAssetThumbnail> Thumb;
		double LastActive = 0.0;
		int32 LastPushed = INDEX_NONE;
		double LastRefresh = -1000.0;
		bool bLoadRequested = false;
	};
	int32 TileRefreshes = 0;
	int32 TileLoads = 0;
	TMap<FSoftObjectPath, FPushTile> PushTiles;
	TMap<FName, TPair<FString, double>> SigCache;
	TMap<FSoftObjectPath, FAssetData> AnimInfo;
	TSet<FSoftObjectPath> NotAnim;
	FSoftObjectPath FillTargetPath;
	double LastPushTime = 0.0;
	double LastLruCheck = 0.0;
	double LastFillSwitch = 0.0;
};

/**
 * Packages the plugin loaded itself (visible auto-load of animations, preview-mesh loads) and the dependency packages
 * that came into memory with them. Editor assets are RF_Standalone, so without this they would stay loaded for the
 * whole session (the cause of the unbounded memory growth). Released = RF_Standalone cleared on clean (not dirty,
 * not open in an editor) packages, so the next GC frees them unless something else still references them.
 */
namespace BongOwned
{
	/** Call right before starting the load of Root (no-op if it is already in memory: then it is not ours). */
	void Track(FName Root, const FSoftObjectPath& AssetPath, bool bMesh);
	void Touch(FName Root, double Now);
	struct FInfo
	{
		FName Root;
		FSoftObjectPath AssetPath;
		bool bMesh = false;
		double TrackedAt = 0.0;
		double LastUse = 0.0;
	};
	/** Releases every root for which ShouldRelease returns true; returns how many were released. */
	int32 ReleaseIf(TFunctionRef<bool(const FInfo&)> ShouldRelease);
	int32 Num(bool bMesh);
	/** Requests a full GC soon (throttled) when something was released. */
	void RequestGC(bool bForce);
	/** Runs the requested GC (at most every 15 s unless forced). Game thread, from the module tick. */
	void TickGC(double Now);
	void ReleaseAll();
}
