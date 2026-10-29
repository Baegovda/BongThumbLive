#include "BongThumbLiveRenderer.h"
#include "BongThumbLiveModule.h"
#include "BongThumbLiveSettings.h"
#include "BongThumbLiveCache.h"

#include "ThumbnailHelpers.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "UObject/ObjectKey.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"
#include "CanvasTypes.h"
#include "CanvasItem.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "ImageUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

/** True while shaders or any asset (anim / mesh / texture) are still compiling: frames rendered now may be framed or
 *  shaded from incomplete data, so they are shown but neither the frame nor the framing bounds are cached. */
static bool BongAnythingCompiling()
{
	return (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
		|| FAssetCompilingManager::Get().GetNumRemainingAssets() > 0;
}

/**
 * Preview-mesh resolver.
 * The stock path (USkeleton::GetAssetPreviewMesh + FindCompatibleMesh) loads the preview mesh synchronously and runs
 * an asset-registry query plus a synchronous load every call. It used to run for every loaded animation every second
 * (and again for every live tile at MaxFPS); the meshes were not held, so after each GC they were loaded again ->
 * periodic editor hitches. Now:
 *  - the result is cached per animation (weak pointer) and the mesh is held while it is in use,
 *  - the preview-mesh soft paths are read by reflection (no load) and the compatible-mesh query runs once per skeleton,
 *  - a mesh that is not loaded is loaded asynchronously (one request per package) - never on the game thread,
 *  - an animation without a usable mesh is retried after 2, 4, 8 s and then given up on (logged once) until the
 *    animation or a skeleton changes, or the settings are edited.
 */
struct FBongMeshSlot
{
	TWeakObjectPtr<USkeletalMesh> Mesh;
	double NextTry = 0.0;
	int32 Failures = 0;
	bool bGaveUp = false;
};

class FBongMeshHolder : public FGCObject
{
public:
	TArray<TObjectPtr<USkeletalMesh>> Meshes;
	TArray<double> LastUse;

	/** Hard cap (MaxHeldPreviewMeshes): holding more evicts the least recently used one (it may then be GC'd). */
	void Hold(USkeletalMesh* Mesh, double Now)
	{
		const int32 Index = Meshes.IndexOfByKey(Mesh);
		if (Index != INDEX_NONE)
		{
			LastUse[Index] = Now;
			return;
		}
		const int32 Cap = FMath::Clamp(GetDefault<UBongThumbLiveSettings>()->MaxHeldPreviewMeshes, 1, 64);
		while (Meshes.Num() >= Cap)
		{
			int32 Oldest = 0;
			for (int32 i = 1; i < Meshes.Num(); ++i)
			{
				if (LastUse[i] < LastUse[Oldest])
				{
					Oldest = i;
				}
			}
			Meshes.RemoveAtSwap(Oldest);
			LastUse.RemoveAtSwap(Oldest);
		}
		Meshes.Add(Mesh);
		LastUse.Add(Now);
	}
	void Prune(double Now, double MaxIdle)
	{
		for (int32 i = Meshes.Num() - 1; i >= 0; --i)
		{
			if (!Meshes[i] || Now - LastUse[i] > MaxIdle)
			{
				Meshes.RemoveAtSwap(i);
				LastUse.RemoveAtSwap(i);
			}
		}
	}
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override { Collector.AddReferencedObjects(Meshes); }
	virtual FString GetReferencerName() const override { return TEXT("FBongMeshHolder"); }
};

static TMap<FObjectKey, FBongMeshSlot> GBongMeshSlots;
static TMap<FObjectKey, FSoftObjectPath> GBongCompatibleMeshPath; // per skeleton; null path = none found
static TSet<FName> GBongMeshLoadsInFlight;
static TSet<FName> GBongMeshLoadsFailed;
static FBongMeshHolder* GBongMeshHolder = nullptr;
static int64 GBongResolveFast = 0;
static int64 GBongResolveSlow = 0;
static int64 GBongResolveAsyncLoads = 0;
static int64 GBongResolveGaveUp = 0;

static FSoftObjectPath BongReadPreviewMeshPath(const UObject* Obj)
{
	if (!Obj)
	{
		return FSoftObjectPath();
	}
	// UAnimationAsset::PreviewSkeletalMesh / USkeleton::PreviewSkeletalMesh (editor-only soft pointers).
	if (const FSoftObjectProperty* Prop = FindFProperty<FSoftObjectProperty>(Obj->GetClass(), TEXT("PreviewSkeletalMesh")))
	{
		if (const FSoftObjectPtr* Ptr = Prop->GetPropertyValuePtr_InContainer(Obj))
		{
			return Ptr->ToSoftObjectPath();
		}
	}
	return FSoftObjectPath();
}

static FSoftObjectPath BongFindCompatibleMeshPath(USkeleton* Skeleton)
{
	const FObjectKey Key(Skeleton);
	if (const FSoftObjectPath* Found = GBongCompatibleMeshPath.Find(Key))
	{
		return *Found;
	}
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	Filter.TagsAndValues.Add(USkeletalMesh::GetSkeletonMemberName(), FAssetData(Skeleton).GetExportTextName());
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);
	FSoftObjectPath Result;
	for (const FAssetData& Asset : Assets)
	{
		if (Result.IsNull() || Asset.IsAssetLoaded())
		{
			Result = Asset.GetSoftObjectPath();
			if (Asset.IsAssetLoaded())
			{
				break; // prefer a mesh that is already in memory
			}
		}
	}
	if (!Result.IsNull() || !AssetRegistry.IsLoadingAssets())
	{
		GBongCompatibleMeshPath.Add(Key, Result); // an empty result during the initial scan is not memoized
	}
	return Result;
}

