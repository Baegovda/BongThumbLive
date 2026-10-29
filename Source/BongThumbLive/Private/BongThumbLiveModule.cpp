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

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
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
		GBongAnimReady.Add(Key);
		return true;
	}
	// HasCompressedDataForPlatform builds the derived-data key (expensive): 0.5 s, 1 s, 2 s ... at most every 10 s per anim.
	const double Now = FPlatformTime::Seconds();
	int32 Tries = 0;
	if (const TPair<double, int32>* Last = GBongAnimNotReadySince.Find(Key))
	{
		Tries = Last->Value;
		if (Now - Last->Key < FMath::Min(0.5 * (double)(1 << FMath::Min(Tries, 5)), 10.0))
		{
			return false;
		}
	}
	static const ITargetPlatform* RunningPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
	if (RunningPlatform && Seq->HasCompressedDataForPlatform(RunningPlatform))
	{
		GBongAnimNotReadySince.Remove(Key);
		GBongAnimReady.Add(Key);
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
		else if (Obj && Obj->IsA<USkeleton>())
		{
			UBongThumbLiveRenderer::ResetPreviewMeshes(); // preview mesh of a skeleton may have changed
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
	if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
	{
		Cache->OnPoolRendered(AssetData.GetSoftObjectPath()); // the pool overwrote the tile: push our frame again
	}
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	if (!Settings->bEnabled || !Settings->bAutoLoadVisibleFolder || !AssetData.IsValid() || AssetData.IsAssetLoaded())
	{
		return;
	}
	if (VisibleSeen.Contains(AssetData.PackageName) || !IsKindEnabled(GetKind(AssetData)))
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

static void BongCollectTileTags(const TSharedRef<SWidget>& Widget, TSet<FString>& Out, int32 Depth)
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
			return; // a tile: nothing more below
		}
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 i = 0; Children && i < Children->Num(); ++i)
	{
		BongCollectTileTags(Children->GetChildAt(i), Out, Depth + 1);
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
	if (Now - LastVisibleScanTime < 0.2 || !FSlateApplication::IsInitialized())
	{
		return;
	}
	LastVisibleScanTime = Now;
	// Full widget-tree walk to find asset views: every 2 s (also while no browser is open - cheap, no rendering).
	if (Now - LastAssetViewScanTime > 2.0)
	{
		LastAssetViewScanTime = Now;
		AssetViews.Reset();
		for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
		{
			if (Window->IsVisible() && !Window->IsWindowMinimized())
			{
				BongCollectAssetViews(Window, AssetViews, 0);
			}
		}
	}
	TSet<FString> Tags;
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
		BongCollectTileTags(View.ToSharedRef(), Tags, 0);
	}
	VisibleTileCount = Tags.Num();
	VisibleTiles.Reset();
	VisibleTileKeys.Reset();
	VisibleAnimPaths.Reset();
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
					if (A.GetAsset())
					{
						++Loaded;
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
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Status: liveActive=%d visibleViews=%d visibleAnimTiles=%d FrameBudgetMs=%.1f avgDrawMs=%.2f renderMsThisFrame=%.2f | %s"),
				LiveState, AssetViews.Num(), VisibleAnimPaths.Num(), GetFrameBudgetMs(), GBongAvgDrawMs, GetRenderMsThisFrame(), *UBongThumbLiveRenderer::GetResolveStats());
			UE_LOG(LogBongThumbLive, Display, TEXT("BongThumbLive.Status: memory: editor %.1f GB (limit %.1f GB), free %.1f GB (minimum %d GB), pressure=%d (events %d), pendingAnimLoads=%d"),
				LastUsedGB, LastCapGB, LastFreeGB, S->MinFreeMemoryGB, bMemoryPressure ? 1 : 0, PressureEvents, PendingAnimPaths.Num());
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
		TEXT("Debug: save a PNG of the active (or main) editor window. Usage: BongThumbLive.CaptureEditorWindow C:/path/shot.png"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
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
			TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelRegularWindow();
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
	const double LeftMs = FMath::Max(0.0, GetFrameBudgetMs() - GetRenderMsThisFrame());
	Params.FillDeadline = FPlatformTime::Seconds() + LeftMs / 1000.0;
	Cache.Tick(Params);

	// Animations on screen are in use (also without the frame cache).
	if (bAnims)
	{
		for (const FSoftObjectPath& Path : VisibleAnimPaths)
		{
			BongOwned::Touch(FName(*Path.GetLongPackageName()), Now);
		}
	}
	// Release what the plugin itself loaded once it is no longer needed (the key fix for the unbounded growth):
	// animations as soon as their frames are cached (the cache plays them without the asset), preview meshes after
	// 10 s without use, anything after 30 s off screen, everything under memory pressure.
	const bool bCacheOn = Settings->bUseFrameCache;
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
		if (Now - Info.LastUse > 30.0)
		{
			return true;
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
	if (bAnims && bCacheOn && Settings->bAutoLoadVisibleFolder && !bMemoryPressure && Now - LastRequeueCheck > 2.0)
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
			VisiblePending.Add(PackageName);
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
			A.GetAsset();
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

void FBongThumbLiveModule::PumpAutoLoads()
{
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	if (!Settings->bEnabled || !Settings->bAutoLoadVisibleFolder)
	{
		VisiblePending.Reset();
		PendingAnimPaths.Reset();
		return;
	}
	if (PendingPackages.Num() == 0 && VisiblePending.Num() == 0)
	{
		return;
	}
	if (bMemoryPressure)
	{
		return; // paused until memory is available again
	}
	if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor))
	{
		return;
	}
	const int32 Batch = FMath::Clamp(Settings->AutoLoadBatchSize, 1, 64);
	TWeakPtr<bool> WeakAlive = AliveToken;
	while (InFlightLoads < Batch && (VisiblePending.Num() > 0 || PendingPackages.Num() > 0))
	{
		// Thumbnails actually on screen first, then the rest of the opened folder.
		const bool bVisible = VisiblePending.Num() > 0;
		const FName Next = bVisible ? VisiblePending.Last() : PendingPackages.Last();
		const FSoftObjectPath* AnimPath = PendingAnimPaths.Find(Next);
		const FSoftObjectPath AnimAssetPath = AnimPath ? *AnimPath : FSoftObjectPath();
		if (AnimPath && Settings->bUseFrameCache)
		{
			FBongThumbCache& Cache = FBongThumbCache::Get();
			if (!Cache.IsDiskReady())
			{
				break; // its frames may be on disk: wait for the startup scan
			}
			if (Cache.CanPlayWithoutLoading(AnimAssetPath, Next))
			{
				(bVisible ? VisiblePending : PendingPackages).Pop(EAllowShrinking::No);
				PendingAnimPaths.Remove(Next);
				if (bVisible) { ++VisibleDone; } else { ++AutoLoadDone; }
				continue; // plays from the cache: never loaded
			}
			if (BongOwned::Num(false) >= 12)
			{
				break; // at most 12 plugin-loaded animations at a time (released once their frames are cached)
			}
		}
		const FName PackageName = bVisible ? VisiblePending.Pop(EAllowShrinking::No) : PendingPackages.Pop(EAllowShrinking::No);
		PendingAnimPaths.Remove(PackageName);
		const UPackage* Existing = FindPackage(nullptr, *PackageName.ToString());
		if (Existing && Existing->IsFullyLoaded())
		{
			if (bVisible) { ++VisibleDone; } else { ++AutoLoadDone; }
			continue;
		}
		++InFlightLoads;
		if (!AnimAssetPath.IsNull())
		{
			BongOwned::Track(PackageName, AnimAssetPath, /*bMesh*/ false); // loaded by us: released when cached / unused
		}
		LoadPackageAsync(PackageName.ToString(), FLoadPackageAsyncDelegate::CreateLambda(
			[WeakAlive, bVisible](const FName& Name, UPackage* /*Package*/, EAsyncLoadingResult::Type Result)
			{
				if (WeakAlive.IsValid())
				{
					if (FBongThumbLiveModule* M = FBongThumbLiveModule::GetPtr())
					{
						M->InFlightLoads = FMath::Max(0, M->InFlightLoads - 1);
						if (Result != EAsyncLoadingResult::Succeeded)
						{
							UE_LOG(LogBongThumbLive, Verbose, TEXT("BongThumbLive auto-load: %s failed (%d)."), *Name.ToString(), (int32)Result);
						}
						if (bVisible)
						{
							++M->VisibleDone;
							if (M->VisibleLoaded.Num() >= 1024)
							{
								M->VisibleLoaded.RemoveAt(0, 256);
							}
							M->VisibleLoaded.Add(Name);
							M->LastProbeTime = 0.0; // make continuous-mode discovery pick it up right away
							if (M->VisiblePending.Num() == 0 && M->InFlightLoads == 0)
							{
								UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive visible auto-load: queue drained (%d / %d loaded)."), M->VisibleDone, M->VisibleQueued);
							}
						}
						else
						{
							++M->AutoLoadDone;
							if (M->PendingPackages.Num() == 0 && M->InFlightLoads == 0)
							{
								UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive auto-load: %s done (%d loaded)."), *M->AutoLoadFolder, M->AutoLoadDone);
							}
						}
					}
				}
			}));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBongThumbLiveModule, BongThumbLive)
