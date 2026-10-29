#include "BongThumbLiveFXRenderer.h"
#include "BongThumbLiveModule.h"
#include "BongThumbLiveSettings.h"

#include "CanvasTypes.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
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
		: FThumbnailPreviewScene(FThumbnailPreviewScene::FConstructionValues().SetCreateFloorPlane(false))
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
			World->Tick(LEVELTICK_All, (float)Step);
			Age += Step;
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

	virtual void GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const override
	{
		FVector Center = FVector::ZeroVector;
		double Radius = 100.0;
		if (FramingBox.IsValid)
		{
			Center = FramingBox.GetCenter();
			Radius = FMath::Max(FramingBox.GetExtent().Size(), 1.0);
		}
		else if (const UParticleSystem* PS = Cast<UParticleSystem>(Asset.Get()))
		{
			Radius = FMath::Max(PS->ThumbnailDistance * 0.25, 10.0);
		}
		const double HalfFOV = FMath::DegreesToRadians((double)InFOVDegrees) * 0.5;
		// Same orbit as the engine particle thumbnail; distance so the bounding sphere fits the view.
		OutOrigin = -Center;
		OutOrbitPitch = -11.25f;
		OutOrbitYaw = -157.5f;
		OutOrbitZoom = (float)(Radius / FMath::Sin(HalfFOV) * 1.02);
	}

	TWeakObjectPtr<UObject> Asset;
	TWeakObjectPtr<UFXSystemComponent> Component;
	FBox FramingBox = FBox(ForceInit);
	double Age = 0.0;
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
	if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
	{
		return System->IsReadyToRun();
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

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(RenderTarget, S->GetScene(), FEngineShowFlags(ESFIM_Game))
		.SetTime(UThumbnailRenderer::GetTime())
		.SetAdditionalViewFamily(bAdditionalViewFamily));
	ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
	ViewFamily.EngineShowFlags.MotionBlur = 0;
	RenderViewFamily(Canvas, &ViewFamily, S->CreateView(&ViewFamily, X, Y, Width, Height));

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
