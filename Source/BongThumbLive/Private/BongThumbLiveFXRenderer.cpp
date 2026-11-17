#include "BongThumbLiveFXRenderer.h"
#include "BongThumbLiveModule.h"
#include "BongThumbLiveSettings.h"

#include "CanvasTypes.h"
#include "ClearQuad.h"
#include "RHICommandList.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "TextureResource.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "ThumbnailHelpers.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BongThumbLiveFXRenderer)

/** Game-thread cost of a live draw counts against the per-frame budget (see FBongThumbLiveModule::AddRenderTime). */
struct FBongFXDrawTimer
{
	double Start = FPlatformTime::Seconds();
	~FBongFXDrawTimer() { FBongThumbLiveModule::AddRenderTime(FPlatformTime::Seconds() - Start); }
};

namespace BongThumbLiveFX
{
	static UThumbnailRenderer* MakeStockRenderer(UObject* Outer, const TCHAR* ClassPath)
	{
		UClass* Class = FindObject<UClass>(nullptr, ClassPath);
		if (!Class)
		{
			Class = LoadObject<UClass>(nullptr, ClassPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		}
		if (!Class || !Class->IsChildOf(UThumbnailRenderer::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			return nullptr;
		}
		return NewObject<UThumbnailRenderer>(Outer, Class, NAME_None, RF_Transient);
	}

	static TSet<UBongThumbLiveFXRenderer*> GFXRenderers;

	// Auto-framing: effect bounds (often fixed, hand-set Niagara bounds) are usually far larger than what is drawn, so
	// the effect was small in its tile. The effect is rendered alone into a small target a few times during its first
	// loop; the camera is fitted to the union of the covered pixels so the effect fills the square.
	static int32 GAutoFrame = 1;
	static FAutoConsoleVariableRef CVarAutoFrame(TEXT("BongThumbLive.FXAutoFrame"), GAutoFrame,
		TEXT("1 = fit Niagara/Cascade thumbnails to the pixels the effect covers (default). 0 = fit the effect bounds (old framing)."));
	static float GAutoFrameFill = 0.85f;
	static FAutoConsoleVariableRef CVarAutoFrameFill(TEXT("BongThumbLive.FXAutoFrameFill"), GAutoFrameFill,
		TEXT("Share of the tile the auto-framed effect covers (0.5-1, default 0.85)."));
	static int32 GOpaqueBackground = 1;
	static FAutoConsoleVariableRef CVarOpaqueBackground(TEXT("BongThumbLive.FXOpaqueBackground"), GOpaqueBackground,
		TEXT("1 = live Niagara/Cascade tiles get an opaque (black) background so translucent particles stay visible in the UE 5.8 Content Browser (default)."));
	static constexpr int32 CalibSize = 256;
	static constexpr int32 MaxCalibSamples = 40;     // samples over the first ~4 s of simulation: short flashes (slashes)
	static constexpr double CalibInterval = 0.1;     // simulated seconds between samples
	static constexpr int32 MaxCalibPerFrame = 2;     // calibration samples per engine frame, all effects together
	static int64 GCalibRenders = 0, GCalibSamples = 0, GCalibEmpty = 0, GCompilePolls = 0, GDirectAdvances = 0;
	static double GCalibMsTotal = 0.0, GCalibMsMax = 0.0;
}

//////////////////////////////////////////////////////////////////////////
// Materials

UBongThumbLiveMaterialRenderer::UBongThumbLiveMaterialRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DefaultSizeX = 512;
	DefaultSizeY = 512;
}

UThumbnailRenderer* UBongThumbLiveMaterialRenderer::GetInner()
{
	if (!Inner && !HasAnyFlags(RF_ClassDefaultObject))
	{
		Inner = BongThumbLiveFX::MakeStockRenderer(this, TEXT("/Script/UnrealEd.MaterialInstanceThumbnailRenderer"));
	}
	return Inner;
}

bool UBongThumbLiveMaterialRenderer::CanVisualizeAsset(UObject* Object)
{
	UThumbnailRenderer* R = GetInner();
	return R ? R->CanVisualizeAsset(Object) : false;
}

void UBongThumbLiveMaterialRenderer::GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const
{
	if (Inner)
	{
		Inner->GetThumbnailSize(Object, Zoom, OutWidth, OutHeight);
	}
	else
	{
		Super::GetThumbnailSize(Object, Zoom, OutWidth, OutHeight);
	}
}

void UBongThumbLiveMaterialRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	FBongFXDrawTimer DrawTimer;
	UThumbnailRenderer* R = GetInner();
	if (!R)
	{
		return;
	}
	// The stock renderer sets the view family time to UThumbnailRenderer::GetTime() (app time), so each
	// re-render shows Time-driven material nodes at the current time.
	R->Draw(Object, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);

