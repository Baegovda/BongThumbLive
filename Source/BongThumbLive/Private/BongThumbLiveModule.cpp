#include "BongThumbLiveModule.h"
#include "BongThumbLiveRenderer.h"
#include "BongThumbLiveFXRenderer.h"
#include "BongThumbLiveSettings.h"
#include "BongThumbLiveSettingsCustomization.h"
#include "BongThumbLiveCache.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "UObject/ObjectSaveContext.h"
#include "HAL/PlatformMemory.h"
#include "RenderingThread.h"
#include <atomic>
#include "Misc/AutomationTest.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "AssetCompilingManager.h"
#include "IAssetCompilingManager.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetThumbnail.h"
#include "ContentBrowserDataSubsystem.h"
#include "GenericPlatform/GenericApplication.h"
#include "IContentBrowserDataModule.h"
#include "Layout/WidgetPath.h"
#include "PropertyEditorModule.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#include "Types/ISlateMetaData.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserItemPath.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "SAssetView.h"
#endif
#include "IContentBrowserSingleton.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Views/STableViewBase.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "ISettingsModule.h"
#include "ISettingsContainer.h"
#include "ISettingsCategory.h"
#include "ISettingsSection.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitter.h"
#include "Particles/ParticleSystem.h"
#include "Settings/ContentBrowserSettings.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Layout/SlateRect.h"

DEFINE_LOG_CATEGORY(LogBongThumbLive);

#define LOCTEXT_NAMESPACE "FBongThumbLiveModule"

static FBongThumbLiveModule* GBongThumbLiveModule = nullptr;

FBongThumbLiveModule* FBongThumbLiveModule::GetPtr()
{
	return GBongThumbLiveModule;
}

// Per-frame render-time accounting (game thread only).
static uint64 GBongRenderFrame = ~0ull;
static double GBongRenderSecThisFrame = 0.0;
/** Moving average of the game-thread cost of one live thumbnail Draw (ms). */
static double GBongAvgDrawMs = 0.5;

void FBongThumbLiveModule::AddRenderTime(double Seconds)
{
	if (GBongRenderFrame != GFrameCounter)
	{
		GBongRenderFrame = GFrameCounter;
		GBongRenderSecThisFrame = 0.0;
	}
	GBongRenderSecThisFrame += Seconds;
	GBongAvgDrawMs = GBongAvgDrawMs * 0.9 + Seconds * 1000.0 * 0.1;
}

double FBongThumbLiveModule::GetRenderMsThisFrame()
{
	return GBongRenderFrame == GFrameCounter ? GBongRenderSecThisFrame * 1000.0 : 0.0;
}

double FBongThumbLiveModule::GetFrameBudgetMs()
{
	return FMath::Clamp((double)GetDefault<UBongThumbLiveSettings>()->FrameBudgetMs, 0.5, 50.0);
}

bool FBongThumbLiveModule::IsOverFrameBudget()
{
	return GetRenderMsThisFrame() >= GetFrameBudgetMs();
}

// Render-thread governor (see CanSceneRender in the header). The render-thread cost of each scene render is measured
// on the render thread itself (timestamps enqueued before / after it), so the per-frame allowance does not depend on
// stats being enabled.
static int32 GBongMaxScenesPerFrame = 16;
static FAutoConsoleVariableRef CVarBongMaxScenesPerFrame(
	TEXT("BongThumbLive.MaxSceneRendersPerFrame"), GBongMaxScenesPerFrame,
	TEXT("Upper limit of off-screen thumbnail scene renders per engine frame (the governor adapts below it to the measured render-thread cost). Default 16."));
static float GBongSceneBudgetMs = 6.f;
static FAutoConsoleVariableRef CVarBongSceneBudgetMs(
	TEXT("BongThumbLive.SceneRenderBudgetMs"), GBongSceneBudgetMs,
	TEXT("Render-thread time (ms per engine frame) thumbnail scene renders may use. Default 6."));
static float GBongSceneBudgetWaitingMs = 0.f;
static FAutoConsoleVariableRef CVarBongSceneBudgetWaitingMs(
	TEXT("BongThumbLive.SceneRenderBudgetWaitingMs"), GBongSceneBudgetWaitingMs,
	TEXT("Render-thread budget while visible anim tiles still wait for their first frame (0 = use SceneRenderBudgetMs)."));
static int32 GBongReleaseAfterCoarse = 2; // A/B 2026-10-30: scrolled-in tiles 0.34 s -> 0.09 s avg
static FAutoConsoleVariableRef CVarBongReleaseAfterCoarse(
	TEXT("BongThumbLive.ReleaseAfterCoarse"), GBongReleaseAfterCoarse,
	TEXT("While visible tiles wait for one of the 12 anim slots (cap unchanged): 1 = free a slot once its coarse frames exist (reloads later), 2 = free a finished+saved anim's slot at once instead of after 3 s."));
static int32 GBongTileTrace = 0;
static FAutoConsoleVariableRef CVarBongTileTrace(TEXT("BongThumbLive.TileTrace"), GBongTileTrace, TEXT("1 = log per-tile timing (BTLT lines)."));
static bool GBongTilesWaiting = false;
static int64 GBongEarlyReleases = 0;
struct FBongTileTraceT { double T[4] = { 0, 0, 0, 0 }; };
static TMap<FName, FBongTileTraceT> GBongTileTraces;
void FBongThumbLiveModule::NoteTileTrace(FName Package, int32 Stage)
{
	if (!GBongTileTrace || Stage < 0 || Stage > 3) { return; }
	if (GBongTileTraces.Num() > 4096) { GBongTileTraces.Reset(); }
	double& V = GBongTileTraces.FindOrAdd(Package).T[Stage];
	if (V == 0.0) { V = FPlatformTime::Seconds(); }
}
static uint64 GBongSceneFrame = ~0ull;
static int32 GBongScenesThisFrame = 0;
static int32 GBongSceneAllowance = 2;
static std::atomic<float> GBongSceneRtMs{ 2.f }; // moving average, written on the render thread
static uint64 GBongSceneRtStart = 0;               // render thread only
static int64 GBongScenesDeferred = 0;
static int64 GBongScenesRendered = 0;

static void BongSceneGovernorFrame()
{
	if (GBongSceneFrame == GFrameCounter)
	{
		return;
	}
	GBongSceneFrame = GFrameCounter;
	GBongScenesThisFrame = 0;
	const float Cost = FMath::Max(0.05f, GBongSceneRtMs.load(std::memory_order_relaxed));
	GBongSceneAllowance = FMath::Clamp(FMath::FloorToInt(FMath::Clamp((GBongTilesWaiting && GBongSceneBudgetWaitingMs > 0.f) ? GBongSceneBudgetWaitingMs : GBongSceneBudgetMs, 0.5f, 100.f) / Cost), 1, FMath::Clamp(GBongMaxScenesPerFrame, 1, 128));
}

bool FBongThumbLiveModule::CanSceneRender()
{
	BongSceneGovernorFrame();
	if (GBongScenesThisFrame < GBongSceneAllowance)
	{
		return true;
	}
	++GBongScenesDeferred;
	return false;
}

void FBongThumbLiveModule::BeginSceneRender()
{
	ENQUEUE_RENDER_COMMAND(BongSceneRenderBegin)([](FRHICommandListImmediate&)
	{
		GBongSceneRtStart = FPlatformTime::Cycles64();
	});
}

void FBongThumbLiveModule::NoteSceneRender()
{
	BongSceneGovernorFrame();
	++GBongScenesThisFrame;
	++GBongScenesRendered;
	ENQUEUE_RENDER_COMMAND(BongSceneRenderEnd)([](FRHICommandListImmediate&)
	{
		if (GBongSceneRtStart != 0)
		{
			const float Ms = (float)FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - GBongSceneRtStart);
			GBongSceneRtStart = 0;
			const float Old = GBongSceneRtMs.load(std::memory_order_relaxed);
			GBongSceneRtMs.store(Old * 0.9f + FMath::Min(Ms, 100.f) * 0.1f, std::memory_order_relaxed);
		}
	});
}

FString FBongThumbLiveModule::GetRenderGovernorStats()
{
	return FString::Printf(TEXT("sceneRenders: %lld, rt %.2f ms each, allowance %d/frame (budget %.1f ms, max %d), deferred=%lld, waiting=%d earlyReleases=%lld"),
		GBongScenesRendered, GBongSceneRtMs.load(), GBongSceneAllowance, GBongSceneBudgetMs, GBongMaxScenesPerFrame, GBongScenesDeferred, GBongTilesWaiting ? 1 : 0, GBongEarlyReleases);
}

float FBongThumbLiveModule::GetLoopTime(const UAnimSequenceBase* Anim)
{
	const float Length = Anim ? Anim->GetPlayLength() : 0.f;
	if (Length <= UE_KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const bool bHovered = GBongThumbLiveModule && GBongThumbLiveModule->IsHovered(Anim);
	const double Fps = bHovered ? (double)FMath::Clamp(Settings->HoverFPS, 1, 120) : (double)FMath::Clamp(Settings->MaxFPS, 1, 60);
	const double Rate = (double)FMath::Clamp(Settings->PlaybackRate, 0.05f, 10.f);
	// Quantise wall-clock to the frame cap, then scale and loop.
	const double Now = FMath::FloorToDouble(FPlatformTime::Seconds() * Fps) / Fps;
	const double T = FMath::Fmod(Now * Rate, (double)Length);
	return (float)FMath::Clamp(T, 0.0, (double)Length);
}

bool FBongThumbLiveModule::IsHovered(const UObject* Asset) const
{
	return Asset && HoveredAsset.Get() == Asset;
}

static bool BongAnyAnimCompiling()
{
	// Cheap (a handful of registered managers); cached per frame anyway.
	static uint64 CachedFrame = ~0ull;
	static bool bCached = false;
	if (CachedFrame == GFrameCounter)
	{
		return bCached;
	}
	CachedFrame = GFrameCounter;
	bCached = false;
	static const FName AnimTypeName(TEXT("UE-AnimationSequence"));
	for (IAssetCompilingManager* Manager : FAssetCompilingManager::Get().GetRegisteredManagers())
	{
		if (Manager && Manager->GetAssetTypeName() == AnimTypeName)
		{
			bCached = Manager->GetNumRemainingAssets() > 0;
			break;
		}
	}
	return bCached;
}

/** Anims whose compressed data was seen finished (no further checks needed); and throttle for the others
 *  (last check time, number of failed checks: the interval grows so a clip that never finishes is not polled forever). */
static TSet<FObjectKey> GBongAnimReady;
static TMap<FObjectKey, TPair<double, int32>> GBongAnimNotReadySince;

static bool BongIsAnimDataReady(const UAnimSequence* Seq)
{
	const FObjectKey Key(Seq);
	if (GBongAnimReady.Contains(Key))
	{
		return true;
	}
	if (!BongAnyAnimCompiling())
	{
		GBongAnimNotReadySince.Remove(Key);
		GBongAnimReady.Add(Key); FBongThumbLiveModule::NoteTileTrace(Seq->GetOutermost()->GetFName(), 2);
		return true;
	}
	const double Now = FPlatformTime::Seconds();
	int32 Tries = 0;
	if (const TPair<double, int32>* Last = GBongAnimNotReadySince.Find(Key))
	{
		Tries = Last->Value;
		if (Now - Last->Key < 0.1) // cheap: poll at most 10x per second
		{
			return false;
		}
	}
	// v2.2: current-platform compressed data present and valid (fast path through the cached platform data; no
	// derived-data key build per poll).
	if (Seq->IsCompressedDataValid())
	{
		GBongAnimNotReadySince.Remove(Key);
		GBongAnimReady.Add(Key); FBongThumbLiveModule::NoteTileTrace(Seq->GetOutermost()->GetFName(), 2);
		return true;
	}
	if (GBongAnimNotReadySince.Num() > 8192)
	{
		GBongAnimNotReadySince.Reset();
	}
	GBongAnimNotReadySince.Add(Key, TPair<double, int32>(Now, Tries + 1));
	return false;
}

static void BongForgetAnimReady(const UObject* Obj)
{
	GBongAnimReady.Remove(FObjectKey(Obj));
	GBongAnimNotReadySince.Remove(FObjectKey(Obj));
}

bool FBongThumbLiveModule::IsObjectSafe(const UObject* Obj)
{
	constexpr EObjectFlags UnsafeFlags = RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_WillBeLoaded
		| RF_BeginDestroyed | RF_FinishDestroyed | RF_ClassDefaultObject | RF_ArchetypeObject;
	auto IsSafeOne = [](const UObject* O)
	{
		return IsValid(O)
			&& !O->HasAnyFlags(UnsafeFlags)
			&& !O->HasAnyInternalFlags(EInternalObjectFlags_AsyncLoading | EInternalObjectFlags::Async | EInternalObjectFlags::Unreachable);
	};
	if (!IsSafeOne(Obj))
	{
		return false;
	}
	if (const UAnimSequence* Seq = Cast<UAnimSequence>(Obj))
	{
		// Compressed animation data is built asynchronously after load. Posing the clip while that task is
		// storing its result races with it (crash in pose extraction): not ready until its data is finished.
		if (!BongIsAnimDataReady(Seq))
		{
			return false;
		}
	}
	const UPackage* Package = Obj->GetPackage();
	if (!Package || Package->HasAnyInternalFlags(EInternalObjectFlags_AsyncLoading | EInternalObjectFlags::Async))
	{
		return false;
	}
	if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
	{
		// Every emitter the system references must be fully loaded as well.
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
			if (Emitter && !IsSafeOne(Emitter))
			{
				return false;
			}
		}
	}
	return true;
}

