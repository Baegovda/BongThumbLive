#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Containers/Ticker.h"
#include "UObject/ObjectKey.h"
#include "UObject/SoftObjectPath.h"
#include "Layout/SlateRect.h"

class SWidget;
class SWindow;

/** One thumbnail viewport of a visible tile, and the asset view it scrolls in (clip rect for the loading spinner). */
struct FBongTileViewport
{
	TWeakPtr<SWidget> Viewport;
	TWeakPtr<SWidget> View;
};

/** A loading spinner to draw this frame (absolute Slate coordinates). */
struct FBongSpinnerDraw
{
	FSlateRect Tile;
	FSlateRect Clip;
	double Age = 0.0; // seconds the tile has been waiting for its first frame
	TWeakPtr<SWindow> Window;
};

class UAnimSequenceBase;
struct FAssetData;
class IConsoleObject;

DECLARE_LOG_CATEGORY_EXTERN(LogBongThumbLive, Log, All);

/** Asset kinds handled by BongThumbLive. */
enum class EBongThumbKind : uint8
{
	None,
	Animation,
	Material,
	Niagara,
	Cascade,
};

class FBongThumbLiveModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FBongThumbLiveModule* GetPtr();

	/** Called by the renderers every time they draw a live thumbnail (only updates tiles that are on screen). */
	void NotifyDrawn(UObject* Asset);

	/** Loop time (seconds) to pose Anim at right now, according to the settings (Hover FPS when it is under the mouse). */
	static float GetLoopTime(const UAnimSequenceBase* Anim);

	/** True when Asset is the Content Browser item currently under the mouse cursor. */
	bool IsHovered(const UObject* Asset) const;

	/** Which kind an object is, or None. */
	static EBongThumbKind GetKind(const UObject* Obj);
	static EBongThumbKind GetKind(const FAssetData& Asset);
	/**
	 * True when Obj is completely loaded and alive, so it is safe to inspect and simulate.
	 * Objects still being (async) loaded or post-loaded must never be touched by a thumbnail renderer:
	 * e.g. UNiagaraSystem::IsReadyToRun() on a system that is mid async-load dereferences null data.
	 */
	static bool IsObjectSafe(const UObject* Obj);
	/** Live behaviour enabled for this kind (master switch + per-type toggle). */
	static bool IsKindEnabled(EBongThumbKind Kind);

	/** Settings page edited: re-evaluate folder auto-load etc. right away. */
	void OnSettingsChanged();

	/**
	 * Per-frame game-thread budget for live thumbnail work (Settings: FrameBudgetMs).
	 * The renderers report the time each Draw took; the scheduler requests only as many refreshes per frame as fit
	 * in the budget, and a renderer that is over budget shows a cached frame instead of rendering a new one.
	 */
	static void AddRenderTime(double Seconds);
	static double GetRenderMsThisFrame();
	static bool IsOverFrameBudget();
	static double GetFrameBudgetMs();

	/**
	 * Render-thread / GPU governor for off-screen scene renders (cache fill and uncached live draws).
	 * The game-thread budget above does not see the render-thread cost of a scene render (~1.5-2 ms each with a
	 * re-created skinned proxy), so a fill that was cheap on the game thread could queue 50+ renders in one frame and
	 * the next frames waited ~100 ms in "Frame Sync" (measured with Unreal Insights, v2.1). The number of scene renders
	 * per engine frame now follows their measured render-thread cost (BongThumbLive.SceneRenderBudgetMs per frame).
	 */
	static bool CanSceneRender();
	/** Bracket a scene render: BeginSceneRender() before RenderViewFamily, NoteSceneRender() after it. */
	static void BeginSceneRender();
	static void NoteSceneRender();
	static FString GetRenderGovernorStats();

private:
	bool Tick(float DeltaTime);
	void OnContentBrowserSelectionChanged(const TArray<FAssetData>& Selected, bool bIsPrimaryBrowser);
	void OnContentBrowserPathChanged(const FString& NewPath);
	/** Shared thumbnail pool rendered (or failed to render) a tile: queue its asset for loading when it is not loaded yet. */
	void OnPoolThumbnail(const FAssetData& AssetData);
	void HookThumbnailPool();
	void UnhookThumbnailPool();
	/** Find the Content Browser item under the mouse cursor (asset tiles carry their virtual path as FTagMetaData). */
	void UpdateHover(double Now);
	void UpdateVisibleTiles(double Now);