static void BongStartMeshLoad(const FSoftObjectPath& Path)
{
	const FName PackageName = Path.GetLongPackageFName();
	if (PackageName.IsNone() || GBongMeshLoadsInFlight.Contains(PackageName) || GBongMeshLoadsFailed.Contains(PackageName))
	{
		return;
	}
	GBongMeshLoadsInFlight.Add(PackageName);
	++GBongResolveAsyncLoads;
	BongOwned::Track(PackageName, Path, /*bMesh*/ true); // loaded by us: released again when no longer used
	LoadPackageAsync(PackageName.ToString(), FLoadPackageAsyncDelegate::CreateLambda(
		[](const FName& LoadedName, UPackage* Package, EAsyncLoadingResult::Type Result)
		{
			GBongMeshLoadsInFlight.Remove(LoadedName);
			if (Result != EAsyncLoadingResult::Succeeded || !Package)
			{
				GBongMeshLoadsFailed.Add(LoadedName);
			}
		}));
}

/** Preview mesh the live scene uses for an animation, and whether it can be rendered right now.
 *  Shared by CanVisualizeAsset and Draw so the pool never asks for a draw we cannot do (which left black tiles).
 *  Never blocks: returns null while the mesh is being found / loaded. */
static USkeletalMesh* BongResolvePreviewMesh(UAnimSequenceBase* Anim, bool& bOutRenderable)
{
	bOutRenderable = false;
	if (!Anim)
	{
		return nullptr;
	}
	const double Now = FPlatformTime::Seconds();
	FBongMeshSlot& Slot = GBongMeshSlots.FindOrAdd(FObjectKey(Anim));
	USkeletalMesh* Mesh = Slot.Mesh.Get();
	if (Mesh)
	{
		++GBongResolveFast;
	}
	else
	{
		if (Slot.bGaveUp || Now < Slot.NextTry)
		{
			return nullptr;
		}
		++GBongResolveSlow;
		USkeleton* Skeleton = Anim->GetSkeleton();
		bool bWaitingForLoad = false;
		if (Skeleton)
		{
			const FSoftObjectPath Candidates[] = { BongReadPreviewMeshPath(Anim), BongReadPreviewMeshPath(Skeleton), FSoftObjectPath() };
			for (int32 i = 0; i < 3 && !Mesh; ++i)
			{
				const FSoftObjectPath Path = (i < 2) ? Candidates[i] : BongFindCompatibleMeshPath(Skeleton);
				if (Path.IsNull())
				{
					continue;
				}
				if (USkeletalMesh* Candidate = Cast<USkeletalMesh>(Path.ResolveObject()))
				{
					USkeleton* MeshSkeleton = Candidate->GetSkeleton();
					if (MeshSkeleton && (MeshSkeleton == Skeleton || Skeleton->IsCompatibleForEditor(MeshSkeleton)))
					{
						Mesh = Candidate;
					}
				}
				else if (!GBongMeshLoadsFailed.Contains(Path.GetLongPackageFName()))
				{
					if (GBongMeshLoadsInFlight.Num() >= 2 && !GBongMeshLoadsInFlight.Contains(Path.GetLongPackageFName()))
					{
						bWaitingForLoad = true; // at most two mesh loads at a time (memory)
						break;
					}
					BongStartMeshLoad(Path);
					bWaitingForLoad = true;
					break; // try again once it is in memory
				}
			}
		}
		if (!Mesh)
		{
			if (bWaitingForLoad)
			{
				Slot.NextTry = Now + 0.25;
			}
			else if (++Slot.Failures >= 4)
			{
				Slot.bGaveUp = true;
				++GBongResolveGaveUp;
				UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: no usable preview mesh for %s (skeleton %s) - live thumbnail disabled for it until the asset or settings change."),
					*Anim->GetPathName(), Skeleton ? *Skeleton->GetPathName() : TEXT("none"));
			}
			else
			{
				Slot.NextTry = Now + (double)(1 << Slot.Failures); // 2, 4, 8 s
			}
			return nullptr;
		}
		Slot.Mesh = Mesh;
		Slot.Failures = 0;
	}
	if (!GBongMeshHolder)
	{
		GBongMeshHolder = new FBongMeshHolder();
	}
	GBongMeshHolder->Hold(Mesh, Now); // keep it loaded while live thumbnails use it (no reload after every GC)
	BongOwned::Touch(Mesh->GetOutermost()->GetFName(), Now);
	bOutRenderable = FBongThumbLiveModule::IsObjectSafe(Mesh) && !Mesh->IsCompiling() && Mesh->GetResourceForRendering() != nullptr;
	return Mesh;
}