EBongThumbKind FBongThumbLiveModule::GetKind(const UObject* Obj)
{
	if (!Obj)
	{
		return EBongThumbKind::None;
	}
	if (Obj->IsA<UAnimSequenceBase>()) { return EBongThumbKind::Animation; }
	if (Obj->IsA<UMaterialInterface>()) { return EBongThumbKind::Material; }
	if (Obj->IsA<UNiagaraSystem>()) { return EBongThumbKind::Niagara; }
	if (Obj->IsA<UParticleSystem>()) { return EBongThumbKind::Cascade; }
	return EBongThumbKind::None;
}

EBongThumbKind FBongThumbLiveModule::GetKind(const FAssetData& Asset)
{
	if (Asset.IsInstanceOf(UAnimSequenceBase::StaticClass())) { return EBongThumbKind::Animation; }
	if (Asset.IsInstanceOf(UMaterialInterface::StaticClass())) { return EBongThumbKind::Material; }
	if (Asset.IsInstanceOf(UNiagaraSystem::StaticClass())) { return EBongThumbKind::Niagara; }
	if (Asset.IsInstanceOf(UParticleSystem::StaticClass())) { return EBongThumbKind::Cascade; }
	return EBongThumbKind::None;
}

bool FBongThumbLiveModule::IsKindEnabled(EBongThumbKind Kind)
{
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	if (!S->bEnabled)
	{
		return false;
	}
	switch (Kind)
	{
	case EBongThumbKind::Animation: return S->bAnimateAnimations;
	case EBongThumbKind::Material: return S->bAnimateMaterials;
	case EBongThumbKind::Niagara: return S->bAnimateNiagara;
	case EBongThumbKind::Cascade: return S->bAnimateCascade;
	default: return false;
	}
}

namespace BongThumbLive
{
	struct FRendererBinding
	{
		const TCHAR* AssetClass;
		const TCHAR* StockRenderer;
		UClass* (*OurRenderer)();
		/** Registered by its editor module in FCoreDelegates::OnPostEngineInit: replace only after engine init. */
		bool bDeferred;
	};

	static UClass* AnimRendererClass() { return UBongThumbLiveRenderer::StaticClass(); }
	static UClass* MaterialRendererClass() { return UBongThumbLiveMaterialRenderer::StaticClass(); }
	static UClass* FXRendererClass() { return UBongThumbLiveFXRenderer::StaticClass(); }

	static const FRendererBinding GBindings[] =
	{
		{ TEXT("/Script/Engine.AnimSequenceBase"), TEXT("/Script/UnrealEd.AnimSequenceThumbnailRenderer"), &AnimRendererClass, false },
		{ TEXT("/Script/Engine.Material"), TEXT("/Script/UnrealEd.MaterialInstanceThumbnailRenderer"), &MaterialRendererClass, false },
		{ TEXT("/Script/Engine.MaterialInterface"), TEXT("/Script/UnrealEd.MaterialInstanceThumbnailRenderer"), &MaterialRendererClass, false },
		{ TEXT("/Script/Niagara.NiagaraSystem"), TEXT("/Script/NiagaraEditor.NiagaraSystemThumbnailRenderer"), &FXRendererClass, true },
		{ TEXT("/Script/Engine.ParticleSystem"), TEXT("/Script/UnrealEd.ParticleSystemThumbnailRenderer"), &FXRendererClass, false },
	};
}

void FBongThumbLiveModule::RegisterRenderers(bool bIncludeDeferred)
{
	UThumbnailManager& Manager = UThumbnailManager::Get();
	for (const BongThumbLive::FRendererBinding& B : BongThumbLive::GBindings)
	{
		if (B.bDeferred && !bIncludeDeferred)
		{
			continue;
		}
		UClass* AssetClass = FindObject<UClass>(nullptr, B.AssetClass);
		if (!AssetClass)
		{
			continue;
		}
		Manager.UnregisterCustomRenderer(AssetClass);
		Manager.RegisterCustomRenderer(AssetClass, B.OurRenderer());
		ReplacedRenderers.Add(B.AssetClass, B.StockRenderer);
	}
	bRenderersRegistered = true;
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive renderers registered (animations, materials, Cascade%s)."), bIncludeDeferred ? TEXT(", Niagara systems") : TEXT(""));
}

void FBongThumbLiveModule::UnregisterRenderers()
{
	if (!bRenderersRegistered)
	{
		return;
	}
	bRenderersRegistered = false;
	if (UObjectInitialized() && !IsEngineExitRequested())
	{
		if (UThumbnailManager* Manager = UThumbnailManager::TryGet())
		{
			for (const TPair<FString, FString>& Pair : ReplacedRenderers)
			{
				UClass* AssetClass = FindObject<UClass>(nullptr, *Pair.Key);
				UClass* Stock = FindObject<UClass>(nullptr, *Pair.Value);
				if (AssetClass)
				{
					Manager->UnregisterCustomRenderer(AssetClass);
					if (Stock)
					{
						Manager->RegisterCustomRenderer(AssetClass, Stock);
					}
				}
			}
		}
	}
	ReplacedRenderers.Reset();
}

void FBongThumbLiveModule::StartupModule()
{
	GBongThumbLiveModule = this;

	if (!GIsEditor || IsRunningCommandlet() || !FApp::CanEverRender())
	{
		return;
	}

	GetMutableDefault<UBongThumbLiveSettings>()->MigrateLegacyConfig();

	RegisterRenderers(false);
	// NiagaraEditor registers its own renderers in FCoreDelegates::OnPostEngineInit (after this module
	// loads), so re-apply ours on the first ticks once engine init is complete.
	RegisterRetryTicks = 3;

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FBongThumbLiveModule::Tick), 0.f);

	FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	SelectionHandle = CB.GetOnAssetSelectionChanged().AddRaw(this, &FBongThumbLiveModule::OnContentBrowserSelectionChanged);
	PathChangedHandle = CB.GetOnAssetPathChanged().AddRaw(this, &FBongThumbLiveModule::OnContentBrowserPathChanged);
	AliveToken = MakeShared<bool>(true);
	// Live effect previews hold the asset: release them before an asset delete so it is not "in use".
	PreDeleteHandle = FEditorDelegates::OnAssetsPreDelete.AddLambda([](const TArray<UObject*>&)
	{
		UBongThumbLiveFXRenderer::PruneAllScenes(-1.0);
		UBongThumbLiveRenderer::ReleaseHeldAssets();
	});

	// Settings page: per-asset-type labels drawn in the Content Browser type colour.
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyModule.RegisterCustomClassLayout(TEXT("BongThumbLiveSettings"),
		FOnGetDetailCustomizationInstance::CreateStatic(&FBongThumbLiveSettingsCustomization::MakeInstance));
	bCustomizationRegistered = true;

	HookThumbnailPool();
	// An edited / reimported animation must not keep playing stale cached frames.
	ObjectChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([](UObject* Obj, FPropertyChangedEvent&)
	{
		if (Obj && Obj->IsA<UAnimSequenceBase>())
		{
			UBongThumbLiveRenderer::InvalidateCached(Obj);
			BongForgetAnimReady(Obj); // an edit can trigger a recompression
		}
		else if (Obj && (Obj->IsA<USkeleton>() || Obj->IsA<USkeletalMesh>()))
		{
			UBongThumbLiveRenderer::ResetPreviewMeshes(); // preview mesh or skeleton data may have changed
			if (FBongThumbCache* Cache = FBongThumbCache::GetPtr()) { Cache->InvalidatePackage(Obj->GetOutermost()->GetFName()); }
		}
	});
	RegisterConsoleCommands();

	// Disk cache housekeeping: deleted / renamed assets lose their files; a save re-checks the key.
	FBongThumbCache::Get(); // starts the background scan of the disk cache
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	AssetRemovedHandle = AR.OnAssetRemoved().AddLambda([](const FAssetData& Asset)
	{
		if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
		{
			Cache->OnAssetRemoved(Asset);
		}
	});
	AssetRenamedHandle = AR.OnAssetRenamed().AddLambda([](const FAssetData& Asset, const FString& OldObjectPath)
	{
		if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
		{
			Cache->OnAssetRenamed(Asset, OldObjectPath);
		}
	});
	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddLambda([](const FString&, UPackage* Package, FObjectPostSaveContext)
	{
		if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
		{
			if (Package)
			{
				Cache->OnPackageSaved(Package->GetFName());
			}
		}
	});
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBongThumbLiveModule::RegisterToolbar));
}

void FBongThumbLiveModule::RegisterToolbar()
{
	// Content Browser toolbar: a settings button just left of the lock icon. The lock is created by the dynamic
	// entry "Settings" of section "Settings" (ContentBrowserMenus.cpp), so we insert before that entry.
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.ToolBar"));
	if (!Menu)
	{
		return;
	}
	// Own section placed right before the "Settings" section (which holds only the lock); both are right-aligned,
	// so the button sits directly left of the lock. (Inserting into "Settings" lands after the lock, because the
	// lock is added by a dynamic entry when the toolbar is generated.)
	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("BongThumbLive"), FText::GetEmpty(), FToolMenuInsert(TEXT("Settings"), EToolMenuInsertType::Before));
	Section.Alignment = EToolMenuSectionAlign::Last;
	FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
		TEXT("BongThumbLiveSettings"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			if (ISettingsModule* SM = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
			{
				SM->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("BongThumbLive"));
			}
		})),
		FText::GetEmpty(),
		LOCTEXT("BongThumbLiveToolbarTooltip", "BongThumbLive 설정"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")));
	Section.AddEntry(Entry);
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: Content Browser toolbar button registered."));
}

void FBongThumbLiveModule::HookThumbnailPool()
{
	if (HookedPool.IsValid())
	{
		return;
	}
	UThumbnailManager* Manager = UThumbnailManager::TryGet();
	TSharedPtr<FAssetThumbnailPool> Pool = Manager ? Manager->GetSharedThumbnailPool() : nullptr;
	if (!Pool.IsValid())
	{
		return; // retried from Tick
	}
	HookedPool = Pool;
	PoolRenderedHandle = Pool->OnThumbnailRendered().AddRaw(this, &FBongThumbLiveModule::OnPoolThumbnail);
	PoolFailedHandle = Pool->OnThumbnailRenderFailed().AddLambda([this](const FAssetData& AssetData)
	{
		if (GetKind(AssetData) != EBongThumbKind::None)
		{
			++PoolRenderFailures; // the pool showed the static fallback for a live kind
			if (GetKind(AssetData) == EBongThumbKind::Animation)
			{
				AnnounceNeeded.Add(AssetData.GetSoftObjectPath()); // shown again from cached frames (see TickCache)
			}
		}
		OnPoolThumbnail(AssetData);
	});
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: hooked the shared thumbnail pool (visible-thumbnail auto-load)."));
}

void FBongThumbLiveModule::UnhookThumbnailPool()
{
	if (TSharedPtr<FAssetThumbnailPool> Pool = HookedPool.Pin())
	{
		Pool->OnThumbnailRendered().Remove(PoolRenderedHandle);
		Pool->OnThumbnailRenderFailed().Remove(PoolFailedHandle);
	}
	HookedPool.Reset();
	PoolRenderedHandle.Reset();
	PoolFailedHandle.Reset();
}

void FBongThumbLiveModule::OnPoolThumbnail(const FAssetData& AssetData)
{
	// The pool draws a tile for every asset the Content Browser actually shows (folder view, recursive filter
	// results, search, content drawer, other browsers). Unloaded supported assets cannot animate: load them.
	if (bAnnouncing)
	{
		return; // our own re-sent event (nothing was drawn into the tile; no auto-load for cache-played tiles)
	}
	if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
	{
		Cache->OnPoolRendered(AssetData.GetSoftObjectPath()); // the pool overwrote the tile: push our frame again
	}
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	if (!Settings->bEnabled || !Settings->bAutoLoadVisibleFolder || !AssetData.IsValid() || AssetData.IsAssetLoaded())
	{
		return;
	}
	if (!IsKindEnabled(GetKind(AssetData)) || InFlightPackages.Contains(AssetData.PackageName)
		|| VisiblePending.Contains(AssetData.PackageName) || PendingPackages.Contains(AssetData.PackageName)
		|| ExplicitPending.Contains(AssetData.GetSoftObjectPath()))
	{
		return;
	}
	if (VisibleSeen.Num() > 16384)
	{
		VisibleSeen.Reset();
	}
	VisibleSeen.Add(AssetData.PackageName);
	VisiblePending.Add(AssetData.PackageName); // popped from the back: most recently shown first
	if (GetKind(AssetData) == EBongThumbKind::Animation)
	{
		PendingAnimPaths.Add(AssetData.PackageName, AssetData.GetSoftObjectPath());
	}
	++VisibleQueued;
	++VisibleQueuedSinceLog;
}

bool FBongThumbLiveModule::IsRefreshableNow(UObject* Obj)
{
	// The pool falls back to the static package thumbnail (the stock checker-background image) whenever it is
	// asked to refresh an asset it cannot render right now: shaders compiling, the asset compiling, or no renderer
	// accepting it (CanVisualizeAsset == false). Never ask in those cases so a live tile keeps its last frame.
	if (!Obj || !IsObjectSafe(Obj))
	{
		return false;
	}
	if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
	{
		return false;
	}
	if (const IInterface_AsyncCompilation* Async = Cast<IInterface_AsyncCompilation>(Obj))
	{
		if (Async->IsCompiling())
		{
			return false;
		}
	}
	UThumbnailManager* Manager = UThumbnailManager::TryGet();
	FThumbnailRenderingInfo* Info = Manager ? Manager->GetRenderingInfo(Obj) : nullptr;
	return Info && Info->Renderer;
}

