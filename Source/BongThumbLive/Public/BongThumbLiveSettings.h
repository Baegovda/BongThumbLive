// BongThumbLive - live Content Browser thumbnails (animations, materials, Niagara, Cascade).
// NOTE: this file is saved as UTF-8 with BOM (Korean display names / tooltips).
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "BongThumbLiveSettings.generated.h"

UENUM(BlueprintType)
enum class EBongThumbLiveMode : uint8
{
	/** 마우스를 올린 썸네일만 움직입니다 (가장 가벼움). */
	OnHover UMETA(DisplayName = "마우스를 올린 것만"),
	/** 콘텐츠 브라우저에서 선택한 에셋(과 마우스를 올린 에셋)의 썸네일만 계속 움직입니다. */
	Selected UMETA(DisplayName = "선택한 것만"),
	/** 콘텐츠 브라우저에 보이는 모든 썸네일이 계속 움직입니다. */
	VisibleInBrowser UMETA(DisplayName = "브라우저에 보이는 것 전부"),
};

/**
 * Project Settings > Plugins > Bong Thumb Live
 * Stored per-user (Saved/Config/<Platform>Editor/EditorPerProjectUserSettings.ini) so it never dirties
 * shared project config. Every option is editable from the settings page and applies immediately.
 * Per-asset-type option labels are drawn in the Content Browser type colour (FBongThumbLiveSettingsCustomization).
 */