static void BongResetMeshSlots()
{
	GBongMeshSlots.Reset();
	GBongCompatibleMeshPath.Reset();
	GBongMeshLoadsFailed.Reset();
}

// Default 1: GPU skinning uploads bone matrices at most once per engine frame per mesh object, so with a pose-only
// update several frames rendered in the same tick (cache fill) all showed the same pose (measured: 9 unique of 72).
// Re-creating the render state gives every rendered frame its own pose; with the frame cache this cost is paid once.
static int32 GBongRecreateRenderState = 1;
static FAutoConsoleVariableRef CVarBongRecreateRenderState(
	TEXT("BongThumbLive.RecreateRenderState"),
	GBongRecreateRenderState,
	TEXT("1 = re-create the preview mesh render state for every rendered frame (default; correct pose for every cached frame). 0 = only update the pose (faster, but frames rendered in the same tick can repeat a pose)."));

#include UE_INLINE_GENERATED_CPP_BY_NAME(BongThumbLiveRenderer)

ABongThumbLiveMeshActor::ABongThumbLiveMeshActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UDebugSkelMeshComponent>(TEXT("SkeletalMeshComponent0")))
{
}

/** Thumbnail scene: same lighting/floor as the engine's anim thumbnail scene, but posable at any time. */
class FBongThumbLiveScene : public FThumbnailPreviewScene
{
public:
	FBongThumbLiveScene()
		: FThumbnailPreviewScene()
	{
		bForceAllUsedMipsResident = false;
		FActorSpawnParameters SpawnInfo;
		SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnInfo.bNoFail = true;
		SpawnInfo.ObjectFlags = RF_Transient;
		PreviewActor = GetWorld()->SpawnActor<ABongThumbLiveMeshActor>(SpawnInfo);
		PreviewActor->SetActorEnableCollision(false);
	}

	/** Drop the mesh / animation from the preview component (idle, or assets about to be deleted). */
	void Release()
	{
		USkeletalMeshComponent* Comp = PreviewActor->GetSkeletalMeshComponent();
		if (Comp->GetSkeletalMeshAsset() || CurrentAnim.IsValid())
		{
			Comp->SetAnimation(nullptr);
			Comp->SetSkeletalMesh(nullptr);
			Comp->ClearAnimScriptInstance();
		}
		CurrentAnim.Reset();
		PreviewAnimation = nullptr;
	}