public:
	/** Loading spinner (drawn by SBongSpinnerOverlay in the window overlay, see UpdateSpinners). */
	const TArray<FBongSpinnerDraw>& GetSpinnerDraws() const { return SpinnerDraws; }
	double GetSpinnerUpdateTime() const { return SpinnerUpdateTime; }
	static constexpr double SpinnerDelaySeconds = 0.3;  // no flicker for tiles whose cached frames arrive at once
	static constexpr double SpinnerMaxSeconds = 90.0;   // safety net: stop spinning for tiles that never get frames
private:
	void UpdateSpinners(double Now);
	void RemoveSpinnerOverlays();
	UObject* ResolveTileTag(const FString& TagString);
	/** Content Browser tile tag (virtual path) -> asset path (no load). */
	FSoftObjectPath TagToPath(const FString& TagString);
	/** Frame cache tick (playback of cached tiles, disk loads, fill), memory guard and release of what we loaded. */
	void TickCache(double Now, bool bBlocked);
	void UpdateMemoryGuard(double Now);
	void ReleaseUnderPressure(const TCHAR* Why);
	void RegisterToolbar();
public:
	bool IsUnderMemoryPressure() const { return bMemoryPressure; }
private:
public:
	/** True when the shared pool can render a fresh thumbnail for Obj right now (no stock-thumbnail fallback). */
	static bool IsRefreshableNow(UObject* Obj);