	if (Object && FBongThumbLiveModule::IsKindEnabled(EBongThumbKind::Material))
	{
		if (FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr())
		{
			Module->NotifyDrawn(Object);
		}
	}
}

EThumbnailRenderFrequency UBongThumbLiveMaterialRenderer::GetThumbnailRenderFrequency(UObject* Object) const
{
	if (FBongThumbLiveModule::IsKindEnabled(EBongThumbKind::Material))
	{
		return EThumbnailRenderFrequency::Realtime;
	}
	return Inner ? Inner->GetThumbnailRenderFrequency(Object) : EThumbnailRenderFrequency::Realtime;
}

//////////////////////////////////////////////////////////////////////////
// Effects

/** One small preview world per visible effect asset. */
class FBongThumbLiveFXScene : public FThumbnailPreviewScene
{
public:
	FBongThumbLiveFXScene(UObject* InAsset)
		// No floor and no sky sphere: in UE 5.8 the thumbnail sky sphere is the checker backdrop, which hid or washed out
		// translucent effects (and covered them completely when the bounds camera sat outside the sphere). The tile
		// gets an opaque black background instead (BongThumbLive.FXOpaqueBackground), like the classic effect thumbnails.
		: FThumbnailPreviewScene(FThumbnailPreviewScene::FConstructionValues().SetCreateFloorPlane(false).SetCreateSkySphere(false))
		, Asset(InAsset)
	{
		bForceAllUsedMipsResident = false;
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(InAsset))
		{
			UNiagaraComponent* NC = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			NC->SetAllowScalability(false);
			NC->SetForceSolo(true);
			NC->bAutoActivate = false;
			NC->SetAsset(System);
			AddComponent(NC, FTransform::Identity);
			NC->Activate(true);
			Component = NC;
		}
		else if (UParticleSystem* PS = Cast<UParticleSystem>(InAsset))
		{
			UParticleSystemComponent* PSC = NewObject<UParticleSystemComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			PSC->bAutoActivate = false;
			PSC->Template = PS;
			AddComponent(PSC, FTransform::Identity);
			PSC->InitializeSystem();
			PSC->ActivateSystem();
			Component = PSC;
		}
	}

	virtual ~FBongThumbLiveFXScene() override
	{
		if (UFXSystemComponent* C = Component.Get())
		{
			C->Deactivate();
			RemoveComponent(C);
		}
	}

	bool IsValidScene() const { return Component.IsValid() && Asset.IsValid(); }

	void Restart()
	{
		if (UNiagaraComponent* NC = Cast<UNiagaraComponent>(Component.Get()))
		{
			NC->ResetSystem();
		}
		else if (UParticleSystemComponent* PSC = Cast<UParticleSystemComponent>(Component.Get()))
		{
			PSC->ResetParticles(true);
			PSC->ResetBurstLists();
			PSC->InitializeSystem();
			PSC->ActivateSystem();
		}
		Age = 0.0;
	}

	bool IsFinished() const
	{
		if (UNiagaraComponent* NC = Cast<UNiagaraComponent>(Component.Get()))
		{
			return NC->IsComplete() || !NC->IsActive();
		}
		if (UParticleSystemComponent* PSC = Cast<UParticleSystemComponent>(Component.Get()))
		{
			return PSC->HasCompleted() || !PSC->IsActive();
		}
		return false;
	}

	static float GetNiagaraAge(const UNiagaraComponent* NC)
	{
		if (NC && NC->GetSystemInstanceController().IsValid())
		{
			if (const FNiagaraSystemInstance* SI = NC->GetSystemInstanceController()->GetSystemInstance_Unsafe())
			{
				return SI->GetAge();
			}
		}
		return 0.f;
	}

	/** Advance the simulation by Seconds (world tick incl. FX system), restarting finished effects. */
	void Simulate(double Seconds, double MaxLoop)
	{
		UWorld* World = GetWorld();
		if (!World || !Component.IsValid())
		{
			return;
		}
		const double MaxStep = 1.0 / 30.0;
		int32 Guard = 0;
		while (Seconds > UE_KINDA_SMALL_NUMBER && Guard++ < 300)
		{
			const double Step = FMath::Min(Seconds, MaxStep);
			Seconds -= Step;
			UNiagaraComponent* NC = Cast<UNiagaraComponent>(Component.Get());
			const float NiagaraAgeBefore = GetNiagaraAge(NC);
			World->Tick(LEVELTICK_All, (float)Step);
			// UE 5.8: ticking the preview world barely advances Niagara (measured: 0.8 s of system age after 9.7 s of
			// world ticks), so effects showed next to no particles. Advance the system directly when the world tick
			// did not (adaptive, so it never runs twice if the world tick does work).
			if (NC && NC->IsActive() && GetNiagaraAge(NC) - NiagaraAgeBefore < (float)Step * 0.5f)
			{
				NC->AdvanceSimulation(1, (float)Step);
				++BongThumbLiveFX::GDirectAdvances;
			}
			Age += Step;
			TotalSimTime += Step;
			if (IsFinished() || (MaxLoop > 0.0 && Age >= MaxLoop))
			{
				Restart();
			}
		}
		UpdateFraming();
	}

	void UpdateFraming()
	{
		UFXSystemComponent* C = Component.Get();
		if (!C)
		{
			return;
		}
		const FBoxSphereBounds B = C->CalcBounds(FTransform::Identity);
		const bool bValid = B.SphereRadius > 0.5 && B.SphereRadius < 1.0e6 && !B.Origin.ContainsNaN();
		if (!bValid)
		{
			return;
		}
		const FBox Box = B.GetBox();
		const bool bStable = GetDefault<UBongThumbLiveSettings>()->bStableFraming;
		if (bStable && FramingBox.IsValid)
		{
			FramingBox += Box;
		}
		else
		{
			FramingBox = Box;
		}
	}

	/** Camera that fits the effect bounds (the pre-auto-frame framing; also the wide calibration camera). */
	void GetBoundsCamera(double HalfFOV, FVector& OutCenter, double& OutZoom) const
	{
		OutCenter = FVector::ZeroVector;
		double Radius = 100.0;
		if (FramingBox.IsValid)
		{
			OutCenter = FramingBox.GetCenter();
			Radius = FMath::Max(FramingBox.GetExtent().Size(), 1.0);
		}
		else if (const UParticleSystem* PS = Cast<UParticleSystem>(Asset.Get()))
		{
			Radius = FMath::Max(PS->ThumbnailDistance * 0.25, 10.0);
		}
		OutZoom = Radius / FMath::Sin(HalfFOV) * 1.02;
	}

	virtual void GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const override
	{
		const double HalfFOV = FMath::DegreesToRadians((double)InFOVDegrees) * 0.5;
		FVector Center;
		double Zoom;
		if (bCalibCamera)
		{
			Center = CalibCenter;
			Zoom = CalibZoom;
		}
		else if (BongThumbLiveFX::GAutoFrame && bFitValid)
		{
			// Fitted on the covered pixels: the union rectangle (camera right/up plane) fills GAutoFrameFill of the view.
			Center = FitRight * (0.5 * (FitMin.X + FitMax.X)) + FitUp * (0.5 * (FitMin.Y + FitMax.Y)) + FitDir * FitDepth;
			const double Half = 0.5 * FMath::Max(FitMax.X - FitMin.X, FitMax.Y - FitMin.Y);
			Zoom = Half / (FMath::Tan(HalfFOV) * FMath::Clamp((double)BongThumbLiveFX::GAutoFrameFill, 0.5, 1.0));
		}
		else
		{
			GetBoundsCamera(HalfFOV, Center, Zoom);
		}
		// Same orbit as the engine particle thumbnail.
		OutOrigin = -Center;
		OutOrbitPitch = -11.25f;
		OutOrbitYaw = -157.5f;
		OutOrbitZoom = (float)FMath::Max(Zoom, 1.0);
	}

	float GetFOVPublic() const { return GetFOV(); }

	// Calibration camera override (only while CalibrateFraming renders).
	bool bCalibCamera = false;
	FVector CalibCenter = FVector::ZeroVector;
	double CalibZoom = 100.0;
	// Fitted framing: union of covered-pixel rectangles in the camera's right/up plane (fixed orbit -> fixed basis).
	bool bFitValid = false;
	FVector FitRight = FVector::RightVector, FitUp = FVector::UpVector, FitDir = FVector::ForwardVector;
	FVector2D FitMin = FVector2D(UE_BIG_NUMBER), FitMax = FVector2D(-UE_BIG_NUMBER);
	double FitDepth = 0.0;
	int32 CalibSamples = 0;
	double NextCalibTime = 0.0;

	TWeakObjectPtr<UObject> Asset;
	TWeakObjectPtr<UFXSystemComponent> Component;
	FBox FramingBox = FBox(ForceInit);
	double Age = 0.0;
	double TotalSimTime = 0.0; // never reset (auto-frame sampling schedule)
	double LastSimTime = 0.0;
	double LastDrawTime = 0.0;
	bool bWarmedUp = false;
};