	USkeletalMesh* GetMesh() const { return PreviewActor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset(); }

	bool HasContent() const { return CurrentAnim.IsValid() || PreviewActor->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() != nullptr; }

	bool SetAnimation(UAnimSequenceBase* InAnimation, float Time, bool bStableFraming)
	{
		USkeletalMeshComponent* Comp = PreviewActor->GetSkeletalMeshComponent();

		bool bOk = false;
		PreviewAnimation = InAnimation;

		if (InAnimation)
		{
			{
				bool bRenderable = false;
				USkeletalMesh* Mesh = BongResolvePreviewMesh(InAnimation, bRenderable);
				if (!bRenderable)
				{
					Mesh = nullptr; // mesh still loading / compiling: not renderable now (caller keeps the last frame)
				}
				// The scene is reused between draws: mesh and preview instance are only re-initialised when they change.
				const bool bMeshChanged = Comp->GetSkeletalMeshAsset() != Mesh;
				if (bMeshChanged)
				{
					Comp->OverrideMaterials.Empty();
					Comp->SetSkeletalMesh(Mesh);
					CurrentAnim.Reset();
				}

				if (Mesh)
				{
					bOk = true;
					PreviewActor->SetActorLocation(FVector::ZeroVector, false);

					const bool bPlayable = InAnimation->IsValidToPlay();
					if (bPlayable)
					{
						UAnimSingleNodeInstance* SingleNode = Comp->GetSingleNodeInstance();
						if (CurrentAnim.Get() != InAnimation || !SingleNode || SingleNode->GetAnimationAsset() != InAnimation)
						{
							UDebugSkelMeshComponent* DebugComp = CastChecked<UDebugSkelMeshComponent>(Comp);
							DebugComp->EnablePreview(true, InAnimation);
							Comp->Play(false);
							Comp->Stop();
							CurrentAnim = InAnimation;
						}
					}
					else
					{
						CurrentAnim.Reset();
					}

					// Framing bounds: stable union over the clip, or the current pose (stock behaviour).
					if (bPlayable && bStableFraming)
					{
						FramingBounds = GetStableBounds(InAnimation, Mesh);
					}

					if (bPlayable)
					{
						PoseAt(InAnimation, Time);
					}
					Comp->UpdateBounds();

					if (!(bPlayable && bStableFraming))
					{
						FramingBounds = Comp->Bounds;
					}

					const float BoundsZOffset = GetBoundsZOffset(FramingBounds);
					PreviewActor->SetActorLocation(-FramingBounds.Origin + FVector(0, 0, BoundsZOffset), false);
					Comp->UpdateBounds();
					if (GBongRecreateRenderState != 0)
					{
						Comp->RecreateRenderState_Concurrent(); // old, slow path (re-creates the scene proxy every frame)
					}
					else
					{
						// New pose / position only: send bone transforms + transform to the existing proxy.
						Comp->MarkRenderTransformDirty();
						Comp->MarkRenderDynamicDataDirty();
						Comp->DoDeferredRenderUpdates_Concurrent();
					}
				}
			}
		}

		if (!bOk)
		{
			Release();
		}
		return bOk;
	}