FSoftObjectPath FBongThumbLiveModule::TagToPath(const FString& TagString)
{
	if (const FSoftObjectPath* Cached = TagPathCache.Find(TagString))
	{
		return *Cached;
	}
	FString Internal = TagString;
	if (IContentBrowserDataModule* DataModule = IContentBrowserDataModule::GetPtr())
	{
		if (UContentBrowserDataSubsystem* Subsystem = DataModule->GetSubsystem())
		{
			FName InternalName;
			if (Subsystem->TryConvertVirtualPath(TagString, InternalName) == EContentBrowserPathType::Internal)
			{
				Internal = InternalName.ToString();
			}
		}
	}
	if (!Internal.Contains(TEXT(".")))
	{
		Internal += TEXT(".") + FPackageName::GetShortName(Internal);
	}
	const FSoftObjectPath Path(Internal);
	if (TagPathCache.Num() > 16384)
	{
		TagPathCache.Reset();
	}
	TagPathCache.Add(TagString, Path);
	return Path;
}

UObject* FBongThumbLiveModule::ResolveTileTag(const FString& TagString)
{
	if (const TWeakObjectPtr<UObject>* Cached = TagCache.Find(TagString))
	{
		if (UObject* Obj = Cached->Get())
		{
			return Obj;
		}
	}
	UObject* Obj = TagToPath(TagString).ResolveObject();
	if (Obj)
	{
		if (TagCache.Num() > 8192)
		{
			TagCache.Reset();
		}
		TagCache.Add(TagString, Obj);
	}
	return Obj;
}

static void BongCollectAssetViews(const TSharedRef<SWidget>& Widget, TArray<TWeakPtr<SWidget>>& Out, int32 Depth)
{
	static const FName AssetViewType(TEXT("SAssetView"));
	if (Widget->GetType() == AssetViewType)
	{
		Out.Add(Widget);
		return;
	}
	if (Depth > 96 || !Widget->GetVisibility().IsVisible())
	{
		return;
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 i = 0; Children && i < Children->Num(); ++i)
	{
		BongCollectAssetViews(Children->GetChildAt(i), Out, Depth + 1);
	}
}

/** Sizes of the thumbnail viewports (SAssetThumbnail -> SViewport -> FAssetThumbnail) inside one tile widget. */
static void BongCollectViewportSizes(const TSharedRef<SWidget>& Widget, TArray<FIntPoint, TInlineAllocator<2>>& Out, int32 Depth,
	TArray<TWeakPtr<SWidget>, TInlineAllocator<2>>* OutWidgets = nullptr)
{
	if (Depth > 24)
	{
		return;
	}
	static const FName ViewportType(TEXT("SViewport"));
	if (Widget->GetType() == ViewportType)
	{
		const TSharedPtr<ISlateViewport> Viewport = static_cast<SViewport&>(Widget.Get()).GetViewportInterface().Pin();
		if (Viewport.IsValid())
		{
			const FIntPoint Size = Viewport->GetSize();
			if (Size.X > 0 && Size.Y > 0)
			{
				Out.AddUnique(Size);
				if (OutWidgets)
				{
					OutWidgets->Add(Widget.ToWeakPtr());
				}
			}
		}
		return;
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 i = 0; Children && i < Children->Num(); ++i)
	{
		BongCollectViewportSizes(Children->GetChildAt(i), Out, Depth + 1, OutWidgets);
	}
}

static void BongCollectTileTags(const TSharedRef<SWidget>& Widget, TSet<FString>& Out, int32 Depth,
	TMap<FString, TArray<FIntPoint, TInlineAllocator<2>>>* OutSizes = nullptr,
	TMap<FString, TArray<FBongTileViewport, TInlineAllocator<2>>>* OutViewports = nullptr, const TSharedPtr<SWidget>& View = nullptr)
{
	if (Depth > 64 || !Widget->GetVisibility().IsVisible())
	{
		return;
	}
	if (const TSharedPtr<FTagMetaData> Tag = Widget->GetMetaData<FTagMetaData>())
	{
		const FString TagString = Tag->Tag.ToString();
		if (TagString.StartsWith(TEXT("/")))
		{
			Out.Add(TagString);
			if (OutSizes)
			{
				TArray<FIntPoint, TInlineAllocator<2>> Sizes;
				TArray<TWeakPtr<SWidget>, TInlineAllocator<2>> Viewports;
				BongCollectViewportSizes(Widget, Sizes, 0, OutViewports ? &Viewports : nullptr);
				if (OutViewports && Viewports.Num() > 0)
				{
					TArray<FBongTileViewport, TInlineAllocator<2>>& DstVP = OutViewports->FindOrAdd(TagString);
					for (const TWeakPtr<SWidget>& VP : Viewports)
					{
						DstVP.Add({ VP, View });
					}
				}
				if (Sizes.Num() > 0)
				{
					TArray<FIntPoint, TInlineAllocator<2>>& Dst = OutSizes->FindOrAdd(TagString);
					for (const FIntPoint& S : Sizes)
					{
						Dst.AddUnique(S);
					}
				}
			}
			return; // a tile: nothing more below
		}
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 i = 0; Children && i < Children->Num(); ++i)
	{
		BongCollectTileTags(Children->GetChildAt(i), Out, Depth + 1, OutSizes, OutViewports, View);
	}
}

/** True if the widget is attached (through visible parents) to a visible, non-minimised window. */
static bool BongIsShownInVisibleWindow(const TSharedPtr<SWidget>& Widget)
{
	TSharedPtr<SWidget> Current = Widget;
	for (int32 Guard = 0; Current.IsValid() && Guard < 512; ++Guard)
	{
		if (!Current->GetVisibility().IsVisible())
		{
			return false;
		}
		if (Current->Advanced_IsWindow())
		{
			const SWindow* Window = static_cast<const SWindow*>(Current.Get());
			return Window->IsVisible() && !Window->IsWindowMinimized();
		}
		Current = Current->GetParentWidget();
	}
	return false;
}

void FBongThumbLiveModule::UpdateVisibleTiles(double Now)
{
	// The asset views virtualise their tiles: only the rows inside the scroll viewport (plus a little overscan) exist
	// as widgets. Counting the tile widgets therefore gives the number of thumbnails currently on screen, and it
	// follows scrolling, resizing and thumbnail-size changes automatically.
	// Every 0.2 s; every frame for a few seconds after a folder change (the new tiles appear over several frames).
	if (Now - LastVisibleScanTime < (Now < FastScanUntil ? 0.0 : 0.2) || !FSlateApplication::IsInitialized())
	{
		return;
	}
	LastVisibleScanTime = Now;
	// Full widget-tree walk to find asset views: every 2 s (also while no browser is open - cheap, no rendering).
	if (Now - LastAssetViewScanTime > 2.0)
	{
		LastAssetViewScanTime = Now;
		AssetViews.Reset();
		// Floating (undocked) Content Browser tabs live in CHILD windows of the main window, which are neither in
		// the top-level window list nor in the main window's widget tree: walk the child windows too.
		TFunction<void(const TSharedRef<SWindow>&, int32)> VisitWindow = [&](const TSharedRef<SWindow>& Window, int32 Depth)
		{
			if (Window->IsVisible() && !Window->IsWindowMinimized())
			{
				BongCollectAssetViews(Window, AssetViews, 0);
			}
			if (Depth < 8)
			{
				for (const TSharedRef<SWindow>& Child : Window->GetChildWindows())
				{
					VisitWindow(Child, Depth + 1);
				}
			}
		};
		for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
		{
			VisitWindow(Window, 0);
		}
	}
	TSet<FString> Tags;
	TMap<FString, TArray<FIntPoint, TInlineAllocator<2>>> TagSizes;
	TMap<FString, TArray<FBongTileViewport, TInlineAllocator<2>>> TagViewports;
	for (int32 Index = AssetViews.Num() - 1; Index >= 0; --Index)
	{
		TSharedPtr<SWidget> View = AssetViews[Index].Pin();
		// Closed browsers (widget destroyed), browsers in a background tab / collapsed drawer (detached or hidden
		// ancestor) and minimised windows show nothing: drop them so their tiles stop refreshing right away.
		if (!View.IsValid() || !BongIsShownInVisibleWindow(View))
		{
			AssetViews.RemoveAtSwap(Index);
			continue;
		}
		BongCollectTileTags(View.ToSharedRef(), Tags, 0, &TagSizes, &TagViewports, View);
	}
	VisibleTileCount = Tags.Num();
	VisibleTiles.Reset();
	VisibleTileKeys.Reset();
	VisibleAnimPaths.Reset();
	VisibleTileSizes.Reset();
	VisibleTileViewports.Reset();
	const bool bAnims = IsKindEnabled(EBongThumbKind::Animation);
	FBongThumbCache& Cache = FBongThumbCache::Get();
	for (const FString& Tag : Tags)
	{
		if (bAnims)
		{
			const FSoftObjectPath Path = TagToPath(Tag);
			if (Cache.IsAnimPath(Path, nullptr))
			{
				VisibleAnimPaths.Add(Path); // loaded or not: cached frames play without loading the asset
				if (const TArray<FIntPoint, TInlineAllocator<2>>* Sizes = TagSizes.Find(Tag))
				{
					VisibleTileSizes.Add(Path, *Sizes);
				}
				if (const TArray<FBongTileViewport, TInlineAllocator<2>>* Viewports = TagViewports.Find(Tag))
				{
					VisibleTileViewports.Add(Path, *Viewports);
				}
			}
		}
		if (UObject* Obj = ResolveTileTag(Tag))
		{
			if (IsKindEnabled(GetKind(Obj)))
			{
				VisibleTiles.Add(Obj);
				VisibleTileKeys.Add(FObjectKey(Obj));
			}
		}
	}
}

/**
 * Loading spinner overlay: one hit-test-invisible leaf widget in each window's overlay that shows a Content Browser.
 * It draws a thin rotating arc over every visible anim tile that is still waiting for its first frame. Only a few
 * Slate line elements per waiting tile: no texture, no frame-cache memory, no scene render.
 */
class SBongSpinnerOverlay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SBongSpinnerOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SWindow>& InWindow)
	{
		Window = InWindow;
		SetVisibility(EVisibility::HitTestInvisible);
		SetCanTick(false);
		ForceVolatile(true); // repainted every frame (animation), never cached
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr();
		const double Now = FPlatformTime::Seconds();
		if (!Module || Now - Module->GetSpinnerUpdateTime() > 0.5) // stale (ticker paused): draw nothing
		{
			return LayerId;
		}
		const TSharedPtr<SWindow> Mine = Window.Pin();
		const float Scale = FMath::Max(AllottedGeometry.Scale, 0.01f);
		const UBongThumbLiveSettings* SS = GetDefault<UBongThumbLiveSettings>();
		const EBongSpinnerStyle Style = SS->LoadingSpinnerStyle;
		const double Speed = FMath::Clamp((double)SS->LoadingSpinnerSpeed, 0.1, 5.0);
		const FLinearColor Col = FLinearColor(FColor(SS->LoadingSpinnerColor.R, SS->LoadingSpinnerColor.G, SS->LoadingSpinnerColor.B, 255));
		const float Phase = (float)FMath::Fmod(Now * 1.25 * Speed, 1.0); // 0..1, 1.25 cycles per second at speed 1
		const float Spin = Phase * UE_TWO_PI;
		TArray<FVector2f> Ring;
		TArray<FVector2f> Arc;
		for (const FBongSpinnerDraw& D : Module->GetSpinnerDraws())
		{
			if (!Mine.IsValid() || D.Window.Pin() != Mine)
			{
				continue;
			}
			const float W = D.Tile.Right - D.Tile.Left;
			const float H = D.Tile.Bottom - D.Tile.Top;
			const FVector2f CenterAbs((D.Tile.Left + D.Tile.Right) * 0.5f, (D.Tile.Top + D.Tile.Bottom) * 0.5f);
			const FVector2f C = FVector2f(AllottedGeometry.AbsoluteToLocal(CenterAbs));
			// Radius follows the tile size (any thumbnail size): ~11 % of the shorter side, 6-18 px on screen.
			const float R = FMath::Clamp(FMath::Min(W, H) * 0.11f, 6.0f, 18.0f) / Scale;
			const float Thick = FMath::Max(1.5f, R * 0.2f * Scale);
			const float Fade = FMath::Clamp((float)((D.Age - FBongThumbLiveModule::SpinnerDelaySeconds) / 0.25), 0.0f, 1.0f);
			Ring.Reset();
			for (int32 i = 0; i <= 32; ++i)
			{
				const float A = (float)i / 32.0f * UE_TWO_PI;
				Ring.Add(C + FVector2f(FMath::Cos(A), FMath::Sin(A)) * R);
			}
			Arc.Reset();
			for (int32 i = 0; i <= 12; ++i)
			{
				const float A = Spin + (float)i / 12.0f * UE_TWO_PI * 0.3f; // 108 degree arc
				Arc.Add(C + FVector2f(FMath::Cos(A), FMath::Sin(A)) * R);
			}
			OutDrawElements.PushClip(FSlateClippingZone(D.Clip)); // tiles half scrolled out stay inside the asset view
			const FPaintGeometry Geo = AllottedGeometry.ToPaintGeometry();
			if (Style == EBongSpinnerStyle::Spin) FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geo, Ring, ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.35f * Fade), true, Thick + 1.5f);
			if (Style == EBongSpinnerStyle::Spin) FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Geo, Ring, ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, 0.12f * Fade), true, Thick);
			const FLinearColor Fg(Col.R, Col.G, Col.B, 0.9f * Fade);
			switch (Style)
			{
			case EBongSpinnerStyle::Pulse:
			{
				const float P = 0.5f - 0.5f * FMath::Cos(Phase * UE_TWO_PI);
				TArray<FVector2f> Circ;
				for (int32 i = 0; i <= 32; ++i)
				{
					const float A = (float)i / 32.0f * UE_TWO_PI;
					Circ.Add(C + FVector2f(FMath::Cos(A), FMath::Sin(A)) * (R * (0.35f + 0.65f * P)));
				}
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Circ, ESlateDrawEffect::None, FLinearColor(Col.R, Col.G, Col.B, (0.4f + 0.55f * (1.0f - P)) * Fade), true, Thick);
				break;
			}
			case EBongSpinnerStyle::Dots:
			{
				// Real dots: 8 filled round discs on the ring, the whole ring rotates and the dots shrink/fade
				// behind the head, so it reads as a classic rotating dots spinner (not short bars).
				const float DotR = FMath::Max(1.5f / Scale, R * 0.22f); // local units
				for (int32 k = 0; k < 8; ++k)
				{
					const float T = (float)k / 8.0f;           // 0 = head
					const float A = Spin - T * UE_TWO_PI;       // trail behind the head, whole ring turns
					const FVector2f P0 = C + FVector2f(FMath::Cos(A), FMath::Sin(A)) * R;
					const float Rk = DotR * (1.0f - 0.55f * T);
					// Filled disc: a closed circle of radius Rk/2 stroked with width Rk (in pixels).
					TArray<FVector2f> Disc;
					for (int32 i = 0; i <= 16; ++i)
					{
						const float B = (float)i / 16.0f * UE_TWO_PI;
						Disc.Add(P0 + FVector2f(FMath::Cos(B), FMath::Sin(B)) * (Rk * 0.5f));
					}
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Disc, ESlateDrawEffect::None,
						FLinearColor(Col.R, Col.G, Col.B, (0.25f + 0.7f * (1.0f - T)) * Fade), true, FMath::Max(1.0f, Rk * Scale));
				}
				break;
			}
			case EBongSpinnerStyle::Bar:
			{
				const float X = FMath::Sin(Phase * UE_TWO_PI) * R * 0.6f;
				TArray<FVector2f> Track; Track.Add(C + FVector2f(-R, 0.f)); Track.Add(C + FVector2f(R, 0.f));
				TArray<FVector2f> Seg; Seg.Add(C + FVector2f(X - R * 0.4f, 0.f)); Seg.Add(C + FVector2f(X + R * 0.4f, 0.f));
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Track, ESlateDrawEffect::None, FLinearColor(Col.R, Col.G, Col.B, 0.25f * Fade), true, Thick);
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Seg, ESlateDrawEffect::None, Fg, true, Thick);
				break;
			}
			default:
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Arc, ESlateDrawEffect::None, Fg, true, Thick);
				break;
			}
			OutDrawElements.PopClip();
		}
		return LayerId + 3;
	}