UBongThumbLiveFXRenderer::UBongThumbLiveFXRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DefaultSizeX = 512;
	DefaultSizeY = 512;
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		BongThumbLiveFX::GFXRenderers.Add(this);
	}
}

UThumbnailRenderer* UBongThumbLiveFXRenderer::GetInner(const UObject* Object)
{
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return nullptr;
	}
	if (Object && Object->IsA<UNiagaraSystem>())
	{
		if (!InnerNiagara)
		{
			InnerNiagara = BongThumbLiveFX::MakeStockRenderer(this, TEXT("/Script/NiagaraEditor.NiagaraSystemThumbnailRenderer"));
		}
		return InnerNiagara;
	}
	if (Object && Object->IsA<UParticleSystem>())
	{
		if (!InnerCascade)
		{
			InnerCascade = BongThumbLiveFX::MakeStockRenderer(this, TEXT("/Script/UnrealEd.ParticleSystemThumbnailRenderer"));
		}
		return InnerCascade;
	}
	return nullptr;
}

bool UBongThumbLiveFXRenderer::IsLive(const UObject* Object) const
{
	// Objects that are still loading are never simulated (the stock renderer handles them until they are ready).
	return Object && FBongThumbLiveModule::IsKindEnabled(FBongThumbLiveModule::GetKind(Object)) && FBongThumbLiveModule::IsObjectSafe(Object);
}