private:
	/** Folder auto-load: detect the Content Browser's current folder and queue its assets. */
	void UpdateFolderAutoLoad(double Now);
	void PumpAutoLoads();
	FString GetCurrentContentBrowserFolder() const;
	void RegisterRenderers(bool bIncludeDeferred);
	void UnregisterRenderers();
	void RegisterConsoleCommands();

	struct FTracked
	{
		TWeakObjectPtr<UObject> Asset;
		double LastDrawTime = 0.0;
		double LastRequestTime = 0.0;
	};
	TMap<FObjectKey, FTracked> Tracked;
	/** Loaded assets we poked (dirtied) to discover whether a live thumbnail exists for them. */
	TMap<FObjectKey, double> Probed;
	double LastProbeTime = 0.0;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle SelectionHandle;
	FDelegateHandle PathChangedHandle;
	FDelegateHandle PreDeleteHandle;
	double LastPruneTime = 0.0;
	double LastBroadcastTime = 0.0;
	bool bRenderersRegistered = false;
	bool bCustomizationRegistered = false;
	int32 RegisterRetryTicks = 0;
	/** Engine renderer classes we replaced, restored on shutdown. Key = asset class path. */
	TMap<FString, FString> ReplacedRenderers;
	TArray<IConsoleObject*> ConsoleObjects;

	/** Folder auto-load state. */
	FString AutoLoadFolder;
	TArray<FSoftObjectPath> FolderAssets;
	bool bFolderCheckRequested = true;
	double LastFolderCheckTime = 0.0;
	TArray<FName> PendingPackages;
	int32 InFlightLoads = 0;
	int32 AutoLoadQueued = 0;
	int32 AutoLoadDone = 0;
	/** Visible-thumbnail auto-load (driven by the shared thumbnail pool): not reset on folder change. */
	TArray<FName> VisiblePending;
	TSet<FName> VisibleSeen;
	/** Packages recently loaded by the visible-thumbnail auto-load (probed in continuous mode). */
	TArray<FName> VisibleLoaded;
	int32 VisibleQueued = 0;
	int32 VisibleDone = 0;
	int32 VisibleQueuedSinceLog = 0;
	double LastVisibleLogTime = 0.0;
	TWeakPtr<class FAssetThumbnailPool> HookedPool;
	FDelegateHandle PoolRenderedHandle;
	FDelegateHandle PoolFailedHandle;

	/** Hover state. */
	TWeakObjectPtr<UObject> HoveredAsset;
	FString HoveredTag;
	/** Content Browser asset views (found by widget type) and the tiles currently generated in them. */
	TArray<TWeakPtr<SWidget>> AssetViews;
	TArray<TWeakObjectPtr<UObject>> VisibleTiles;
	TMap<FString, TWeakObjectPtr<UObject>> TagCache;
	double LastAssetViewScanTime = 0.0;
	double LastVisibleScanTime = 0.0;
	/** Until then (a few seconds after a folder change) tiles are scanned every frame and missing anims re-queued quickly. */
	double FastScanUntil = 0.0;
	int32 VisibleTileCount = 0;
	/** Keys of VisibleTiles (on screen in a visible Content Browser right now). */
	TSet<FObjectKey> VisibleTileKeys;
	/** True while at least one live tile is on screen; periodic refresh work only runs then. */
	/** -1 = not reported yet, 0 = idle, 1 = active (only transitions are logged). */
	int32 LiveState = -1;
	double LastStatsLogTime = 0.0;
	int32 RequestsSinceStats = 0;
	int32 LastLiveCount = 0;
	FDelegateHandle ObjectChangedHandle;
	int64 SkippedNotRefreshable = 0;
	int64 PoolRenderFailures = 0;
	double LastHoverCheckTime = 0.0;
	double LastHoverRequestTime = 0.0;

	/** Content Browser selection (for Mode = Selected). */
	TArray<FSoftObjectPath> SelectedAssets;
	TMap<FObjectKey, double> SelectedLastRequest;

	/** Animation tiles on screen (loaded or not) and the hovered tile's path (may be unloaded). */
	TArray<FSoftObjectPath> VisibleAnimPaths;
	int32 VisibleDropped = 0;
	/** Pool thumbnail size(s) of each visible tile's viewport (what the tile widget really displays). */
	TMap<FSoftObjectPath, TArray<FIntPoint, TInlineAllocator<2>>> VisibleTileSizes;
	// Tile "announce": the Content Browser tile widget only shows the pool texture (where the frame cache pushes its
	// frames) after the pool reported a successful render for that asset. Anims without a saved package thumbnail,
	// anims whose first pool render happened before they were renderable, and tiles created by scrolling get a
	// failed/no render and would keep the class icon forever (until hovered). We re-send the rendered event for them.
	TSet<FSoftObjectPath> AnnounceNeeded;
	TSet<FSoftObjectPath> PrevVisibleAnims;
	double LastAnnounceSweep = 0.0;
	bool bAnnouncing = false;
	int64 TilesAnnounced = 0;
	TMap<FSoftObjectPath, double> VisibleSince; // newly visible anim tile -> time it appeared (latency stats)
	/** Thumbnail viewport widgets of each visible anim tile (loading spinner position). */
	TMap<FSoftObjectPath, TArray<FBongTileViewport, TInlineAllocator<2>>> VisibleTileViewports;
	TMap<FSoftObjectPath, double> SpinnerSince; // tile waiting for its first frame -> since when
	TSet<FSoftObjectPath> SpinnerCounted;
	TArray<FBongSpinnerDraw> SpinnerDraws;
	TArray<TPair<TWeakPtr<SWindow>, TSharedPtr<SWidget>>> SpinnerOverlays;
	double SpinnerUpdateTime = 0.0;
	int32 SpinnerTiles = 0;
	int64 SpinnerShownTotal = 0;
	int64 SpinnerTimedOut = 0;
	int64 AnnounceLatencyCount = 0;
	double AnnounceLatencySum = 0.0;
	double AnnounceLatencyMax = 0.0;
	FSoftObjectPath HoveredPath;
	TMap<FString, FSoftObjectPath> TagPathCache;
	/** Queued auto-load packages that are animations (they wait for the disk cache and may not need loading). */
	TMap<FName, FSoftObjectPath> PendingAnimPaths;
	double LastRequeueCheck = 0.0;
	/** Memory guard. */
	bool bMemoryPressure = false;
	double LastMemCheck = 0.0;
	double LastPressureRelease = 0.0;
	int32 PressureEvents = 0;
	double LastUsedGB = 0.0;
	double LastFreeGB = 0.0;
	double LastCapGB = 0.0;
	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle PackageSavedHandle;

	/** Shared liveness token so async-load callbacks never touch a shut-down module. */
	TSharedPtr<bool> AliveToken;
};