private:
	TWeakPtr<SWindow> Window;
};

static TSharedPtr<SWindow> BongFindParentWindow(const TSharedPtr<SWidget>& Widget)
{
	TSharedPtr<SWidget> Current = Widget;
	for (int32 Guard = 0; Current.IsValid() && Guard < 512; ++Guard)
	{
		if (Current->Advanced_IsWindow())
		{
			return StaticCastSharedPtr<SWindow>(Current);
		}
		Current = Current->GetParentWidget();
	}
	return nullptr;
}

static FSlateRect BongSpinnerClip(const TSharedPtr<SWidget>& Viewport, const TSharedPtr<SWidget>& View)
{
	FSlateRect Clip = View->GetPaintSpaceGeometry().GetLayoutBoundingRect();
	for (TSharedPtr<SWidget> W = Viewport; W.IsValid() && W != View; W = W->GetParentWidget())
	{
		const FName Type = W->GetType();
		const EWidgetClipping Policy = W->GetClipping();
		if (Type == TEXT("SAssetTileView") || Type == TEXT("SAssetListView") || Type == TEXT("SAssetColumnView")
			|| Policy == EWidgetClipping::ClipToBounds || Policy == EWidgetClipping::ClipToBoundsAlways
			|| Policy == EWidgetClipping::ClipToBoundsWithoutIntersecting)
		{
			Clip = Clip.IntersectionWith(W->GetPaintSpaceGeometry().GetLayoutBoundingRect());
		}
	}
	return Clip.IntersectionWith(Viewport->GetPaintSpaceGeometry().GetLayoutBoundingRect());
}

void FBongThumbLiveModule::UpdateSpinners(double Now)
{
	SpinnerDraws.Reset();
	SpinnerUpdateTime = FPlatformTime::Seconds();
	SpinnerTiles = 0;
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	const bool bOn = S->bEnabled && S->bShowLoadingSpinner && S->bUseFrameCache && IsKindEnabled(EBongThumbKind::Animation) && FSlateApplication::IsInitialized();
	TSet<FSoftObjectPath> Waiting;
	if (bOn)
	{
		FBongThumbCache& Cache = FBongThumbCache::Get();
		for (const FSoftObjectPath& Path : VisibleAnimPaths)
		{
			// Only tiles the plugin is actually going to animate (Mode), else e.g. hover mode would spin forever.
			const bool bActive = S->Mode == EBongThumbLiveMode::VisibleInBrowser
				|| (S->Mode == EBongThumbLiveMode::Selected && SelectedAssets.Contains(Path)) || Path == HoveredPath;
			if (!bActive)
			{
				continue;
			}
			const FBongCacheEntry* Entry = Cache.Find(Path);
			if (Entry && Entry->NumValid > 0)
			{
				continue; // first frame is there: gone immediately
			}
			if (UBongThumbLiveRenderer::HasGivenUp(Path))
			{
				continue; // no usable preview mesh: the stock icon stays, nothing to wait for
			}
			const TArray<FBongTileViewport, TInlineAllocator<2>>* Viewports = VisibleTileViewports.Find(Path);
			if (!Viewports)
			{
				continue;
			}
			const double Since = SpinnerSince.FindOrAdd(Path, Now);
			Waiting.Add(Path);
			const double Age = Now - Since;
			if (Age < SpinnerDelaySeconds)
			{
				continue;
			}
			if (Age > SpinnerMaxSeconds)
			{
				if (SpinnerCounted.Remove(Path) > 0)
				{
					++SpinnerTimedOut; // counted once, when it stops spinning
				}
				continue;
			}
			bool bDrew = false;
			for (const FBongTileViewport& Tile : *Viewports)
			{
				const TSharedPtr<SWidget> Viewport = Tile.Viewport.Pin();
				const TSharedPtr<SWidget> View = Tile.View.Pin();
				if (!Viewport.IsValid() || !View.IsValid())
				{
					continue;
				}
				const FSlateRect TileRect = Viewport->GetPaintSpaceGeometry().GetLayoutBoundingRect();
				const FSlateRect ClipRect = BongSpinnerClip(Viewport, View);
				if (TileRect.Right - TileRect.Left < 8.0f || TileRect.Bottom - TileRect.Top < 8.0f || !FSlateRect::DoRectanglesIntersect(TileRect, ClipRect))
				{
					continue;
				}
				const TSharedPtr<SWindow> Window = BongFindParentWindow(Viewport);
				if (!Window.IsValid())
				{
					continue;
				}
				bool bHasOverlay = false;
				for (const TPair<TWeakPtr<SWindow>, TSharedPtr<SWidget>>& Pair : SpinnerOverlays)
				{
					bHasOverlay |= Pair.Key.Pin() == Window;
				}
				if (!bHasOverlay)
				{
					const TSharedRef<SBongSpinnerOverlay> Overlay = SNew(SBongSpinnerOverlay, Window.ToSharedRef());
					Window->AddOverlaySlot()[Overlay];
					SpinnerOverlays.Emplace(Window, Overlay);
				}
				FBongSpinnerDraw& Draw = SpinnerDraws.AddDefaulted_GetRef();
				Draw.Tile = TileRect;
				Draw.Clip = ClipRect;
				Draw.Age = Age;
				Draw.Window = Window;
				bDrew = true;
			}
			if (bDrew)
			{
				++SpinnerTiles;
				if (!SpinnerCounted.Contains(Path))
				{
					SpinnerCounted.Add(Path);
					++SpinnerShownTotal;
				}
			}
		}
	}
	for (auto It = SpinnerSince.CreateIterator(); It; ++It)
	{
		if (!Waiting.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = SpinnerCounted.CreateIterator(); It; ++It)
	{
		if (!Waiting.Contains(*It))
		{
			It.RemoveCurrent();
		}
	}
	for (int32 i = SpinnerOverlays.Num() - 1; i >= 0; --i)
	{
		if (!SpinnerOverlays[i].Key.IsValid())
		{
			SpinnerOverlays.RemoveAtSwap(i); // window closed (its overlay died with it)
		}
	}
}

void FBongThumbLiveModule::RemoveSpinnerOverlays()
{
	for (const TPair<TWeakPtr<SWindow>, TSharedPtr<SWidget>>& Pair : SpinnerOverlays)
	{
		if (const TSharedPtr<SWindow> Window = Pair.Key.Pin())
		{
			if (Pair.Value.IsValid())
			{
				Window->RemoveOverlaySlot(Pair.Value.ToSharedRef());
			}
		}
	}
	SpinnerOverlays.Reset();
	SpinnerDraws.Reset();
}

void FBongThumbLiveModule::UpdateHover(double Now)
{
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const double HoverInterval = 1.0 / (double)FMath::Clamp(Settings->HoverFPS, 1, 120);
	if (Now - LastHoverCheckTime < FMath::Min(HoverInterval, 1.0 / 30.0))
	{
		return;
	}
	LastHoverCheckTime = Now;

	UObject* Found = nullptr;
	FString FoundTag;
	FSoftObjectPath FoundPath;
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& App = FSlateApplication::Get();
		const TSharedPtr<GenericApplication> Platform = App.GetPlatformApplication();
		if (Platform.IsValid() && Platform->IsCursorDirectlyOverSlateWindow() && !App.IsDragDropping())
		{
			const FWidgetPath Path = App.LocateWindowUnderMouse(App.GetCursorPos(), App.GetInteractiveTopLevelWindows());
			// Walk from the deepest widget up; the asset tile/list item's border carries FTagMetaData(virtual path).
			for (int32 i = Path.Widgets.Num() - 1; i >= 0 && i >= Path.Widgets.Num() - 24; --i)
			{
				const TSharedRef<SWidget>& W = Path.Widgets[i].Widget;
				const TSharedPtr<FTagMetaData> Tag = W->GetMetaData<FTagMetaData>();
				if (!Tag.IsValid())
				{
					continue;
				}
				const FString TagString = Tag->Tag.ToString();
				if (!TagString.StartsWith(TEXT("/")))
				{
					continue;
				}
				FoundPath = TagToPath(TagString);
				if (TagString == HoveredTag && HoveredAsset.IsValid())
				{
					Found = HoveredAsset.Get();
					FoundTag = TagString;
					break;
				}
				UObject* Obj = ResolveTileTag(TagString);
				if (Obj && IsKindEnabled(GetKind(Obj)))
				{
					Found = Obj;
					FoundTag = TagString;
				}
				break;
			}
		}
	}
	if (Found != HoveredAsset.Get())
	{
		HoveredAsset = Found;
		LastHoverRequestTime = 0.0;
		UE_LOG(LogBongThumbLive, Verbose, TEXT("BongThumbLive hover: %s"), Found ? *Found->GetPathName() : TEXT("<none>"));
	}
	HoveredTag = FoundTag;
	HoveredPath = FoundPath;
}

void FBongThumbLiveModule::RegisterConsoleCommands()
{
	IConsoleManager& CM = IConsoleManager::Get();
#if WITH_DEV_AUTOMATION_TESTS
	ConsoleObjects.Add(CM.RegisterConsoleCommand(TEXT("BongThumbLive.TestThumbnailSize"), TEXT("QA fixture: Huge or Medium thumbnails; changes only the disposable test browser."),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			UpdateVisibleTiles(FPlatformTime::Seconds() + 1.0);
			for (const TWeakPtr<SWidget>& Weak : AssetViews)
			{
				if (const TSharedPtr<SWidget> View = Weak.Pin())
				{
					static_cast<SAssetView*>(View.Get())->SetCurrentThumbnailSize(Args.Num() && Args[0] == TEXT("Huge") ? EThumbnailSize::Huge : EThumbnailSize::Medium);
				}
			}
		}), ECVF_Default));