bool UBongThumbLiveFXRenderer::IsReadyToSimulate(UObject* Object) const
{
	if (!IsLive(Object))
	{
		return false;
	}
	if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
	{
		if (System->IsReadyToRun())
		{
			return true;
		}
#if WITH_EDITOR
		// A loaded system's compile request is deferred until someone polls it with bFlushRequestCompile = true. The
		// editor's per-frame poll passes false and only a component activation flushes, but no component exists until
		// the system is ready: systems never opened in this session stayed on the static thumbnail forever. Flush the
		// request here (throttled per system); the compile then runs asynchronously as usual.
		static TMap<FObjectKey, double> LastPoll;
		const double Now = FPlatformTime::Seconds();
		double& Last = LastPoll.FindOrAdd(FObjectKey(System), -1.0);
		if (Now - Last > 0.5)
		{
			Last = Now;
			++BongThumbLiveFX::GCompilePolls;
			System->PollForCompilationComplete(true);
			if (LastPoll.Num() > 4096)
			{
				LastPoll.Reset();
			}
			return System->IsReadyToRun();
		}
#endif
		return false;
	}
	return Object->IsA<UParticleSystem>();
}

bool UBongThumbLiveFXRenderer::CanVisualizeAsset(UObject* Object)
{
	if (IsReadyToSimulate(Object))
	{
		return true;
	}
	UThumbnailRenderer* R = GetInner(Object);
	return R ? R->CanVisualizeAsset(Object) : false;
}

void UBongThumbLiveFXRenderer::GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const
{
	if (IsLive(Object))
	{
		Super::GetThumbnailSize(Object, Zoom, OutWidth, OutHeight);
		return;
	}
	const UThumbnailRenderer* R = (Object && Object->IsA<UNiagaraSystem>()) ? InnerNiagara.Get() : InnerCascade.Get();
	if (R)
	{
		R->GetThumbnailSize(Object, Zoom, OutWidth, OutHeight);
	}
	else
	{
		Super::GetThumbnailSize(Object, Zoom, OutWidth, OutHeight);
	}
}

EThumbnailRenderFrequency UBongThumbLiveFXRenderer::GetThumbnailRenderFrequency(UObject* Object) const
{
	if (IsLive(Object))
	{
		return EThumbnailRenderFrequency::Realtime;
	}
	const UThumbnailRenderer* R = (Object && Object->IsA<UNiagaraSystem>()) ? InnerNiagara.Get() : InnerCascade.Get();
	return R ? R->GetThumbnailRenderFrequency(Object) : EThumbnailRenderFrequency::Realtime;
}

void UBongThumbLiveFXRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	FBongFXDrawTimer DrawTimer;
	if (!Object)
	{
		return;
	}
	if (!IsReadyToSimulate(Object))
	{
		if (UThumbnailRenderer* R = GetInner(Object))
		{
			R->Draw(Object, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
		}
		return;
	}

	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const double Now = FPlatformTime::Seconds();

	FBongThumbLiveFXScene*& Scene = Scenes.FindOrAdd(FObjectKey(Object));
	if (Scene && !Scene->IsValidScene())
	{
		FlushRenderingCommands();
		delete Scene;
		Scene = nullptr;
	}
	if (!Scene)
	{
		Scene = new FBongThumbLiveFXScene(Object);
	}
	FBongThumbLiveFXScene* S = Scene; // the map may rehash in PruneScenes below

	const double Rate = (double)FMath::Clamp(Settings->PlaybackRate, 0.05f, 10.f);
	const double MaxLoop = (double)FMath::Max(0.f, Settings->FXMaxLoopSeconds);
	if (!S->bWarmedUp)
	{
		S->bWarmedUp = true;
		S->Simulate(FMath::Clamp((double)Settings->FXWarmupSeconds, 0.0, 10.0), MaxLoop);
	}
	else
	{
		// Real elapsed time since this effect was last simulated (clamped so a long pause does not fast-forward).
		const double Dt = FMath::Clamp(Now - S->LastSimTime, 0.0, 0.25) * Rate;
		S->Simulate(Dt, MaxLoop);
	}
	S->LastSimTime = Now;
	S->LastDrawTime = Now;
	// Push the simulated particles to the render thread now. Otherwise this happens at the end of the engine frame,
	// and several draws within one frame (automation, DumpThumb) render particles that were never sent.
	if (UWorld* World = S->GetWorld())
	{
		World->SendAllEndOfFrameUpdates();
	}
	// At most MaxCalibPerFrame samples per engine frame across all effects (each is a small render + GPU readback),
	// so a folder full of new effects spreads its calibration over many frames instead of hitching.
	static uint64 CalibFrame = 0;
	static int32 CalibThisFrame = 0;
	if (CalibFrame != GFrameCounter) { CalibFrame = GFrameCounter; CalibThisFrame = 0; }
	if (BongThumbLiveFX::GAutoFrame && S->CalibSamples < BongThumbLiveFX::MaxCalibSamples && S->TotalSimTime >= S->NextCalibTime
		&& CalibThisFrame < BongThumbLiveFX::MaxCalibPerFrame)
	{
		++CalibThisFrame;
		CalibrateFraming(S);
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(RenderTarget, S->GetScene(), FEngineShowFlags(ESFIM_Game))
		.SetTime(UThumbnailRenderer::GetTime())
		.SetAdditionalViewFamily(bAdditionalViewFamily));
	ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
	ViewFamily.EngineShowFlags.MotionBlur = 0;
	RenderViewFamily(Canvas, &ViewFamily, S->CreateView(&ViewFamily, X, Y, Width, Height));
	if (BongThumbLiveFX::GOpaqueBackground && RenderTarget)
	{
		// UE 5.8 composites thumbnails with their alpha. Translucent/additive particles write no alpha, so the effect
		// vanished on the checker background. Make this tile's rectangle opaque (black where nothing is drawn).
		const FIntRect Rect(FMath::Max(X, 0), FMath::Max(Y, 0), FMath::Max(X, 0) + (int32)Width, FMath::Max(Y, 0) + (int32)Height);
		ENQUEUE_RENDER_COMMAND(BongFXOpaqueAlpha)([RenderTarget, Rect](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = RenderTarget->GetRenderTargetTexture();
			if (!Texture || Rect.Area() <= 0)
			{
				return;
			}
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::RTV));
			FRHIRenderPassInfo PassInfo(Texture, ERenderTargetActions::Load_Store);
			RHICmdList.BeginRenderPass(PassInfo, TEXT("BongThumbLiveFXOpaqueAlpha"));
			RHICmdList.SetViewport((float)Rect.Min.X, (float)Rect.Min.Y, 0.f, (float)Rect.Max.X, (float)Rect.Max.Y, 1.f);
			DrawClearQuadAlpha(RHICmdList, 1.0f);
			RHICmdList.EndRenderPass();
		});
	}

	if (FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr())
	{
		Module->NotifyDrawn(Object);
	}

	const int32 MaxScenes = FMath::Clamp(Settings->MaxLiveFXScenes, 1, 256);
	if (Scenes.Num() > MaxScenes)
	{
		PruneScenes(-1.0, MaxScenes);
	}
}