	void ClearCache() { BoundsCache.Reset(); }

protected:
	virtual void GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const override
	{
		check(PreviewAnimation);
		const float HalfFOVRadians = FMath::DegreesToRadians<float>(InFOVDegrees) * 0.5f;
		const float HalfMeshSize = static_cast<float>(FramingBounds.SphereRadius);
		const float BoundsZOffset = GetBoundsZOffset(FramingBounds);
		const float TargetDistance = HalfMeshSize / FMath::Tan(HalfFOVRadians);

		USceneThumbnailInfo* ThumbnailInfo = Cast<USceneThumbnailInfo>(PreviewAnimation->ThumbnailInfo);
		if (ThumbnailInfo)
		{
			if (TargetDistance + ThumbnailInfo->OrbitZoom < 0)
			{
				ThumbnailInfo->OrbitZoom = -TargetDistance;
			}
		}
		else
		{
			ThumbnailInfo = USceneThumbnailInfo::StaticClass()->GetDefaultObject<USceneThumbnailInfo>();
		}

		OutOrigin = FVector(0, 0, -BoundsZOffset);
		OutOrbitPitch = ThumbnailInfo->OrbitPitch;
		OutOrbitYaw = ThumbnailInfo->OrbitYaw;
		OutOrbitZoom = TargetDistance + ThumbnailInfo->OrbitZoom;
	}

private:
	void PoseAt(UAnimSequenceBase* Anim, float Time)
	{
		USkeletalMeshComponent* Comp = PreviewActor->GetSkeletalMeshComponent();
		Comp->SetPosition(Time, false);
		if (UAnimSingleNodeInstance* SingleNode = Comp->GetSingleNodeInstance())
		{
			SingleNode->UpdateMontageWeightForTimeSkip(Time);
		}
		Comp->RefreshBoneTransforms(nullptr);
	}

	/** Union of component bounds sampled across the clip, with the actor at the origin. Cached per anim+mesh. */
	FBoxSphereBounds GetStableBounds(UAnimSequenceBase* Anim, USkeletalMesh* Mesh)
	{
		const TPair<FObjectKey, FObjectKey> Key{ FObjectKey(Anim), FObjectKey(Mesh) };
		if (const FBoxSphereBounds* Found = BoundsCache.Find(Key))
		{
			return *Found;
		}

		USkeletalMeshComponent* Comp = PreviewActor->GetSkeletalMeshComponent();
		const float Length = Anim->GetPlayLength();
		constexpr int32 NumSamples = 9;
		FBox Box(ForceInit);
		for (int32 i = 0; i < NumSamples; ++i)
		{
			const float T = (Length > UE_KINDA_SMALL_NUMBER) ? Length * (float(i) / float(NumSamples - 1)) : 0.f;
			PoseAt(Anim, FMath::Min(T, FMath::Max(0.f, Length - UE_KINDA_SMALL_NUMBER)));
			Comp->UpdateBounds();
			Box += Comp->Bounds.GetBox();
		}
		const FBoxSphereBounds Result = Box.IsValid ? FBoxSphereBounds(Box) : Comp->Bounds;
		if (BongAnythingCompiling())
		{
			return Result; // possibly from incomplete data: recompute next time instead of memoizing
		}

		if (BoundsCache.Num() > 2048)
		{
			BoundsCache.Reset();
		}
		BoundsCache.Add(Key, Result);
		return Result;
	}

	ABongThumbLiveMeshActor* PreviewActor = nullptr;
	UAnimSequenceBase* PreviewAnimation = nullptr;
	/** Animation the preview instance is currently set up for (reuse across draws). */
	TWeakObjectPtr<UAnimSequenceBase> CurrentAnim;
	FBoxSphereBounds FramingBounds;
	TMap<TPair<FObjectKey, FObjectKey>, FBoxSphereBounds> BoundsCache;
};


/** Live renderer instances (for idle cleanup of their preview scenes). */
static TSet<UBongThumbLiveRenderer*> GBongRenderers;
/** Scene used by the cache fill (renders missing frames off-screen, independent of the thumbnail pool). */
static FBongThumbLiveScene* GBongFillScene = nullptr;
static double GBongFillSceneLastUse = 0.0;

FString UBongThumbLiveRenderer::GetCacheStats()
{
	FBongThumbCache* Cache = FBongThumbCache::GetPtr();
	return Cache ? Cache->GetStats() : FString(TEXT("cache: not started"));
}

FString UBongThumbLiveRenderer::GetResolveStats()
{
	int32 GaveUp = 0;
	for (const TPair<FObjectKey, FBongMeshSlot>& Pair : GBongMeshSlots)
	{
		GaveUp += Pair.Value.bGaveUp ? 1 : 0;
	}
	return FString::Printf(TEXT("meshResolve: slots=%d fast=%lld slow=%lld asyncLoads=%lld inFlight=%d gaveUp=%d (total %lld) heldMeshes=%d (cap %d) ownedMeshRoots=%d ownedAnimRoots=%d"),
		GBongMeshSlots.Num(), GBongResolveFast, GBongResolveSlow, GBongResolveAsyncLoads, GBongMeshLoadsInFlight.Num(), GaveUp, GBongResolveGaveUp,
		GBongMeshHolder ? GBongMeshHolder->Meshes.Num() : 0, GetDefault<UBongThumbLiveSettings>()->MaxHeldPreviewMeshes, BongOwned::Num(true), BongOwned::Num(false));
}