#endif

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.LoadFolder"),
		TEXT("Load all supported assets (animations, materials, Niagara, Cascade) under a content path so their thumbnails can animate. Usage: BongThumbLive.LoadFolder /Game/Path"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogBongThumbLive, Warning, TEXT("Usage: BongThumbLive.LoadFolder /Game/Path"));
				return;
			}
			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Args[0]));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			AR.GetAssets(Filter, Assets);
			int32 Loaded = 0, Total = 0;
			for (const FAssetData& A : Assets)
			{
				if (IsKindEnabled(GetKind(A)))
				{
					++Total;
					if (FBongThumbLiveModule* Module = GetPtr())
					{
						Loaded += Module->QueueAssetLoad(A.GetSoftObjectPath()) ? 1 : 0;
					}
				}
			}
			UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive.LoadFolder %s: loaded %d / %d assets."), *Args[0], Loaded, Total);
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.Set"),
		TEXT("Change a BongThumbLive setting (saved per user). Usage: BongThumbLive.Set Enabled|HoverOnly|Mode (OnHover|Selected|VisibleInBrowser)|HoverFPS|MaxFPS|PlaybackRate|MaxRefreshPerTickCap (0=auto)|FrameBudgetMs|UseFrameCache|CacheFPS|CacheResolution|MaxFramesPerAsset|CacheMemoryMB|EnableDiskCache|MaxDiskCacheMB|MaxHeldPreviewMeshes|MinFreeMemoryGB|MaxEditorMemoryGB|StableFraming|LoadAnimsOnSelection|AutoLoadVisibleFolder|AnimateAnimations|AnimateMaterials|AnimateNiagara|AnimateCascade <value>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UBongThumbLiveSettings* S = GetMutableDefault<UBongThumbLiveSettings>();
			if (Args.Num() < 2)
			{
				UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive: Enabled=%d HoverOnly=%d Mode=%s MaxFPS=%d PlaybackRate=%.2f MaxRefreshPerTickCap=%d StableFraming=%d LoadAnimsOnSelection=%d AutoLoadVisibleFolder=%d AnimateAnimations=%d AnimateMaterials=%d AnimateNiagara=%d AnimateCascade=%d"),
					S->bEnabled, S->IsContinuous() ? 0 : 1, S->IsContinuous() ? TEXT("VisibleInBrowser") : TEXT("OnHover"), S->MaxFPS, S->PlaybackRate, S->MaxRefreshPerTickCap, S->bStableFraming, S->bLoadAnimsOnSelection,
					S->bAutoLoadVisibleFolder, S->bAnimateAnimations, S->bAnimateMaterials, S->bAnimateNiagara, S->bAnimateCascade);
				return;
			}
			const FString& Name = Args[0];
			const FString& V = Args[1];
			const bool bBool = V.ToBool();
			if (Name.Equals(TEXT("Enabled"), ESearchCase::IgnoreCase)) { S->bEnabled = bBool; }
			else if (Name.Equals(TEXT("HoverOnly"), ESearchCase::IgnoreCase)) { S->Mode = bBool ? EBongThumbLiveMode::OnHover : EBongThumbLiveMode::VisibleInBrowser; }
			else if (Name.Equals(TEXT("Mode"), ESearchCase::IgnoreCase))
			{
				S->Mode = V.Equals(TEXT("Selected"), ESearchCase::IgnoreCase) ? EBongThumbLiveMode::Selected
					: V.Equals(TEXT("VisibleInBrowser"), ESearchCase::IgnoreCase) ? EBongThumbLiveMode::VisibleInBrowser : EBongThumbLiveMode::OnHover;
			}
			else if (Name.Equals(TEXT("HoverFPS"), ESearchCase::IgnoreCase)) { S->HoverFPS = FMath::Clamp(FCString::Atoi(*V), 1, 120); }
			else if (Name.Equals(TEXT("MaxFPS"), ESearchCase::IgnoreCase)) { S->MaxFPS = FMath::Clamp(FCString::Atoi(*V), 1, 60); }
			else if (Name.Equals(TEXT("PlaybackRate"), ESearchCase::IgnoreCase)) { S->PlaybackRate = FMath::Clamp(FCString::Atof(*V), 0.05f, 10.f); }
			else if (Name.Equals(TEXT("MaxRefreshPerTickCap"), ESearchCase::IgnoreCase)) { S->MaxRefreshPerTickCap = FMath::Clamp(FCString::Atoi(*V), 0, 1024); }
			else if (Name.Equals(TEXT("FrameBudgetMs"), ESearchCase::IgnoreCase)) { S->FrameBudgetMs = FMath::Clamp((float)FCString::Atof(*V), 0.5f, 50.0f); }
			else if (Name.Equals(TEXT("UseFrameCache"), ESearchCase::IgnoreCase)) { S->bUseFrameCache = V.ToBool(); }
			else if (Name.Equals(TEXT("CacheFPS"), ESearchCase::IgnoreCase)) { S->CacheFPS = FMath::Clamp(FCString::Atoi(*V), 1, 120); }
			else if (Name.Equals(TEXT("CacheResolution"), ESearchCase::IgnoreCase)) { S->CacheResolution = FMath::Clamp(FCString::Atoi(*V), 32, 256); }
			else if (Name.Equals(TEXT("MaxFramesPerAsset"), ESearchCase::IgnoreCase)) { S->MaxFramesPerAsset = FMath::Clamp(FCString::Atoi(*V), 8, 1024); }
			else if (Name.Equals(TEXT("CacheMemoryMB"), ESearchCase::IgnoreCase)) { S->CacheMemoryMB = FMath::Clamp(FCString::Atoi(*V), 64, 8192); }
			else if (Name.Equals(TEXT("EnableDiskCache"), ESearchCase::IgnoreCase)) { S->bEnableDiskCache = bBool; }
			else if (Name.Equals(TEXT("MaxDiskCacheMB"), ESearchCase::IgnoreCase)) { S->MaxDiskCacheMB = FMath::Clamp(FCString::Atoi(*V), 64, 1024 * 1024); }
			else if (Name.Equals(TEXT("MaxHeldPreviewMeshes"), ESearchCase::IgnoreCase)) { S->MaxHeldPreviewMeshes = FMath::Clamp(FCString::Atoi(*V), 1, 64); }
			else if (Name.Equals(TEXT("MinFreeMemoryGB"), ESearchCase::IgnoreCase)) { S->MinFreeMemoryGB = FMath::Clamp(FCString::Atoi(*V), 0, 256); }
			else if (Name.Equals(TEXT("MaxEditorMemoryGB"), ESearchCase::IgnoreCase)) { S->MaxEditorMemoryGB = FMath::Clamp(FCString::Atoi(*V), 0, 4096); }
			else if (Name.Equals(TEXT("StableFraming"), ESearchCase::IgnoreCase)) { S->bStableFraming = bBool; }
			else if (Name.Equals(TEXT("LoadAnimsOnSelection"), ESearchCase::IgnoreCase)) { S->bLoadAnimsOnSelection = bBool; }
			else if (Name.Equals(TEXT("AutoLoadVisibleFolder"), ESearchCase::IgnoreCase)) { S->bAutoLoadVisibleFolder = bBool; }
			else if (Name.Equals(TEXT("AnimateAnimations"), ESearchCase::IgnoreCase)) { S->bAnimateAnimations = bBool; }
			else if (Name.Equals(TEXT("AnimateMaterials"), ESearchCase::IgnoreCase)) { S->bAnimateMaterials = bBool; }
			else if (Name.Equals(TEXT("AnimateNiagara"), ESearchCase::IgnoreCase)) { S->bAnimateNiagara = bBool; }
			else if (Name.Equals(TEXT("AnimateCascade"), ESearchCase::IgnoreCase)) { S->bAnimateCascade = bBool; }
			else
			{
				UE_LOG(LogBongThumbLive, Warning, TEXT("BongThumbLive.Set: unknown setting '%s'"), *Name);
				return;
			}
			S->SaveConfig();
			if (FBongThumbLiveModule* M = FBongThumbLiveModule::GetPtr()) { M->OnSettingsChanged(); }
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Set %s = %s"), *Name, *V);
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.Status"),
		TEXT("Debug: print BongThumbLive runtime status (settings section, auto-load folder, tracked thumbnails)."),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
			int32 PerKind[5] = { 0, 0, 0, 0, 0 };
			for (const TPair<FObjectKey, FTracked>& P : Tracked)
			{
				PerKind[(int32)GetKind(P.Value.Asset.Get())]++;
			}
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Status: Container=%s Category=%s Section=%s | Enabled=%d HoverOnly=%d MaxFPS=%d AutoLoadVisibleFolder=%d Anim=%d Mat=%d Niagara=%d Cascade=%d | Folder='%s' queued=%d loaded=%d pending=%d inflight=%d | visible queued=%d loaded=%d pending=%d | tiles on screen=%d live=%d cap=%d skipped(not renderable)=%lld poolFailures=%lld | %s | hover='%s' HoverFPS=%d Mode=%d | tracked=%d (anim=%d mat=%d niagara=%d cascade=%d) fxScenes=%d"),
				*S->GetContainerName().ToString(), *S->GetCategoryName().ToString(), *S->GetSectionName().ToString(),
				S->bEnabled, S->IsContinuous() ? 0 : 1, S->MaxFPS, S->bAutoLoadVisibleFolder, S->bAnimateAnimations, S->bAnimateMaterials, S->bAnimateNiagara, S->bAnimateCascade,
				*AutoLoadFolder, AutoLoadQueued, AutoLoadDone, PendingPackages.Num(), InFlightLoads, VisibleQueued, VisibleDone, VisiblePending.Num(),
				VisibleTileCount, LastLiveCount, S->MaxRefreshPerTickCap, SkippedNotRefreshable, PoolRenderFailures, *UBongThumbLiveRenderer::GetCacheStats(),
				HoveredAsset.IsValid() ? *HoveredAsset->GetName() : TEXT(""), S->HoverFPS, (int32)S->Mode, Tracked.Num(),
				PerKind[1], PerKind[2], PerKind[3], PerKind[4], UBongThumbLiveFXRenderer::GetNumLiveScenes());
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Status: liveActive=%d visibleViews=%d visibleAnimTiles=%d tilesAnnounced=%lld announcePending=%d newTileToShown(n=%lld avg=%.2fs max=%.2fs waiting=%d) FrameBudgetMs=%.1f avgDrawMs=%.2f renderMsThisFrame=%.2f | %s"),
				LiveState, AssetViews.Num(), VisibleAnimPaths.Num(), TilesAnnounced, AnnounceNeeded.Num(), AnnounceLatencyCount, AnnounceLatencyCount ? AnnounceLatencySum / AnnounceLatencyCount : 0.0, AnnounceLatencyMax, VisibleSince.Num(), GetFrameBudgetMs(), GBongAvgDrawMs, GetRenderMsThisFrame(), *(UBongThumbLiveRenderer::GetResolveStats() + TEXT(" | ") + GetRenderGovernorStats()));
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Status: memory: editor %.1f GB (limit %.1f GB), free %.1f GB (minimum %d GB), pressure=%d (events %d), pendingAnimLoads=%d"),
				LastUsedGB, LastCapGB, LastFreeGB, S->MinFreeMemoryGB, bMemoryPressure ? 1 : 0, PressureEvents, PendingAnimPaths.Num());
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Status: spinner: enabled=%d spinnerTiles=%d spinnerDraws=%d spinnerWaiting=%d spinnerShownTotal=%lld spinnerTimedOut=%lld overlays=%d"),
				S->bShowLoadingSpinner ? 1 : 0, SpinnerTiles, SpinnerDraws.Num(), SpinnerSince.Num(), SpinnerShownTotal, SpinnerTimedOut, SpinnerOverlays.Num());
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.ClearDiskCache"),
		TEXT("Delete every cached thumbnail frame on disk (Saved/BongThumbLive/Cache) and in memory. They are rendered again when shown."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FBongThumbCache::Get().ClearDisk();
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.ClearMemoryCache"),
		TEXT("Drop the in-memory (GPU) frame cache; the disk cache is kept and reloaded as needed."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FBongThumbCache::Get().ClearMemory();
			UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: memory frame cache cleared."));
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.OpenSettings"),
		TEXT("Open Project Settings > Plugins > Bong Thumb Live and log where the settings section is registered."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			ISettingsModule* SM = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings"));
			if (!SM)
			{
				UE_LOG(LogBongThumbLive, Warning, TEXT("BongThumbLive.OpenSettings: Settings module not loaded."));
				return;
			}
			SM->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("BongThumbLive"));
			for (const TCHAR* ContainerName : { TEXT("Project"), TEXT("Editor") })
			{
				bool bFound = false;
				if (TSharedPtr<ISettingsContainer> Container = SM->GetContainer(ContainerName))
				{
					if (TSharedPtr<ISettingsCategory> Category = Container->GetCategory(TEXT("Plugins")))
					{
						bFound = Category->GetSection(TEXT("BongThumbLive"), true).IsValid();
					}
				}
				UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.OpenSettings: section %s/Plugins/BongThumbLive registered=%d"), ContainerName, bFound ? 1 : 0);
			}
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.ScrollBrowser"),
		TEXT("Debug: scroll every visible Content Browser asset view. Usage: BongThumbLive.ScrollBrowser <rows to add | top | bottom>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const FString Arg = Args.Num() > 0 ? Args[0] : TEXT("1");
			LastAssetViewScanTime = 0.0;
			LastVisibleScanTime = 0.0;
			UpdateVisibleTiles(FPlatformTime::Seconds());
			int32 Scrolled = 0;
			TFunction<void(const TSharedRef<SWidget>&, int32)> Visit = [&](const TSharedRef<SWidget>& W, int32 Depth)
			{
				const FString Type = W->GetTypeAsString();
				if (Type.StartsWith(TEXT("SAssetTileView")) || Type.StartsWith(TEXT("SAssetListView")) || Type.StartsWith(TEXT("SAssetColumnView")))
				{
					STableViewBase* View = static_cast<STableViewBase*>(&W.Get());
					if (Arg == TEXT("top")) { View->ScrollToTop(); }
					else if (Arg == TEXT("bottom")) { View->ScrollToBottom(); }
					else { View->SetScrollOffset(FMath::Max(0.f, View->GetScrollOffset() + FCString::Atof(*Arg))); }
					++Scrolled;
					return;
				}
				FChildren* Children = Depth < 64 ? W->GetChildren() : nullptr;
				for (int32 i = 0; Children && i < Children->Num(); ++i)
				{
					Visit(Children->GetChildAt(i), Depth + 1);
				}
			};
			for (const TWeakPtr<SWidget>& Weak : AssetViews)
			{
				if (TSharedPtr<SWidget> View = Weak.Pin())
				{
					Visit(View.ToSharedRef(), 0);
				}
			}
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.ScrollBrowser %s: %d view(s) scrolled"), *Arg, Scrolled);
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.DumpCache"),
		TEXT("Debug: save a few cached frames (with alpha) as PNGs. Usage: BongThumbLive.DumpCache C:/dir"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Dir = FString::Join(Args, TEXT(" ")).TrimStartAndEnd().TrimQuotes();
			UBongThumbLiveRenderer::DumpCache(Dir.IsEmpty() ? FPaths::ProjectSavedDir() : Dir);
		}),
		ECVF_Default));

	ConsoleObjects.Add(CM.RegisterConsoleCommand(
		TEXT("BongThumbLive.CaptureEditorWindow"),
		TEXT("Debug: save a PNG of the window showing a Content Browser (else the active or main editor window). Usage: BongThumbLive.CaptureEditorWindow C:/path/shot.png"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			FString Path = FString::Join(Args, TEXT(" ")).TrimStartAndEnd().TrimQuotes();
			if (Path.IsEmpty())
			{
				Path = FPaths::ProjectSavedDir() / TEXT("BongThumbLive_Capture.png");
			}
			if (!FSlateApplication::IsInitialized())
			{
				return;
			}
			TSharedPtr<SWindow> Window;
			for (const TWeakPtr<SWidget>& Weak : AssetViews)
			{
				if (TSharedPtr<SWidget> View = Weak.Pin())
				{
					Window = FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef());
					if (Window.IsValid())
					{
						break;
					}
				}
			}
			if (!Window.IsValid())
			{
				Window = FSlateApplication::Get().GetActiveTopLevelRegularWindow();
			}
			if (!Window.IsValid())
			{
				const TArray<TSharedRef<SWindow>>& Windows = FSlateApplication::Get().GetTopLevelWindows();
				if (Windows.Num() > 0)
				{
					Window = Windows[0];
				}
			}
			if (!Window.IsValid())
			{
				UE_LOG(LogBongThumbLive, Warning, TEXT("CaptureEditorWindow: no window."));
				return;
			}
			TArray<FColor> Pixels;
			FIntVector Size;
			if (FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) && Pixels.Num() == Size.X * Size.Y)
			{
				for (FColor& C : Pixels)
				{
					C.A = 255;
				}
				FImageView Image(Pixels.GetData(), Size.X, Size.Y);
				const bool bSaved = FImageUtils::SaveImageByExtension(*Path, Image);
				UE_LOG(LogBongThumbLive, Log, TEXT("CaptureEditorWindow: %s (%dx%d) -> %s"), bSaved ? TEXT("saved") : TEXT("FAILED"), Size.X, Size.Y, *Path);
			}
			else
			{
				UE_LOG(LogBongThumbLive, Warning, TEXT("CaptureEditorWindow: TakeScreenshot failed."));
			}
		}),
		ECVF_Default));
}