namespace BongThumbLiveFX
{
	/** Covered-pixel rectangle in NDC (-1..1, +Y up). False if nothing was drawn. bTouchesEdge: cut by the view. */
	static bool CoveredRect(const TArray<FColor>& Pixels, int32 Size, FVector2D& OutMin, FVector2D& OutMax, bool& bTouchesEdge)
	{
		// Per-column / per-row counts of lit pixels; the rectangle spans the 1st..99th percentile of the lit pixels on
		// each axis, so a few far-flung sparks do not set the framing (measured: a torch whose sparks fly 10x higher
		// than the flame stayed tiny with a plain bounding box).
		TArray<int32> Cols, Rows;
		Cols.SetNumZeroed(Size);
		Rows.SetNumZeroed(Size);
		int64 Lit = 0;
		bool bEdge = false;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			const FColor* Row = Pixels.GetData() + (int64)Y * Size;
			for (int32 X = 0; X < Size; ++X)
			{
				const FColor C = Row[X];
				if (FMath::Max3(C.R, C.G, C.B) > 32) // above the dither/grain noise of an empty view (measured up to ~10-20)
				{
					++Cols[X]; ++Rows[Y]; ++Lit;
					bEdge |= X == 0 || Y == 0 || X == Size - 1 || Y == Size - 1;
				}
			}
		}
		if (Lit < 12)
		{
			return false; // nothing (or a few stray pixels) drawn
		}
		auto Percentile = [Lit](const TArray<int32>& Counts, double Fraction)
		{
			const int64 Target = (int64)FMath::FloorToInt64(Fraction * (double)Lit);
			int64 Sum = 0;
			for (int32 I = 0; I < Counts.Num(); ++I)
			{
				Sum += Counts[I];
				if (Sum > Target) { return I; }
			}
			return Counts.Num() - 1;
		};
		const int32 X0 = Percentile(Cols, 0.01), X1 = Percentile(Cols, 0.99);
		const int32 Y0 = Percentile(Rows, 0.01), Y1 = Percentile(Rows, 0.99);
		bTouchesEdge = bEdge;
		const double Inv = 2.0 / Size;
		OutMin = FVector2D(X0 * Inv - 1.0, 1.0 - (Y1 + 1) * Inv);
		OutMax = FVector2D((X1 + 1) * Inv - 1.0, 1.0 - Y0 * Inv);
		return true;
	}
}