UCLASS(config = EditorPerProjectUserSettings, BlueprintType, meta = (DisplayName = "Bong Thumb Live"))
class BONGTHUMBLIVE_API UBongThumbLiveSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UBongThumbLiveSettings();

	/**
	 * UDeveloperSettings::GetContainerName() maps config=EditorPerProjectUserSettings to the "Editor"
	 * container (Editor Preferences). Force the "Project" container so the page lives under
	 * Project Settings > Plugins while the values stay in the per-user ini.
	 */
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("BongThumbLive"); }
#if WITH_EDITOR
	virtual FText GetSectionText() const override { return NSLOCTEXT("BongThumbLive", "Section", "Bong Thumb Live"); }
	virtual FText GetSectionDescription() const override { return NSLOCTEXT("BongThumbLive", "SectionDescKo", "콘텐츠 브라우저 썸네일을 실시간으로 움직이게 합니다: 애니메이션 재생, 머티리얼 시간 애니메이션, 나이아가라/캐스케이드 파티클 시뮬레이션. 설정은 사용자별로 저장됩니다."); }
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** One-time import of values saved by the plugin's previous name (AnimThumbLive). */
	void MigrateLegacyConfig();

	bool IsContinuous() const { return Mode == EBongThumbLiveMode::VisibleInBrowser; }

	// ───────────────────────────── 기본 ─────────────────────────────

	/** 플러그인 전체를 켜고 끕니다. 끄면 모든 썸네일이 엔진 기본(멈춘) 썸네일로 돌아가고 메모리/CPU를 전혀 쓰지 않습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "기본", meta = (DisplayName = "실시간 썸네일 사용"))
	bool bEnabled = true;

	/** 어떤 썸네일을 움직일지 고릅니다. '마우스를 올린 것만'이 가장 가볍고, '브라우저에 보이는 것 전부'는 보기 좋지만 처음 한 번 캐시를 만드는 동안 무거울 수 있습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "기본", meta = (DisplayName = "움직일 썸네일", EditCondition = "bEnabled"))
	EBongThumbLiveMode Mode = EBongThumbLiveMode::OnHover;

	/** 애니메이션(시퀀스/몽타주) 썸네일이 동작을 반복 재생합니다. 캐릭터 메시를 불러와야 해서 처음에는 메모리를 쓰지만, 캐시가 생기면 가벼워집니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "기본", meta = (DisplayName = "애니메이션 움직이기", EditCondition = "bEnabled"))
	bool bAnimateAnimations = true;

	/** 시간에 따라 변하는 머티리얼(흐르는 물, 깜빡임 등) 썸네일을 계속 다시 그립니다. 보이는 머티리얼이 많으면 조금 무거워집니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "기본", meta = (DisplayName = "머티리얼 움직이기", EditCondition = "bEnabled"))
	bool bAnimateMaterials = true;

	/** 나이아가라 이펙트 썸네일을 실제로 재생합니다. 이펙트마다 작은 미리보기 장면을 만들어서 개수가 많으면 메모리를 씁니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "기본", meta = (DisplayName = "나이아가라 이펙트 움직이기", EditCondition = "bEnabled"))
	bool bAnimateNiagara = true;

	/** 예전 방식 파티클(캐스케이드) 썸네일을 실제로 재생합니다. 나이아가라와 마찬가지로 개수가 많으면 메모리를 씁니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "기본", meta = (DisplayName = "캐스케이드 파티클 움직이기", EditCondition = "bEnabled"))
	bool bAnimateCascade = true;

	// ───────────────────────────── 재생 ─────────────────────────────

	/** 썸네일이 1초에 몇 번 바뀌는지 정합니다. 높이면 부드럽지만, 캐시가 없는 썸네일은 그만큼 자주 다시 그려서 무거워집니다(캐시된 애니메이션은 거의 영향 없음). */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "재생 FPS (초당 프레임)", EditCondition = "bEnabled", ClampMin = "1", ClampMax = "60", UIMin = "1", UIMax = "60"))
	int32 MaxFPS = 20;

	/** 마우스를 올린 썸네일만 따로 이 속도로 재생합니다. 자세히 보고 싶을 때 높이세요. 썸네일 하나라서 성능 영향은 작습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "마우스 올렸을 때 FPS", EditCondition = "bEnabled", ClampMin = "1", ClampMax = "120", UIMin = "1", UIMax = "120"))
	int32 HoverFPS = 60;

	/** 재생 빠르기입니다(1 = 원래 속도, 2 = 두 배). 성능과 메모리에는 영향이 없고 캐시도 다시 만들지 않습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "재생 속도 배율", EditCondition = "bEnabled", ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.1", UIMax = "4.0"))
	float PlaybackRate = 1.0f;

	/** 켜면 동작 전체가 화면에 들어오도록 카메라를 고정해 흔들리지 않습니다. 끄면 매 프레임 포즈에 맞춰 화면이 움직입니다. 바꾸면 캐시를 새로 만듭니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "카메라 고정 (흔들림 방지)", EditCondition = "bEnabled"))
	bool bStableFraming = true;

	/** 썸네일이 처음 움직이기 시작할 때까지(첫 그림이 준비될 때까지) 썸네일 가운데에 작은 로딩 표시(빙글빙글 도는 원)를 보여 줍니다. 캐시가 비어 있으면 처음 몇 초가 걸릴 수 있는데, 이 표시로 기다리는 중인지 알 수 있습니다. 첫 그림이 나오면 바로 사라지고, 메모리를 더 쓰지 않습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "로딩 표시 보이기", EditCondition = "bEnabled"))
	bool bShowLoadingSpinner = true;

	/** 이펙트를 처음 보여 줄 때 미리 이만큼(초) 재생해 둡니다. 썸네일이 처음에 텅 비어 보이면 늘리세요. 늘리면 처음 표시가 조금 늦어집니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "이펙트 미리 재생 시간 (초)", EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "5.0"))
	float FXWarmupSeconds = 1.0f;

	/** 이펙트가 끝나지 않아도 이 시간(초)이 지나면 처음부터 다시 재생합니다. 0이면 이펙트가 끝날 때만 다시 시작합니다. 성능 영향은 없습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "이펙트 반복 길이 (초, 0 = 끝날 때)", EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "20.0"))
	float FXMaxLoopSeconds = 0.0f;

	// ───────────────────────────── 캐시 ─────────────────────────────

	/** 애니메이션 썸네일을 한 번 그린 그림(프레임)을 저장해 두고 다시 씁니다. 켜 두면 많은 썸네일이 동시에 움직여도 가볍습니다. 끄면 매번 새로 그려서 매우 무겁습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "프레임 캐시 사용 (권장)", EditCondition = "bEnabled"))
	bool bUseFrameCache = true;

	/** 캐시에 1초당 몇 장을 저장할지 정합니다. 높을수록 부드럽지만 메모리와 디스크를 더 씁니다. 바꿔도 다른 값으로 만든 캐시는 지워지지 않습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "캐시 FPS (초당 저장 장수)", EditCondition = "bEnabled && bUseFrameCache", ClampMin = "1", ClampMax = "120", UIMin = "1", UIMax = "120"))
	int32 CacheFPS = 60;

	/** 저장하는 그림 한 장의 크기(픽셀)입니다. 128이면 충분히 선명합니다. 두 배로 올리면 메모리와 디스크 사용량이 네 배가 됩니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "캐시 그림 크기 (픽셀)", EditCondition = "bEnabled && bUseFrameCache", ClampMin = "32", ClampMax = "256", UIMin = "32", UIMax = "256"))
	int32 CacheResolution = 128;

	/** 애니메이션 하나에 저장할 최대 장수입니다. 긴 동작은 이 장수에 맞춰 촘촘함을 줄입니다. 낮추면 메모리를 아끼고, 높이면 긴 동작도 부드럽습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "애니메이션당 최대 장수", EditCondition = "bEnabled && bUseFrameCache", ClampMin = "8", ClampMax = "1024", UIMin = "30", UIMax = "480"))
	int32 MaxFramesPerAsset = 240;

	/** 플러그인이 쓸 수 있는 메모리(그래픽 메모리 포함) 총량(MB)입니다. 넘으면 가장 오래 안 본 썸네일부터 메모리에서 빼고(디스크에는 남음), 화면에 다 못 담으면 일부는 캐시 없이 그립니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "메모리 사용 한도 (MB)", EditCondition = "bEnabled && bUseFrameCache", ClampMin = "64", ClampMax = "8192", UIMin = "128", UIMax = "4096"))
	int32 CacheMemoryMB = 1024;

	/** 만든 캐시를 프로젝트의 Saved 폴더에 파일로 저장해서, 에디터를 다시 켜도 바로 움직이게 합니다. 끄면 매번 처음부터 다시 그립니다(무거움). */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "디스크 캐시 사용 (권장)", EditCondition = "bEnabled && bUseFrameCache"))
	bool bEnableDiskCache = true;

	/** 디스크 캐시가 쓸 수 있는 최대 용량(MB)입니다. 넘으면 가장 오래 안 본 애니메이션의 파일부터 자동으로 지웁니다. 애니메이션 하나는 보통 1-4 MB입니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "캐시", meta = (DisplayName = "디스크 캐시 최대 용량 (MB)", EditCondition = "bEnabled && bUseFrameCache && bEnableDiskCache", ClampMin = "64", ClampMax = "65536", UIMin = "256", UIMax = "16384"))
	int32 MaxDiskCacheMB = 2048;

	// ───────────────────────────── 성능 ─────────────────────────────

	/** 한 프레임에 썸네일을 새로 그리는 데 쓸 수 있는 시간(ms)입니다. 낮추면 에디터가 부드럽고 캐시가 천천히 만들어지며, 높이면 캐시가 빨리 만들어지지만 그동안 에디터가 버벅일 수 있습니다. 캐시된 썸네일 재생은 여기에 포함되지 않습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "성능", meta = (DisplayName = "프레임당 작업 시간 (ms)", EditCondition = "bEnabled", ClampMin = "0.5", ClampMax = "50.0", UIMin = "1.0", UIMax = "20.0"))
	float FrameBudgetMs = 4.0f;

	/** 움직이려면 에셋이 메모리에 올라와 있어야 해서, 화면에 보이는 에셋을 자동으로 불러옵니다. 디스크 캐시가 있는 애니메이션은 불러오지 않습니다. 메모리가 부족하면 끄세요. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "성능", meta = (DisplayName = "보이는 에셋 자동 불러오기", EditCondition = "bEnabled"))
	bool bAutoLoadVisibleFolder = true;

	/** 콘텐츠 브라우저에서 에셋을 선택하면 바로 불러와서 움직이게 합니다. 선택한 것만 불러오므로 메모리 영향은 작습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "성능", meta = (DisplayName = "선택하면 불러오기", EditCondition = "bEnabled"))
	bool bLoadAnimsOnSelection = true;

	/** 애니메이션을 그리려고 동시에 메모리에 붙잡아 둘 캐릭터 메시(3D 모델) 수입니다. 메시는 텍스처까지 함께 올라와 아주 큽니다. 낮추면 메모리를 크게 아끼고, 높이면 캐시를 조금 더 빨리 만듭니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "성능", meta = (DisplayName = "동시에 붙잡는 메시 수", EditCondition = "bEnabled", ClampMin = "1", ClampMax = "64", UIMin = "1", UIMax = "32"))
	int32 MaxHeldPreviewMeshes = 6;

	/** 컴퓨터의 남은 메모리(GB)가 이보다 적어지면 플러그인이 스스로 작업을 멈추고, 자기가 불러온 에셋과 캐시 메모리를 풀어 줍니다. 메모리가 적은 PC라면 낮추세요. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "성능", meta = (DisplayName = "남은 메모리 최소 (GB)", EditCondition = "bEnabled", ClampMin = "1", ClampMax = "256", UIMin = "2", UIMax = "64"))
	int32 MinFreeMemoryGB = 8;

	/** 에디터가 쓰는 메모리(GB)가 이 값을 넘으면 플러그인이 작업을 멈추고 자기가 불러온 에셋을 풀어 줍니다. 0이면 PC 메모리의 절반으로 자동 설정됩니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "성능", meta = (DisplayName = "에디터 메모리 상한 (GB, 0 = 자동)", EditCondition = "bEnabled", ClampMin = "0", ClampMax = "1024", UIMin = "0", UIMax = "256"))
	int32 MaxEditorMemoryGB = 0;

	// ───────────────────────────── 고급 ─────────────────────────────

	/** 동시에 메모리에 둘 이펙트 미리보기 장면 수입니다. 넘으면 가장 오래 안 보인 것부터 지웁니다. 낮추면 메모리를 아끼지만, 스크롤할 때 이펙트가 처음부터 다시 시작될 수 있습니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "고급", meta = (DisplayName = "이펙트 미리보기 장면 최대 수", EditCondition = "bEnabled", ClampMin = "1", ClampMax = "256", UIMin = "1", UIMax = "128"))
	int32 MaxLiveFXScenes = 48;

	/** 폴더를 열 때 자동으로 불러올 에셋의 최대 개수입니다. 아주 큰 폴더에서 메모리가 늘어나는 것이 걱정되면 낮추세요. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "고급", meta = (DisplayName = "폴더당 자동 불러오기 최대 수", EditCondition = "bEnabled && bAutoLoadVisibleFolder", ClampMin = "1", ClampMax = "4096", UIMin = "1", UIMax = "1024"))
	int32 MaxAutoLoadPerFolder = 512;

	/** 한꺼번에 불러오는 에셋 수입니다. 낮을수록 에디터가 부드럽고, 높을수록 썸네일이 빨리 움직이기 시작합니다. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "고급", meta = (DisplayName = "동시에 불러오는 개수", EditCondition = "bEnabled && bAutoLoadVisibleFolder", ClampMin = "1", ClampMax = "64", UIMin = "1", UIMax = "32"))
	int32 AutoLoadBatchSize = 4;

	/** 캐시를 쓰지 않는 썸네일(머티리얼, 이펙트)을 한 프레임에 최대 몇 개까지 다시 그릴지 정합니다. 0이면 위의 작업 시간에 맞춰 자동으로 정합니다. 에디터가 버벅일 때만 숫자를 넣으세요. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "고급", meta = (DisplayName = "프레임당 최대 갱신 수 (0 = 자동)", EditCondition = "bEnabled", ClampMin = "0", ClampMax = "1024", UIMin = "0", UIMax = "256"))
	int32 MaxRefreshPerTickCap = 0;

	/** 켜면 콘텐츠 브라우저 설정의 '실시간 썸네일(Real-Time Thumbnails)'이 꺼져 있을 때 움직임을 멈춥니다. 브라우저 설정 하나로 함께 끄고 싶을 때 쓰세요. */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "고급", meta = (DisplayName = "브라우저 '실시간 썸네일' 옵션 따르기", EditCondition = "bEnabled"))
	bool bRespectRealTimeThumbnailsToggle = true;

	/** Internal: settings schema version (used for one-time default upgrades). Not shown in the UI. */
	UPROPERTY(config)
	int32 SettingsVersion = 0;
};