void FBongThumbLiveModule::ShutdownModule()
{
	for (IConsoleObject* Obj : ConsoleObjects)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Obj);
	}
	ConsoleObjects.Reset();

	if (ObjectChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectChangedHandle);
		ObjectChangedHandle.Reset();
	}
	if (IAssetRegistry* AR = IAssetRegistry::Get())
	{
		AR->OnAssetRemoved().Remove(AssetRemovedHandle);
		AR->OnAssetRenamed().Remove(AssetRenamedHandle);
	}
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	if (UObjectInitialized() && UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
	RemoveSpinnerOverlays();
	UBongThumbLiveRenderer::ShutdownCache(); // waits for pending disk writes and saves the cache index

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (FContentBrowserModule* CB = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
	{
		if (SelectionHandle.IsValid())
		{
			CB->GetOnAssetSelectionChanged().Remove(SelectionHandle);
		}
		if (PathChangedHandle.IsValid())
		{
			CB->GetOnAssetPathChanged().Remove(PathChangedHandle);
		}
	}
	SelectionHandle.Reset();
	PathChangedHandle.Reset();
	if (PreDeleteHandle.IsValid())
	{
		FEditorDelegates::OnAssetsPreDelete.Remove(PreDeleteHandle);
		PreDeleteHandle.Reset();
	}
	AliveToken.Reset();
	PendingPackages.Reset();
	VisiblePending.Reset();
	UnhookThumbnailPool();
	if (bCustomizationRegistered)
	{
		bCustomizationRegistered = false;
		if (FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			PropertyModule->UnregisterCustomClassLayout(TEXT("BongThumbLiveSettings"));
		}
	}

	UnregisterRenderers();
	Tracked.Reset();
	GBongThumbLiveModule = nullptr;
}

void FBongThumbLiveModule::NotifyDrawn(UObject* Asset)
{
	// Only tiles that are on screen are tracked (see Tick). Thumbnails drawn anywhere else are not added, so a
	// draw can never keep an asset in the live set by itself.
	if (FTracked* Entry = Tracked.Find(FObjectKey(Asset)))
	{
		Entry->LastDrawTime = FPlatformTime::Seconds();
	}
}

bool FBongThumbLiveModule::Tick(float DeltaTime)
{
	if (RegisterRetryTicks > 0 && --RegisterRetryTicks == 0)
	{
		// Engine init (incl. OnPostEngineInit, where NiagaraEditor registers its renderer) is done now.
		RegisterRenderers(true);
	}
	if (!HookedPool.IsValid())
	{
		HookThumbnailPool();
	}

	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const double Now = FPlatformTime::Seconds();
	UpdateMemoryGuard(Now);
	BongOwned::TickGC(Now);
	UpdateFolderAutoLoad(Now);
	PumpAutoLoads();
	if (VisibleQueuedSinceLog > 0 && Now - LastVisibleLogTime > 2.0)
	{
		LastVisibleLogTime = Now;
		UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive visible auto-load: +%d shown-but-unloaded assets queued (total queued=%d loaded=%d pending=%d inflight=%d)."),
			VisibleQueuedSinceLog, VisibleQueued, VisibleDone, VisiblePending.Num(), InFlightLoads);
		VisibleQueuedSinceLog = 0;
	}
	if (Now - LastPruneTime > 2.0)
	{
		LastPruneTime = Now;
		// Free effect preview worlds whose thumbnail has not been drawn for a while (scrolled away, folder changed, PIE).
		UBongThumbLiveFXRenderer::PruneAllScenes(15.0);
		// Let the shared animation preview scene drop its mesh/anim once nothing animated has been drawn for a while.
		UBongThumbLiveRenderer::PruneIdle(Now);
	}
	auto SetLiveActive = [this](bool bActive, const TCHAR* Reason)
	{
		if ((bActive ? 1 : 0) != LiveState)
		{
			LiveState = bActive ? 1 : 0;
			if (bActive)
			{
				LastStatsLogTime = FPlatformTime::Seconds(); // the stats interval starts now (not at editor start)
				RequestsSinceStats = 0;
				UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: live refresh active (%d live tile(s) on screen in %d visible asset view(s))."), Tracked.Num(), AssetViews.Num());
			}
			else
			{
				UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: live refresh idle (%s) - periodic thumbnail work stopped."), Reason);
			}
		}
	};
	const bool bBlocked = !Settings->bEnabled
		|| (Settings->bRespectRealTimeThumbnailsToggle && !GetDefault<UContentBrowserSettings>()->RealTimeThumbnails)
		|| (GEditor && (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor)); // same rule as the pool: no live thumbnails during PIE/SIE
	if (bBlocked)
	{
		HoveredAsset.Reset();
		HoveredPath.Reset();
		Tracked.Reset();
		Probed.Reset();
		SetLiveActive(false, TEXT("disabled, real-time thumbnails off or PIE"));
		TickCache(Now, true);
		return true;
	}

	UThumbnailManager* Manager = UThumbnailManager::TryGet();
	if (!Manager)
	{
		return true;
	}

	// Hovered tile: re-rendered at Hover FPS, outside the per-frame budget. Requested last so it is rendered
	// first (the pool's render stack is LIFO and takes priority over its own real-time list).
	UpdateHover(Now);
	UpdateVisibleTiles(Now);
	TickCache(Now, false);
	UpdateSpinners(Now);
	auto RequestHovered = [&]()
	{
		UObject* Hovered = HoveredAsset.Get();
		if (!Hovered || !IsRefreshableNow(Hovered))
		{
			return;
		}
		if (Settings->bUseFrameCache && GetKind(Hovered) == EBongThumbKind::Animation)
		{
			return; // played (and filled first) by the frame cache at Hover FPS
		}
		const double HoverInterval = 1.0 / (double)FMath::Clamp(Settings->HoverFPS, 1, 120);
		if (Now - LastHoverRequestTime >= HoverInterval * 0.95)
		{
			LastHoverRequestTime = Now;
			Manager->GetOnThumbnailDirtied().Broadcast(FSoftObjectPath(Hovered));
		}
	};

	if (Settings->Mode == EBongThumbLiveMode::OnHover)
	{
		Tracked.Reset();
		Probed.Reset();
		SetLiveActive(false, TEXT("hover-only mode"));
		RequestHovered();
		return true;
	}

	// Live set = ONLY the tiles currently on screen in a visible Content Browser asset view (re-counted on
	// scroll/resize/zoom; closed, hidden or background-tab browsers drop out). Nothing else - loaded assets,
	// thumbnails drawn elsewhere, selected assets that are scrolled away - is refreshed periodically, so all
	// periodic work stops as soon as no browser shows a live tile.
	const bool bSelectedMode = Settings->Mode == EBongThumbLiveMode::Selected;
	TSet<FObjectKey> SelectedKeys;
	if (bSelectedMode)
	{
		for (const FSoftObjectPath& Path : SelectedAssets)
		{
			if (UObject* Obj = Path.ResolveObject())
			{
				SelectedKeys.Add(FObjectKey(Obj));
			}
		}
	}
	for (auto It = Tracked.CreateIterator(); It; ++It)
	{
		if (!It.Value().Asset.IsValid() || !VisibleTileKeys.Contains(It.Key()) || (bSelectedMode && !SelectedKeys.Contains(It.Key())))
		{
			It.RemoveCurrent();
		}
	}
	for (const TWeakObjectPtr<UObject>& Weak : VisibleTiles)
	{
		if (UObject* Obj = Weak.Get())
		{
			const FObjectKey Key(Obj);
			if (bSelectedMode && !SelectedKeys.Contains(Key))
			{
				continue;
			}
			FTracked& Entry = Tracked.FindOrAdd(Key);
			if (!Entry.Asset.IsValid())
			{
				Entry.Asset = Obj;
			}
			Entry.LastDrawTime = FMath::Max(Entry.LastDrawTime, Now);
		}
	}
	LastLiveCount = Tracked.Num();
	if (Tracked.Num() == 0)
	{
		SetLiveActive(false, AssetViews.Num() == 0 ? TEXT("no visible Content Browser") : TEXT("no live tile on screen"));
		RequestHovered();
		return true;
	}
	SetLiveActive(true, TEXT(""));

	// Time-sliced refresh: each frame, the tiles whose refresh is due (MaxFPS) are requested, least recently
	// refreshed first, but only as many as fit in the per-frame budget (measured cost of one Draw); the rest
	// follow on the next frames. Cheap checks first, IsRefreshableNow only for tiles that are due.
	const double Interval = 1.0 / (double)FMath::Clamp(Settings->MaxFPS, 1, 60);
	UObject* const HoveredObj = HoveredAsset.Get();
	TArray<FTracked*> Due;
	for (TPair<FObjectKey, FTracked>& Pair : Tracked)
	{
		FTracked& Entry = Pair.Value;
		UObject* Obj = Entry.Asset.Get();
		if (!Obj || Obj == HoveredObj || Now - Entry.LastRequestTime < Interval * 0.95)
		{
			continue;
		}
		if (Settings->bUseFrameCache && GetKind(Obj) == EBongThumbKind::Animation)
		{
			continue; // animations: GPU playback from the frame cache (TickCache), not a pool re-render
		}
		if (!IsRefreshableNow(Obj))
		{
			++SkippedNotRefreshable;
			continue; // keep showing its last frame instead of letting the pool fall back to the stock thumbnail
		}
		Due.Add(&Entry);
	}
	Due.Sort([](const FTracked& A, const FTracked& B) { return A.LastRequestTime < B.LastRequestTime; });
	const double BudgetMs = GetFrameBudgetMs();
	int32 Allowed = FMath::Clamp(FMath::FloorToInt(BudgetMs / FMath::Max(GBongAvgDrawMs, 0.02)), 1, 64);
	const int32 Cap = Settings->MaxRefreshPerTickCap; // 0 = budget only
	if (Cap > 0)
	{
		Allowed = FMath::Min(Allowed, Cap);
	}
	const int32 Max = FMath::Min(Due.Num(), Allowed);
	for (int32 i = 0; i < Max; ++i)
	{
		Due[i]->LastRequestTime = Now;
		// FAssetThumbnailPool::OnThumbnailDirtied -> RefreshThumbnailsFor: re-queues every live
		// thumbnail of this asset (any size); the pool renders them within its per-frame time budget.
		Manager->GetOnThumbnailDirtied().Broadcast(FSoftObjectPath(Due[i]->Asset.Get()));
	}
	RequestsSinceStats += Max;
	if (Now - LastStatsLogTime > 30.0)
	{
		UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive live: %d tiles, %d due, %d requests in %.1f s, avg draw %.2f ms, budget %.1f ms/frame."),
			Tracked.Num(), Due.Num(), RequestsSinceStats, Now - LastStatsLogTime, GBongAvgDrawMs, BudgetMs);
		LastStatsLogTime = Now;
		RequestsSinceStats = 0;
	}
	RequestHovered();
	return true;
}
void FBongThumbLiveModule::TickCache(double Now, bool bBlocked)
{
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	FBongThumbCache& Cache = FBongThumbCache::Get();
	FBongThumbCache::FTickParams Params;
	Params.Now = Now;
	Params.Pool = HookedPool.Pin();
	const bool bAnims = !bBlocked && IsKindEnabled(EBongThumbKind::Animation);
	if (bAnims && Settings->bUseFrameCache)
	{
		const bool bHoveredIsAnim = !HoveredPath.IsNull() && Cache.IsAnimPath(HoveredPath, nullptr);
		if (bHoveredIsAnim)
		{
			Params.ActivePaths.Add(HoveredPath);
			Params.Hovered = HoveredPath;
		}
		if (Settings->Mode == EBongThumbLiveMode::VisibleInBrowser)
		{
			for (const FSoftObjectPath& Path : VisibleAnimPaths)
			{
				Params.ActivePaths.AddUnique(Path);
			}
		}
		else if (Settings->Mode == EBongThumbLiveMode::Selected)
		{
			for (const FSoftObjectPath& Path : VisibleAnimPaths)
			{
				if (SelectedAssets.Contains(Path))
				{
					Params.ActivePaths.AddUnique(Path);
				}
			}
		}
	}
	Params.bAllowFill = !bMemoryPressure;
	if (Params.ActivePaths.Num() > 0)
	{
		Params.TileSizes = VisibleTileSizes;
	}
	const double LeftMs = FMath::Max(0.0, GetFrameBudgetMs() - GetRenderMsThisFrame());
	Params.FillDeadline = FPlatformTime::Seconds() + LeftMs / 1000.0;
	Cache.Tick(Params);

	// Animations on screen are in use (also without the frame cache).
	if (!bBlocked)
	{
		for (const TWeakObjectPtr<UObject>& Weak : VisibleTiles)
		{
			if (const UObject* Object = Weak.Get()) { BongOwned::Touch(Object->GetOutermost()->GetFName(), Now); }
		}
	}
	if (bAnims)
	{
		for (const FSoftObjectPath& Path : VisibleAnimPaths)
		{
			BongOwned::Touch(FName(*Path.GetLongPackageName()), Now);
		}
	}
	// Tiles that show the class icon although the cache has frames for them: tell their tile widgets the pool
	// texture is valid (the cache pushes its frames into that texture). Newly visible tiles (scrolling, recycled
	// widgets, folder/filter change) and failed pool renders right away; every visible anim once per second as
	// a safety net. Cheap: a delegate broadcast per tile, nothing is rendered or loaded.
	if (bAnims && Settings->bUseFrameCache && Params.Pool.IsValid())
	{
		TSet<FSoftObjectPath> NowVisible;
		NowVisible.Reserve(VisibleAnimPaths.Num());
		const bool bSweep = Now - LastAnnounceSweep > 1.0;
		if (bSweep)
		{
			LastAnnounceSweep = Now;
		}
		for (const FSoftObjectPath& Path : VisibleAnimPaths)
		{
			NowVisible.Add(Path);
			if (!PrevVisibleAnims.Contains(Path))
			{
				AnnounceNeeded.Add(Path);
				VisibleSince.Add(Path, Now);
			}
			else if (bSweep)
			{
				AnnounceNeeded.Add(Path);
			}
		}
		PrevVisibleAnims = MoveTemp(NowVisible);
		GBongTilesWaiting = VisibleSince.Num() > 0;
		for (auto It = VisibleSince.CreateIterator(); It; ++It)
		{
			if (!PrevVisibleAnims.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = AnnounceNeeded.CreateIterator(); It; ++It)
		{
			const FSoftObjectPath Path = *It;
			if (!PrevVisibleAnims.Contains(Path))
			{
				VisibleSince.Remove(Path);
				It.RemoveCurrent(); // off screen: announced again when it comes back
				continue;
			}
			const FBongCacheEntry* Entry = Cache.Find(Path);
			if (!Entry || Entry->NumValid <= 0)
			{
				continue; // nothing to show yet: keep waiting
			}
			FAssetData Data;
			if (Cache.IsAnimPath(Path, &Data))
			{
				TGuardValue<bool> Guard(bAnnouncing, true);
				Params.Pool->OnThumbnailRendered().Broadcast(Data);
				++TilesAnnounced;
			}
			double Since = 0.0;
			if (VisibleSince.RemoveAndCopyValue(Path, Since))
			{
				const double Latency = Now - Since; // tile appeared -> its tile shows cached frames
				++AnnounceLatencyCount;
				AnnounceLatencySum += Latency;
				AnnounceLatencyMax = FMath::Max(AnnounceLatencyMax, Latency);
				if (GBongTileTrace)
				{
					const FName Pkg(*Path.GetLongPackageName());
					const FBongTileTraceT* Tr = GBongTileTraces.Find(Pkg);
					auto Rel = [&](int32 i) { return (Tr && Tr->T[i] > 0.0) ? Tr->T[i] - Since : -999.0; };
					UE_LOG(LogBongThumbLive, Display, TEXT("BTLT tile=%s shown=%.3f q=%.3f l=%.3f r=%.3f f=%.3f"), *Pkg.ToString(), Latency, Rel(0), Rel(1), Rel(2), Rel(3));
				}
			}
			It.RemoveCurrent();
		}
	}

	// Release what the plugin itself loaded once it is no longer needed (the key fix for the unbounded growth):
	// animations as soon as their frames are cached (the cache plays them without the asset), preview meshes after
	// 10 s without use, anything after 30 s off screen, everything under memory pressure.
	const bool bCacheOn = Settings->bUseFrameCache;
	const bool bSlotsWanted = GBongReleaseAfterCoarse != 0 && VisiblePending.Num() > 0 && BongOwned::Num(false) >= 12;
	BongOwned::ReleaseIf([&](const BongOwned::FInfo& Info)
	{
		if (bMemoryPressure)
		{
			return true;
		}
		if (Info.bMesh)
		{
			return Now - Info.LastUse > 10.0;
		}
		// Animations are touched every tick while on screen: off screen for 3 s = scrolled away. Free the slot (cap of
		// 12 plugin-loaded animations) for tiles on screen; partial frames stay in the memory cache.
		if (Now - Info.LastUse > 3.0)
		{
			return true;
		}
		if (bSlotsWanted && bCacheOn)
		{
			const FBongCacheEntry* Entry = Cache.Find(Info.AssetPath);
			const bool bDone = Entry && Entry->IsComplete() && (Entry->bNoDisk || Entry->bSaveStarted || !Settings->bEnableDiskCache);
			if (GBongReleaseAfterCoarse == 2 ? bDone : (Entry && Entry->NumFrames > 0 && Entry->NumValid >= FMath::Min(12, Entry->NumFrames)))
			{
				++GBongEarlyReleases;
				return true; // coarse frames shown; refined after a reload when a slot is free
			}
		}
		if (bCacheOn && Now - Info.TrackedAt > 3.0)
		{
			const FBongCacheEntry* Entry = Cache.Find(Info.AssetPath);
			return Entry && Entry->IsComplete() && (Entry->bNoDisk || Entry->bSaveStarted || !Settings->bEnableDiskCache);
		}
		return false;
	});

	// On-screen animations that are neither loaded nor playable from the cache (e.g. released, then evicted from
	// memory with the disk cache off): queue them for loading again.
	if (bAnims && bCacheOn && Settings->bAutoLoadVisibleFolder && !bMemoryPressure && Now - LastRequeueCheck > (Now < FastScanUntil ? 0.1 : 2.0))
	{
		LastRequeueCheck = Now;
		for (const FSoftObjectPath& Path : VisibleAnimPaths)
		{
			const FName PackageName(*Path.GetLongPackageName());
			if (PendingAnimPaths.Contains(PackageName) || Path.ResolveObject() || !Cache.IsDiskReady() || Cache.CanPlayWithoutLoading(Path, PackageName))
			{
				continue;
			}
			const FBongCacheEntry* Entry = Cache.Find(Path);
			if (Entry && Entry->IsComplete())
			{
				continue;
			}
			if (GBongReleaseAfterCoarse && Entry && Entry->NumValid > 0)
			{
				VisiblePending.Insert(PackageName, 0); // already moving: after the tiles that show nothing yet
			}
			else
			{
				VisiblePending.Add(PackageName);
			}
			PendingAnimPaths.Add(PackageName, Path);
		}
	}
}

void FBongThumbLiveModule::UpdateMemoryGuard(double Now)
{
	if (Now - LastMemCheck < 1.0)
	{
		return;
	}
	LastMemCheck = Now;
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	const double GB = 1024.0 * 1024.0 * 1024.0;
	LastFreeGB = (double)Stats.AvailablePhysical / GB;
	LastUsedGB = (double)Stats.UsedPhysical / GB;
	LastCapGB = Settings->MaxEditorMemoryGB > 0 ? (double)Settings->MaxEditorMemoryGB : (double)Stats.TotalPhysical / GB * 0.5;
	const double MinFree = FMath::Max(0.0, (double)Settings->MinFreeMemoryGB);
	const bool bLow = LastFreeGB < MinFree || LastUsedGB > LastCapGB;
	if (!bMemoryPressure && bLow)
	{
		bMemoryPressure = true;
		++PressureEvents;
		UE_LOG(LogBongThumbLive, Warning, TEXT("BongThumbLive: memory low (editor uses %.1f GB, limit %.1f GB; %.1f GB free, minimum %.1f GB) - pausing thumbnail loading/rendering and releasing what the plugin loaded."),
			LastUsedGB, LastCapGB, LastFreeGB, MinFree);
		ReleaseUnderPressure(TEXT("memory low"));
		LastPressureRelease = Now;
	}
	else if (bMemoryPressure)
	{
		if (LastFreeGB > MinFree + 2.0 && LastUsedGB < LastCapGB * 0.9)
		{
			bMemoryPressure = false;
			UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: memory OK again (editor %.1f GB, %.1f GB free) - resuming."), LastUsedGB, LastFreeGB);
		}
		else if (Now - LastPressureRelease > 20.0)
		{
			ReleaseUnderPressure(TEXT("memory still low"));
			LastPressureRelease = Now;
		}
	}
}

void FBongThumbLiveModule::ReleaseUnderPressure(const TCHAR* Why)
{
	UBongThumbLiveRenderer::ReleaseHeldAssets();
	BongOwned::ReleaseAll();
	if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
	{
		Cache->TrimToVisible();
	}
	UBongThumbLiveFXRenderer::PruneAllScenes(-1.0);
	BongOwned::RequestGC(true);
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: released held meshes, plugin-loaded assets, off-screen cache entries and effect previews (%s)."), Why);
}

void FBongThumbLiveModule::OnContentBrowserSelectionChanged(const TArray<FAssetData>& Selected, bool bIsPrimaryBrowser)
{
	SelectedAssets.Reset();
	for (const FAssetData& A : Selected)
	{
		if (SelectedAssets.Num() < 1024 && GetKind(A) != EBongThumbKind::None)
		{
			SelectedAssets.Add(A.GetSoftObjectPath());
		}
	}
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	if (!Settings->bEnabled || !Settings->bLoadAnimsOnSelection)
	{
		return;
	}
	int32 Budget = 64;
	for (const FAssetData& A : Selected)
	{
		if (Budget <= 0)
		{
			break;
		}
		if (!A.IsAssetLoaded() && IsKindEnabled(GetKind(A)))
		{
			// v2.2: asynchronous (A.GetAsset() loaded up to 64 packages synchronously on the game thread: measured
			// ~10 s at ~9 fps when 50 cold animations were selected).
			QueueAssetLoad(A.GetSoftObjectPath());
			--Budget;
		}
	}
}

void FBongThumbLiveModule::OnSettingsChanged()
{
	// Re-scan the current folder (e.g. auto-load or a type just switched on) and restart tracking.
	UBongThumbLiveRenderer::ClearCache(); // framing / resolution / FPS may have changed
	bFolderCheckRequested = true;
	AutoLoadFolder.Reset();
	Probed.Reset();
	LastProbeTime = 0.0;
	const UBongThumbLiveSettings* S = GetDefault<UBongThumbLiveSettings>();
	if (!S->bEnabled || !S->bAutoLoadVisibleFolder)
	{
		PendingPackages.Reset();
	}
	// Redraw every visible thumbnail so on/off changes show immediately.
	if (UThumbnailManager* Manager = UThumbnailManager::TryGet())
	{
		for (const FSoftObjectPath& Path : FolderAssets)
		{
			if (Path.ResolveObject())
			{
				Manager->GetOnThumbnailDirtied().Broadcast(Path);
			}
		}
	}
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive settings changed: Enabled=%d Mode=%s MaxFPS=%d PlaybackRate=%.2f MaxRefreshPerTickCap=%d AutoLoadVisibleFolder=%d Anim=%d Mat=%d Niagara=%d Cascade=%d"),
		S->bEnabled, S->IsContinuous() ? TEXT("VisibleInBrowser") : TEXT("OnHover"), S->MaxFPS, S->PlaybackRate, S->MaxRefreshPerTickCap, S->bAutoLoadVisibleFolder,
		S->bAnimateAnimations, S->bAnimateMaterials, S->bAnimateNiagara, S->bAnimateCascade);
}