void UBongThumbLiveFXRenderer::CalibrateFraming(FBongThumbLiveFXScene* S)
{
	using namespace BongThumbLiveFX;
	S->NextCalibTime = S->TotalSimTime + CalibInterval;
	UFXSystemComponent* Comp = S->Component.Get();
	if (!Comp || !Comp->SceneProxy)
	{
		return; // nothing on the render thread yet: try again next draw
	}
	const double StartTime = FPlatformTime::Seconds();
	if (!CalibTarget)
	{
		CalibTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
		CalibTarget->InitCustomFormat(CalibSize, CalibSize, PF_B8G8R8A8, false);
		CalibTarget->ClearColor = FLinearColor::Black;
		CalibTarget->UpdateResourceImmediate(true);
	}
	FTextureRenderTargetResource* Resource = CalibTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return;
	}
	const double HalfFOV = FMath::DegreesToRadians((double)S->GetFOVPublic()) * 0.5;
	const double TanHalf = FMath::Tan(HalfFOV);

	// One render of the effect alone from (Center, Zoom); returns its covered rectangle in plane units around Center.
	FVector Right, Up, Dir;
	auto Measure = [&](const FVector& Center, double Zoom, FVector2D& OutMin, FVector2D& OutMax, bool& bEdge) -> bool
	{
		S->bCalibCamera = true;
		S->CalibCenter = Center;
		S->CalibZoom = Zoom;
		FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
		Canvas.Clear(FLinearColor::Black);
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Resource, S->GetScene(), FEngineShowFlags(ESFIM_Game))
			.SetTime(UThumbnailRenderer::GetTime()));
		Family.EngineShowFlags.DisableAdvancedFeatures();
		Family.EngineShowFlags.MotionBlur = 0;
		Family.EngineShowFlags.Bloom = 0;
		Family.EngineShowFlags.EyeAdaptation = 0;
		FSceneView* View = S->CreateView(&Family, 0, 0, CalibSize, CalibSize);
		S->bCalibCamera = false;
		if (!View)
		{
			return false;
		}
		View->ShowOnlyPrimitives.Emplace();
		View->ShowOnlyPrimitives->Add(Comp->GetPrimitiveSceneId()); // no sky, no floor: everything else stays black
		Right = View->GetViewRight();
		Up = View->GetViewUp();
		Dir = View->GetViewDirection();
		RenderViewFamily(&Canvas, &Family, View);
		Canvas.Flush_GameThread();
		TArray<FColor> Pixels;
		const bool bRead = Resource->ReadPixels(Pixels) && Pixels.Num() == CalibSize * CalibSize;
		++GCalibRenders;
		FVector2D NdcMin, NdcMax;
		if (!bRead || !CoveredRect(Pixels, CalibSize, NdcMin, NdcMax, bEdge))
		{
			return false;
		}
		const double PlaneHalf = FMath::Max(Zoom, 48.0) * TanHalf; // NDC 1 at the pivot's depth (CreateView clamps the orbit to >= 48)
		OutMin = FVector2D(FVector::DotProduct(Center, Right), FVector::DotProduct(Center, Up)) + NdcMin * PlaneHalf;
		OutMax = FVector2D(FVector::DotProduct(Center, Right), FVector::DotProduct(Center, Up)) + NdcMax * PlaneHalf;
		return true;
	};

	// First fit: A = the wide bounds camera (sees the whole effect), then B = zoomed in on A's result for precision
	// (rejected when B cuts the effect at the view edge). Later samples: one render from the current fit with room
	// around it (cheaper and sharper); if the effect reaches that view's edge, fall back to the wide camera.
	FVector WideCenter;
	double WideZoom;
	S->GetBoundsCamera(HalfFOV, WideCenter, WideZoom);
	auto CameraFor = [&](const FVector2D& Min, const FVector2D& Max, double Depth, double Share, FVector& OutCenter, double& OutZoom)
	{
		const FVector2D Mid = 0.5 * (Min + Max);
		const double Half = FMath::Max(0.5 * FMath::Max(Max.X - Min.X, Max.Y - Min.Y), 0.5);
		OutCenter = Right * Mid.X + Up * Mid.Y + Dir * Depth;
		OutZoom = Half / (TanHalf * Share);
	};
	FVector2D AMin, AMax;
	bool bEdgeA = false, bA = false;
	double Depth = 0.0;
	if (S->bFitValid)
	{
		Right = S->FitRight; Up = S->FitUp; Dir = S->FitDir; Depth = S->FitDepth;
		FVector Center;
		double Zoom;
		CameraFor(S->FitMin, S->FitMax, Depth, 0.5, Center, Zoom);
		bA = Measure(Center, Zoom, AMin, AMax, bEdgeA);
		if (bA && bEdgeA)
		{
			bA = Measure(WideCenter, WideZoom, AMin, AMax, bEdgeA);
		}
	}
	else
	{
		bA = Measure(WideCenter, WideZoom, AMin, AMax, bEdgeA);
		Depth = FVector::DotProduct(WideCenter, Dir);
	}
	++S->CalibSamples;
	++GCalibSamples;
	if (bA)
	{
		FVector2D RMin = AMin, RMax = AMax;
		if (!S->bFitValid)
		{
			FVector BCenter;
			double BZoom;
			CameraFor(AMin, AMax, Depth, 0.6, BCenter, BZoom);
			FVector2D BMin, BMax;
			bool bEdgeB = false;
			if (Measure(BCenter, BZoom, BMin, BMax, bEdgeB) && !bEdgeB)
			{
				RMin = BMin; RMax = BMax;
			}
		}
		const bool bUnion = GetDefault<UBongThumbLiveSettings>()->bStableFraming && S->bFitValid;
		S->FitRight = Right; S->FitUp = Up; S->FitDir = Dir; S->FitDepth = Depth;
		S->FitMin = bUnion ? FVector2D(FMath::Min(S->FitMin.X, RMin.X), FMath::Min(S->FitMin.Y, RMin.Y)) : RMin;
		S->FitMax = bUnion ? FVector2D(FMath::Max(S->FitMax.X, RMax.X), FMath::Max(S->FitMax.Y, RMax.Y)) : RMax;
		S->bFitValid = true;
	}
	else
	{
		++GCalibEmpty; // nothing visible at this moment (e.g. between bursts): the next sample decides
	}
	const double Ms = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	GCalibMsTotal += Ms;
	GCalibMsMax = FMath::Max(GCalibMsMax, Ms);
}

static FAutoConsoleCommand GBongFXDebugCmd(TEXT("BongThumbLive.FXDebug"), TEXT("Debug: log the state of every live effect scene."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		for (UBongThumbLiveFXRenderer* R : BongThumbLiveFX::GFXRenderers)
		{
			if (R) { R->LogScenes(); }
		}
	}));

void UBongThumbLiveFXRenderer::LogScenes() const
{
	for (const TPair<FObjectKey, FBongThumbLiveFXScene*>& P : Scenes)
	{
		const FBongThumbLiveFXScene* S = P.Value;
		if (!S) { continue; }
		const UFXSystemComponent* C = S->Component.Get();
		const UNiagaraComponent* NC = Cast<UNiagaraComponent>(C);
		const FBox B = C ? C->CalcBounds(C->GetComponentTransform()).GetBox() : FBox(ForceInit);
		float NiagaraAge = -1.f;
		int32 NumEmitters = -1;
		if (NC && NC->GetSystemInstanceController().IsValid())
		{
			if (const FNiagaraSystemInstance* SI = NC->GetSystemInstanceController()->GetSystemInstance_Unsafe())
			{
				NiagaraAge = SI->GetAge();
				NumEmitters = SI->GetEmitters().Num();
			}
		}
		UE_LOG(LogBongThumbLive, Display, TEXT("FXDebug %s: registered=%d active=%d proxy=%d world=%d | niagara: exec=%d requested=%d complete=%d worldReady=%d controller=%d niagaraAge=%.2f emitters=%d | age=%.2f total=%.2f bounds=%s fit=%d"),
			S->Asset.IsValid() ? *S->Asset->GetName() : TEXT("?"), C && C->IsRegistered(), C && C->IsActive(), C && C->SceneProxy != nullptr, C && C->GetWorld() != nullptr,
			NC ? (int32)NC->GetExecutionState() : -1, NC ? (int32)NC->GetRequestedExecutionState() : -1, NC && NC->IsComplete(), NC && NC->IsWorldReadyToRun(),
			NC && NC->GetSystemInstanceController().IsValid(), NiagaraAge, NumEmitters, S->Age, S->TotalSimTime, *B.ToString(), S->bFitValid);
	}
}

