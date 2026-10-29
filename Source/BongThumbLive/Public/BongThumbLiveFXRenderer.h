// BongThumbLive - live material / Niagara / Cascade thumbnails.
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectKey.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "BongThumbLiveFXRenderer.generated.h"

class FCanvas;
class FRenderTarget;
class FBongThumbLiveFXScene;

/**
 * Wraps the engine's material thumbnail renderer (UMaterialInstanceThumbnailRenderer, not exported so it
 * cannot be subclassed). Drawing is delegated to a private instance of the stock renderer; the scene time
 * it uses is UThumbnailRenderer::GetTime() (time since app start), so every re-render shows Time/Panner
 * driven materials at the current time. BongThumbLive re-renders them continuously or on hover.
 * The stock renderer is registered again when the plugin unloads.
 */
UCLASS(transient)
class UBongThumbLiveMaterialRenderer : public UDefaultSizedThumbnailRenderer
{
	GENERATED_UCLASS_BODY()

	virtual bool CanVisualizeAsset(UObject* Object) override;
	virtual void GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const override;
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
	virtual EThumbnailRenderFrequency GetThumbnailRenderFrequency(UObject* Object) const override;

private:
	UThumbnailRenderer* GetInner();

	/** Instance of the stock engine renderer (UMaterialInstanceThumbnailRenderer). */
	UPROPERTY(Transient)
	TObjectPtr<UThumbnailRenderer> Inner;
};

/**
 * Live thumbnails for Niagara Systems and Cascade Particle Systems: each visible system gets its own small
 * preview world that is simulated (warm-up + real-time stepping), restarted when it finishes, and framed
 * so the whole effect fits. When live mode is off for the type, drawing falls back to the stock renderer.
 */
UCLASS(transient)
class UBongThumbLiveFXRenderer : public UDefaultSizedThumbnailRenderer
{
	GENERATED_UCLASS_BODY()

	virtual bool CanVisualizeAsset(UObject* Object) override;
	virtual void GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const override;
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
	virtual EThumbnailRenderFrequency GetThumbnailRenderFrequency(UObject* Object) const override;
	virtual void BeginDestroy() override;

	/** Free effect scenes that have not been drawn for IdleSeconds (or all of them when IdleSeconds < 0). */
	static void PruneAllScenes(double IdleSeconds);
	static int32 GetNumLiveScenes();

private:
	UThumbnailRenderer* GetInner(const UObject* Object);
	bool IsLive(const UObject* Object) const;
	/** Live, fully loaded and (for Niagara) compiled: only then the effect is simulated, otherwise the stock renderer draws it. */
	bool IsReadyToSimulate(UObject* Object) const;
	void PruneScenes(double IdleSeconds, int32 MaxScenes);

	UPROPERTY(Transient)
	TObjectPtr<UThumbnailRenderer> InnerNiagara;

	UPROPERTY(Transient)
	TObjectPtr<UThumbnailRenderer> InnerCascade;

	TMap<FObjectKey, FBongThumbLiveFXScene*> Scenes;
};