void FBongThumbLiveModule::OnContentBrowserPathChanged(const FString& NewPath)
{
	bFolderCheckRequested = true;
	// Folder click: scan the new tiles and queue their loads right away instead of on the next 0.2 s / 2 s tick.
	const double Now = FPlatformTime::Seconds();
	FastScanUntil = Now + 3.0;
	LastVisibleScanTime = 0.0;
	LastRequeueCheck = 0.0;
	LastProbeTime = 0.0;
}

FString FBongThumbLiveModule::GetCurrentContentBrowserFolder() const
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		return FString();
	}
	const FContentBrowserItemPath Path = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetCurrentPath();
	return Path.HasInternalPath() ? Path.GetInternalPathString() : FString();
}

void FBongThumbLiveModule::UpdateFolderAutoLoad(double Now)
{
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	if (!Settings->bEnabled)
	{
		return;
	}
	if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor))
	{
		return;
	}
	// Event-driven (path changed) plus a cheap 0.5 s poll (covers startup, content drawer, history navigation).
	if (!bFolderCheckRequested && Now - LastFolderCheckTime < 0.5)
	{
		return;
	}
	LastFolderCheckTime = Now;

	const FString Folder = GetCurrentContentBrowserFolder();
	if (Folder.IsEmpty() || (!bFolderCheckRequested && Folder == AutoLoadFolder))
	{
		bFolderCheckRequested = false;
		return;
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (AR.IsLoadingAssets())
	{
		return; // retry once the registry has finished its initial scan
	}
	bFolderCheckRequested = false;
	if (Folder == AutoLoadFolder)
	{
		return;
	}
	AutoLoadFolder = Folder;

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Folder));
	Filter.bRecursivePaths = false; // only what the folder view shows directly
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// New folder replaces whatever was still queued for the previous one.
	FolderAssets.Reset();
	PendingPackages.Reset();
	AutoLoadQueued = 0;
	AutoLoadDone = 0;
	const int32 Max = FMath::Max(1, Settings->MaxAutoLoadPerFolder);
	int32 AlreadyLoaded = 0;
	for (const FAssetData& A : Assets)
	{
		if (!IsKindEnabled(GetKind(A)))
		{
			continue;
		}
		FolderAssets.Add(A.GetSoftObjectPath());
		if (A.IsAssetLoaded())
		{
			++AlreadyLoaded;
		}
		else if (Settings->bUseFrameCache && GetKind(A) == EBongThumbKind::Animation)
		{
			// Animations are loaded only when their tile is on screen and not already cached (visible auto-load).
		}
		else if (Settings->bAutoLoadVisibleFolder && PendingPackages.Num() < Max)
		{
			PendingPackages.AddUnique(A.PackageName);
		}
	}
	AutoLoadQueued = PendingPackages.Num();
	// Load in on-screen order (alphabetical, like the default asset view sort): pop from the back.
	PendingPackages.Sort([](const FName& A, const FName& B) { return B.LexicalLess(A); });
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive folder: %s -> %d supported assets directly in it (%d already loaded, %d queued for auto-load); subfolder/filter results are loaded from their visible thumbnails."), *Folder, FolderAssets.Num(), AlreadyLoaded, AutoLoadQueued);
	// Make the continuous-mode discovery pick the new ones up quickly.
	Probed.Reset();
	LastProbeTime = 0.0;
}