void UBongThumbLiveRenderer::ResetPreviewMeshes()
{
	BongResetMeshSlots();
}

void UBongThumbLiveRenderer::ReleaseHeldAssets()
{
	for (UBongThumbLiveRenderer* Renderer : GBongRenderers)
	{
		if (Renderer && Renderer->ThumbnailScene)
		{
			Renderer->ThumbnailScene->Release();
		}
	}
	if (GBongFillScene)
	{
		GBongFillScene->Release();
	}
	if (GBongMeshHolder)
	{
		GBongMeshHolder->Meshes.Reset();
		GBongMeshHolder->LastUse.Reset();
	}
	BongResetMeshSlots();
}

void UBongThumbLiveRenderer::PruneIdle(double Now)
{
	for (UBongThumbLiveRenderer* Renderer : GBongRenderers)
	{
		if (Renderer && Renderer->ThumbnailScene && Now - Renderer->LastSceneUse > 5.0 && Renderer->ThumbnailScene->HasContent())
		{
			Renderer->ThumbnailScene->Release();
		}
	}
	if (GBongFillScene && Now - GBongFillSceneLastUse > 5.0 && GBongFillScene->HasContent())
	{
		GBongFillScene->Release();
	}
	if (GBongMeshHolder)
	{
		GBongMeshHolder->Prune(Now, 10.0);
	}
	for (auto It = GBongMeshSlots.CreateIterator(); It; ++It)
	{
		// Slots of animations that were unloaded are dropped.
		if (!It.Key().ResolveObjectPtr())
		{
			It.RemoveCurrent();
		}
	}
}

void UBongThumbLiveRenderer::ClearCache()
{
	// Cached frames carry their settings in the key: entries made with other settings are dropped automatically
	// (and stay valid on disk for when the settings are switched back).
	BongResetMeshSlots(); // settings changed: give failed animations another chance
}

void UBongThumbLiveRenderer::ShutdownCache()
{
	FBongThumbCache::Shutdown();
	delete GBongFillScene;
	GBongFillScene = nullptr;
	BongResetMeshSlots();
	delete GBongMeshHolder;
	GBongMeshHolder = nullptr;
}

void UBongThumbLiveRenderer::DumpCache(const FString& Dir)
{
	FBongThumbCache::Get().Dump(Dir);
}

void UBongThumbLiveRenderer::InvalidateCached(const UObject* Object)
{
	if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
	{
		Cache->InvalidateObject(Object);
	}
	if (Object)
	{
		GBongMeshSlots.Remove(FObjectKey(Object)); // resolve its preview mesh again (an edit may have changed it)
	}
}

bool UBongThumbLiveRenderer::HasCachedFrames(const UObject* Object)
{
	FBongThumbCache* Cache = FBongThumbCache::GetPtr();
	const FBongCacheEntry* Entry = (Cache && Object) ? Cache->Find(FSoftObjectPath(Object)) : nullptr;
	return Entry && Entry->NumValid > 0;
}

