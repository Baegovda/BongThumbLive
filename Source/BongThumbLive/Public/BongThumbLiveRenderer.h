// BongThumbLive - live looping animation thumbnails for the Content Browser.
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "Animation/SkeletalMeshActor.h"
#include "BongThumbLiveRenderer.generated.h"

class FCanvas;
class FRenderTarget;
class FBongThumbLiveScene;
class UAnimSequenceBase;
struct FBongCacheEntry;

/** Preview actor whose mesh component is a UDebugSkelMeshComponent (same setup as the engine's anim thumbnail actor). */
UCLASS(transient, NotPlaceable, HideDropdown)
class ABongThumbLiveMeshActor : public ASkeletalMeshActor
{
	GENERATED_UCLASS_BODY()
};

/**
 * Replacement thumbnail renderer for UAnimSequenceBase (sequences, montages, composites).
 * Poses the preview mesh at a time derived from the wall clock, looping over the clip length,
 * and reports EThumbnailRenderFrequency::Realtime so the thumbnail pool keeps re-rendering it.
 */
UCLASS(config = Editor, transient)
class UBongThumbLiveRenderer : public UDefaultSizedThumbnailRenderer
{
	GENERATED_UCLASS_BODY()

	// UThumbnailRenderer
	virtual bool CanVisualizeAsset(UObject* Object) override;
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
	virtual EThumbnailRenderFrequency GetThumbnailRenderFrequency(UObject* Object) const override;

	// UObject
	virtual void BeginDestroy() override;

	/** Renders missing frames of a cache entry off-screen until Deadline (starting at StartFrame); returns frames done. */
	static int32 FillFrames(UAnimSequenceBase* Anim, FBongCacheEntry& Entry, double Deadline, int32 StartFrame);

	/** Frame cache (GPU atlas per animation, see BongThumbLiveCache.h). */
	static FString GetCacheStats();
	static void ClearCache();
	static void ShutdownCache();
	static bool HasCachedFrames(const UObject* Object);
	static void InvalidateCached(const UObject* Object);
	static void DumpCache(const FString& Dir);

	/** Preview-mesh resolver (cached, never loads synchronously). */
	static FString GetResolveStats();
	/** Forget resolved / failed preview meshes (e.g. a skeleton's preview mesh changed). */
	static void ResetPreviewMeshes();
	/** Drop every mesh / animation held by the resolver and the preview scenes (assets about to be deleted). */
	static void ReleaseHeldAssets();
	/** Periodic cleanup: release the preview scene's mesh/anim after a few idle seconds, unused held meshes after 10 s. */
	static void PruneIdle(double Now);

private:
	FBongThumbLiveScene* ThumbnailScene = nullptr;
	double LastSceneUse = 0.0;
};