bool FBongThumbLiveModule::QueueAssetLoad(const FSoftObjectPath& Path, bool bRequireAsset)
{
	if (Path.IsNull() || Path.ResolveObject() || !GetDefault<UBongThumbLiveSettings>()->bEnabled) { return false; }
	const FName Package = Path.GetLongPackageFName();
	if (InFlightPackages.Contains(Package)) { return false; }
	if (ExplicitPending.Contains(Path))
	{
		if (bRequireAsset) { ForceAssetLoads.Add(Package); }
		return false;
	}
	if (ExplicitPending.Num() >= 1024) { return false; }
	ExplicitPending.Add(Path);
	if (bRequireAsset) { ForceAssetLoads.Add(Package); }
	return true;
}

void FBongThumbLiveModule::PumpAutoLoads()
{
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	if (!Settings->bEnabled)
	{
		ExplicitPending.Reset(); ForceAssetLoads.Reset();
	}
	if (!Settings->bEnabled || !Settings->bAutoLoadVisibleFolder)
	{
		VisiblePending.Reset(); PendingPackages.Reset(); PendingAnimPaths.Reset();
	}
	if (bMemoryPressure || (GEditor && (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor))) { return; }
	if (Settings->bRespectRealTimeThumbnailsToggle && !GetDefault<UContentBrowserSettings>()->RealTimeThumbnails) { return; }
	if (Settings->bUseFrameCache)
	{
		TSet<FSoftObjectPath> OnScreen(VisibleAnimPaths);
		for (int32 i = VisiblePending.Num() - 1; i >= 0; --i)
		{
			const FSoftObjectPath* P = PendingAnimPaths.Find(VisiblePending[i]);
			if (P && !OnScreen.Contains(*P))
			{
				VisibleSeen.Remove(VisiblePending[i]); PendingAnimPaths.Remove(VisiblePending[i]);
				VisiblePending.RemoveAt(i, EAllowShrinking::No); ++VisibleDropped;
			}
		}
	}
	const int32 Batch = FMath::Clamp(Settings->AutoLoadBatchSize, 1, 64);
	TWeakPtr<bool> WeakAlive = AliveToken;
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	while (InFlightLoads < Batch && (ExplicitPending.Num() || VisiblePending.Num() || PendingPackages.Num()))
	{
		const bool bExplicit = ExplicitPending.Num() > 0;
		const bool bVisible = !bExplicit && VisiblePending.Num() > 0;
		const FName Package = bExplicit ? ExplicitPending.Last().GetLongPackageFName() : (bVisible ? VisiblePending.Last() : PendingPackages.Last());
		FSoftObjectPath Path = bExplicit ? ExplicitPending.Last() : FSoftObjectPath(Package.ToString() + TEXT(".") + FPackageName::GetShortName(Package.ToString()));
		if (const FSoftObjectPath* Known = PendingAnimPaths.Find(Package)) { Path = *Known; }
		const bool bForce = ForceAssetLoads.Contains(Package);
		auto Pop = [&]()
		{
			if (bExplicit) { ExplicitPending.Pop(EAllowShrinking::No); }
			else { (bVisible ? VisiblePending : PendingPackages).Pop(EAllowShrinking::No); }
			PendingAnimPaths.Remove(Package); ForceAssetLoads.Remove(Package);
		};
		const UPackage* Existing = FindPackage(nullptr, *Package.ToString());
		if ((Existing && Existing->IsFullyLoaded()) || InFlightPackages.Contains(Package)) { Pop(); continue; }
		const FAssetData Data = AR.GetAssetByObjectPath(Path);
		if (!Data.IsValid() || !IsKindEnabled(GetKind(Data))) { Pop(); continue; }
		if (Settings->bUseFrameCache && GetKind(Data) == EBongThumbKind::Animation && !bForce)
		{
			FBongThumbCache& Cache = FBongThumbCache::Get();
			if (!Cache.IsDiskReady()) { break; }
			if (Cache.CanPlayWithoutLoading(Path, Package)) { Pop(); continue; }
		}
		// Every plugin-initiated asset load, including selection and tile recovery, owns a slot.
		if (GetKind(Data) == EBongThumbKind::Animation && BongOwned::Num(false) >= 12) { break; }
		Pop();
		BongOwned::Track(Package, Path, false, GetKind(Data) == EBongThumbKind::Animation);
		++InFlightLoads; InFlightPackages.Add(Package); NoteTileTrace(Package, 0);
		LoadPackageAsync(Package.ToString(), FLoadPackageAsyncDelegate::CreateLambda(
			[WeakAlive, bVisible](const FName& Name, UPackage*, EAsyncLoadingResult::Type Result)
			{
				if (!WeakAlive.IsValid()) { return; }
				if (FBongThumbLiveModule* M = FBongThumbLiveModule::GetPtr())
				{
					M->InFlightPackages.Remove(Name);
					M->InFlightLoads = FMath::Max(0, M->InFlightLoads - 1);
					FBongThumbLiveModule::NoteTileTrace(Name, 1);
					if (Result != EAsyncLoadingResult::Succeeded)
					{
						BongOwned::ReleaseIf([Name](const BongOwned::FInfo& Info) { return Info.Root == Name; });
						M->VisibleSeen.Remove(Name);
						UE_LOG(LogBongThumbLive, Verbose, TEXT("BongThumbLive auto-load: %s failed (%d)."), *Name.ToString(), (int32)Result);
					}
					if (bVisible) { ++M->VisibleDone; } else { ++M->AutoLoadDone; }
					M->LastProbeTime = 0.0;
				}
			}));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBongThumbLiveModule, BongThumbLive)

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongLoadQueueRegressionTest, "BongThumbLive.Loading.Guards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongLoadQueueRegressionTest::RunTest(const FString&)
{
	UBongThumbLiveSettings* S = GetMutableDefault<UBongThumbLiveSettings>();
	TGuardValue<bool> Enabled(S->bEnabled, true), Selection(S->bLoadAnimsOnSelection, true), Auto(S->bAutoLoadVisibleFolder, true);
	TGuardValue<bool> RespectToggle(S->bRespectRealTimeThumbnailsToggle, false);
	TGuardValue<int32> Batch(S->AutoLoadBatchSize, 4);
	FBongThumbLiveModule M;
	M.bMemoryPressure = true;
	FAssetData A;
	A.PackageName = TEXT("/Game/BTLRegressionUnloaded"); A.PackagePath = TEXT("/Game"); A.AssetName = TEXT("BTLRegressionUnloaded");
	A.AssetClassPath = UAnimSequence::StaticClass()->GetClassPathName();
	M.OnContentBrowserSelectionChanged({A}, true);
	TestEqual(TEXT("Selection enters the common queue instead of loading immediately"), M.ExplicitPending.Num(), 1);
	M.PumpAutoLoads();
	TestEqual(TEXT("Memory guard blocks selection load"), M.InFlightLoads, 0);
	TestEqual(TEXT("Blocked selection remains queued for recovery"), M.ExplicitPending.Num(), 1);
	TestFalse(TEXT("Tile recovery merges an existing request"), M.QueueAssetLoad(A.GetSoftObjectPath(), true));
	TestTrue(TEXT("Recovery marks the request as needing an actual pool asset"), M.ForceAssetLoads.Contains(A.PackageName));
	M.bMemoryPressure = false; M.InFlightLoads = 4;
	M.PumpAutoLoads();
	TestEqual(TEXT("Full batch does not start more loads"), M.InFlightLoads, 4);
	TestEqual(TEXT("Full batch preserves the pending request"), M.ExplicitPending.Num(), 1);
	S->bAutoLoadVisibleFolder = false; M.PendingPackages.Add(TEXT("/Game/OldFolder"));
	M.PumpAutoLoads();
	TestEqual(TEXT("Disabling auto-load clears its folder queue"), M.PendingPackages.Num(), 0);
	TestEqual(TEXT("Explicit selection still works independently of auto-load"), M.ExplicitPending.Num(), 1);
	S->bEnabled = false; M.PumpAutoLoads();
	TestEqual(TEXT("Disabling the plugin clears explicit requests too"), M.ExplicitPending.Num(), 0);
	TestEqual(TEXT("Forced recovery flags are cleared"), M.ForceAssetLoads.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongSpinnerClipRegressionTest, "BongThumbLive.UI.SpinnerClip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongSpinnerClipRegressionTest::RunTest(const FString&)
{
	FBongThumbLiveModule* M = FBongThumbLiveModule::GetPtr();
	if (!M || !FSlateApplication::IsInitialized()) { AddError(TEXT("Run this UI test in the editor with an animation folder open.")); return false; }
	M->UpdateVisibleTiles(FPlatformTime::Seconds() + 1.0);
	int32 Checked = 0, PartialTiles = 0;
	for (const auto& Pair : M->VisibleTileViewports)
	{
		for (const FBongTileViewport& Tile : Pair.Value)
		{
			const TSharedPtr<SWidget> VP = Tile.Viewport.Pin(), View = Tile.View.Pin();
			if (!VP || !View) { continue; }
			const FSlateRect Clip = BongSpinnerClip(VP, View);
			const FSlateRect Rect = VP->GetPaintSpaceGeometry().GetLayoutBoundingRect();
			for (TSharedPtr<SWidget> W = VP; W && W != View; W = W->GetParentWidget())
			{
				const FName Type = W->GetType();
				if (Type == TEXT("SAssetTileView") || Type == TEXT("SAssetListView") || Type == TEXT("SAssetColumnView"))
				{
					const FSlateRect Table = W->GetPaintSpaceGeometry().GetLayoutBoundingRect();
					if (FSlateRect::DoRectanglesIntersect(Rect, Table))
					{
						++Checked;
						TestTrue(TEXT("Overlay clip stays inside the tile table, excluding footer"), Clip.Bottom <= Table.Bottom && Clip.Top >= Table.Top && Clip.Left >= Table.Left && Clip.Right <= Table.Right);
						if (Rect.Bottom > Table.Bottom) { ++PartialTiles; }
					}
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Test inspected real thumbnail widgets"), Checked > 0);
	TestTrue(TEXT("Fixture includes a partially scrolled row"), PartialTiles > 0);
	AddInfo(FString::Printf(TEXT("Validated %d real tile clips, including %d partially visible tiles."), Checked, PartialTiles));
	return true;
}
#endif