int32 UBongThumbLiveRenderer::FillFrames(UAnimSequenceBase* Anim, FBongCacheEntry& Entry, double Deadline, int32 StartFrame)
{
	if (!FBongThumbLiveModule::IsObjectSafe(Anim) || Entry.NumFrames <= 0)
	{
		return 0;
	}
	FBongThumbCache& Cache = FBongThumbCache::Get();
	if (BongAnythingCompiling())
	{
		++Cache.NotStoredBusy; // frames rendered now may miss materials: wait
		return 0;
	}
	UTextureRenderTarget2D* Target = Cache.GetFillTarget();
	FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource)
	{
		return 0;
	}
	if (!GBongFillScene)
	{
		GBongFillScene = new FBongThumbLiveScene();
	}
	const double T0 = FPlatformTime::Seconds();
	const bool bStable = GetDefault<UBongThumbLiveSettings>()->bStableFraming;
	const int32 Size = FBongThumbCache::TileSize;
	int32 Done = 0;
	for (int32 n = 0; n < Entry.NumFrames; ++n)
	{
		const int32 Frame = (FMath::Max(StartFrame, 0) + n) % Entry.NumFrames;
		if (Entry.Valid[Frame])
		{
			continue;
		}
		if (Done > 0 && FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
		const float Time = FMath::Clamp((float)Frame / FMath::Max(Entry.Fps, 1.f), 0.f, FMath::Max(0.f, Entry.Length - UE_KINDA_SMALL_NUMBER));
		if (!GBongFillScene->SetAnimation(Anim, Time, bStable))
		{
			break; // preview mesh not ready yet (loading / compiling)
		}
		FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
		Canvas.Clear(FLinearColor::Black);
		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(Resource, GBongFillScene->GetScene(), FEngineShowFlags(ESFIM_Game))
			.SetTime(UThumbnailRenderer::GetTime()));
		ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
		ViewFamily.EngineShowFlags.MotionBlur = 0;
		ViewFamily.EngineShowFlags.LOD = 0;
		RenderViewFamily(&Canvas, &ViewFamily, GBongFillScene->CreateView(&ViewFamily, 0, 0, Size, Size));
		Canvas.Flush_GameThread();
		Cache.StoreFrame(Entry, Frame, Resource, FIntRect(0, 0, Size, Size), GBongFillScene->GetMesh());
		++Done;
	}
	GBongFillSceneLastUse = FPlatformTime::Seconds();
	FBongThumbLiveModule::AddRenderTime(GBongFillSceneLastUse - T0);
	return Done;
}

UBongThumbLiveRenderer::UBongThumbLiveRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Same as engine default sized renderers; config may override.
	DefaultSizeX = 512;
	DefaultSizeY = 512;
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		GBongRenderers.Add(this);
	}
}

bool UBongThumbLiveRenderer::CanVisualizeAsset(UObject* Object)
{
	// A complete cached animation plays from its frames: no preview mesh needed (and none is loaded for it).
	if (FBongThumbCache* Cache = FBongThumbCache::GetPtr())
	{
		if (const FBongCacheEntry* Entry = Object ? Cache->Find(FSoftObjectPath(Object)) : nullptr)
		{
			if (Entry->NumValid > 0 && (Entry->IsComplete() || !FBongThumbLiveModule::IsObjectSafe(Object)))
			{
				return true;
			}
		}
	}
	if (!FBongThumbLiveModule::IsObjectSafe(Object))
	{
		return false;
	}
	// Same conditions Draw needs (mesh resolved, loaded and compiled); otherwise the last cached frame or, for a tile
	// that was never live, the stock thumbnail.
	if (UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Object))
	{
		bool bRenderable = false;
		BongResolvePreviewMesh(Anim, bRenderable);
		if (!bRenderable)
		{
			return HasCachedFrames(Object);
		}
	}
	return true;
}

EThumbnailRenderFrequency UBongThumbLiveRenderer::GetThumbnailRenderFrequency(UObject* Object) const
{
	// Realtime == stock value for anim thumbnails; the pool re-renders on hover (or when dirtied in continuous mode).
	return EThumbnailRenderFrequency::Realtime;
}

void UBongThumbLiveRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Object);
	if (!Anim || !RenderTarget || Width == 0 || Height == 0)
	{
		return;
	}
	// Game-thread cost of this draw counts against the per-frame budget (see FBongThumbLiveModule::AddRenderTime).
	struct FBongDrawTimer
	{
		double Start = FPlatformTime::Seconds();
		~FBongDrawTimer() { FBongThumbLiveModule::AddRenderTime(FPlatformTime::Seconds() - Start); }
	} DrawTimer;
	const UBongThumbLiveSettings* Settings = GetDefault<UBongThumbLiveSettings>();
	const bool bLive = FBongThumbLiveModule::IsKindEnabled(EBongThumbKind::Animation);
	const double Now = FPlatformTime::Seconds();
	FBongThumbCache* Cache = (bLive && Settings->bUseFrameCache) ? &FBongThumbCache::Get() : nullptr;
	// Only Content Browser tile-sized draws are stored (the atlas cells are made from 256 px renders).
	const bool bTileSize = Width == (uint32)FBongThumbCache::TileSize && Height == (uint32)FBongThumbCache::TileSize;
	const bool bSafe = FBongThumbLiveModule::IsObjectSafe(Anim);
	FBongCacheEntry* Entry = Cache ? Cache->GetForAnim(Anim, bTileSize && bSafe, Now) : nullptr;
	const FIntRect DstRect(X, Y, X + (int32)Width, Y + (int32)Height);

	auto NotifyDrawn = [bLive, Anim]()
	{
		if (bLive)
		{
			if (FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr())
			{
				Module->NotifyDrawn(Anim);
			}
		}
	};
	auto DrawFallback = [&]()
	{
		// Live scene not renderable right now: keep the last frame this animation showed.
		if (Entry)
		{
			const int32 Frame = Entry->NearestValid(Entry->LastShown == INDEX_NONE ? 0 : Entry->LastShown);
			if (Frame != INDEX_NONE)
			{
				Cache->ShowFrame(*Entry, Frame, Canvas, RenderTarget, DstRect, Now);
			}
		}
	};

	if (!bSafe)
	{
		DrawFallback();
		return;
	}

	float Time = bLive ? FBongThumbLiveModule::GetLoopTime(Anim) : Anim->GetPlayLength() * 0.5f;
	const bool bStable = bLive && Settings->bStableFraming;
	int32 FrameIndex = INDEX_NONE;
	if (Entry)
	{
		FrameIndex = Entry->FrameIndexAt(Time);
		if (Entry->Valid[FrameIndex])
		{
			Cache->ShowFrame(*Entry, FrameIndex, Canvas, RenderTarget, DstRect, Now);
			NotifyDrawn();
			return;
		}
		// Frame budget used up (and not the hovered tile): show the nearest cached frame instead of rendering.
		const FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr();
		if (Entry->NumValid > 0 && FBongThumbLiveModule::IsOverFrameBudget() && !(Module && Module->IsHovered(Anim)))
		{
			Cache->ShowFrame(*Entry, Entry->NearestValid(FrameIndex), Canvas, RenderTarget, DstRect, Now);
			NotifyDrawn();
			return;
		}
		Time = FMath::Clamp((float)FrameIndex / FMath::Max(Entry->Fps, 1.f), 0.f, FMath::Max(0.f, Entry->Length - UE_KINDA_SMALL_NUMBER));
	}

	if (ThumbnailScene == nullptr)
	{
		ThumbnailScene = new FBongThumbLiveScene();
	}
	if (!ThumbnailScene->SetAnimation(Anim, Time, bStable))
	{
		DrawFallback();
		return;
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(RenderTarget, ThumbnailScene->GetScene(), FEngineShowFlags(ESFIM_Game))
		.SetTime(UThumbnailRenderer::GetTime())
		.SetAdditionalViewFamily(bAdditionalViewFamily));
	ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
	ViewFamily.EngineShowFlags.MotionBlur = 0;
	ViewFamily.EngineShowFlags.LOD = 0;
	RenderViewFamily(Canvas, &ViewFamily, ThumbnailScene->CreateView(&ViewFamily, X, Y, Width, Height));
	// The scene keeps its mesh / preview instance for the next draw (released by PruneIdle after a few idle seconds).
	LastSceneUse = Now;

	if (Cache && Entry)
	{
		++Cache->Renders;
		// A frame rendered while shaders are compiling may be missing materials: show it, but do not keep it.
		if (BongAnythingCompiling())
		{
			++Cache->NotStoredBusy;
		}
		else
		{
			Cache->StoreFrame(*Entry, FrameIndex, RenderTarget, DstRect, ThumbnailScene->GetMesh());
		}
	}
	NotifyDrawn();
}

void UBongThumbLiveRenderer::BeginDestroy()
{
	GBongRenderers.Remove(this);
	if (ThumbnailScene != nullptr)
	{
		delete ThumbnailScene;
		ThumbnailScene = nullptr;
	}
	Super::BeginDestroy();
}