FString UBongThumbLiveFXRenderer::GetFramingStats()
{
	using namespace BongThumbLiveFX;
	return FString::Printf(TEXT("fxFraming: auto=%d samples=%lld renders=%lld empty=%lld avg=%.2f ms max=%.2f ms niagaraCompilePolls=%lld directAdvances=%lld"), GAutoFrame, GCalibSamples, GCalibRenders,
		GCalibEmpty, GCalibSamples ? GCalibMsTotal / GCalibSamples : 0.0, GCalibMsMax, GCompilePolls, GDirectAdvances);
}

void UBongThumbLiveFXRenderer::PruneScenes(double IdleSeconds, int32 MaxScenes)
{
	const double Now = FPlatformTime::Seconds();
	TArray<FObjectKey> ToRemove;
	if (IdleSeconds >= 0.0)
	{
		for (const TPair<FObjectKey, FBongThumbLiveFXScene*>& P : Scenes)
		{
			if (!P.Value || !P.Value->IsValidScene() || Now - P.Value->LastDrawTime > IdleSeconds)
			{
				ToRemove.Add(P.Key);
			}
		}
	}
	if (MaxScenes >= 0 && Scenes.Num() - ToRemove.Num() > MaxScenes)
	{
		TArray<TPair<double, FObjectKey>> ByAge;
		for (const TPair<FObjectKey, FBongThumbLiveFXScene*>& P : Scenes)
		{
			if (!ToRemove.Contains(P.Key))
			{
				ByAge.Emplace(P.Value ? P.Value->LastDrawTime : 0.0, P.Key);
			}
		}
		ByAge.Sort([](const TPair<double, FObjectKey>& A, const TPair<double, FObjectKey>& B) { return A.Key < B.Key; });
		const int32 Excess = Scenes.Num() - ToRemove.Num() - MaxScenes;
		for (int32 i = 0; i < Excess && i < ByAge.Num(); ++i)
		{
			// Never free the scene drawn this very frame.
			if (Now - ByAge[i].Key > 0.001)
			{
				ToRemove.Add(ByAge[i].Value);
			}
		}
	}
	if (IdleSeconds < 0.0 && MaxScenes < 0)
	{
		Scenes.GetKeys(ToRemove);
	}
	if (ToRemove.Num() == 0)
	{
		return;
	}
	FlushRenderingCommands();
	for (const FObjectKey& Key : ToRemove)
	{
		if (FBongThumbLiveFXScene** Found = Scenes.Find(Key))
		{
			delete *Found;
			Scenes.Remove(Key);
		}
	}
}

void UBongThumbLiveFXRenderer::PruneAllScenes(double IdleSeconds)
{
	for (UBongThumbLiveFXRenderer* R : BongThumbLiveFX::GFXRenderers)
	{
		if (R)
		{
			R->PruneScenes(IdleSeconds, IdleSeconds < 0.0 ? -1 : FMath::Clamp(GetDefault<UBongThumbLiveSettings>()->MaxLiveFXScenes, 1, 256));
		}
	}
}

int32 UBongThumbLiveFXRenderer::GetNumLiveScenes()
{
	int32 Num = 0;
	for (UBongThumbLiveFXRenderer* R : BongThumbLiveFX::GFXRenderers)
	{
		Num += R ? R->Scenes.Num() : 0;
	}
	return Num;
}

void UBongThumbLiveFXRenderer::BeginDestroy()
{
	BongThumbLiveFX::GFXRenderers.Remove(this);
	if (Scenes.Num() > 0)
	{
		FlushRenderingCommands();
		for (TPair<FObjectKey, FBongThumbLiveFXScene*>& P : Scenes)
		{
			delete P.Value;
		}
		Scenes.Reset();
	}
	Super::BeginDestroy();
}
