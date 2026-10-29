// NOTE: saved as UTF-8 with BOM.
#include "BongThumbLiveSettingsCustomization.h"
#include "BongThumbLiveModule.h"
#include "BongThumbLiveSettings.h"

#include "Animation/AnimSequence.h"
#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#include "AssetToolsModule.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IAssetTools.h"
#include "IAssetTypeActions.h"
#include "IDetailPropertyRow.h"
#include "Materials/Material.h"
#include "Modules/ModuleManager.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "PropertyHandle.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "DetailCategoryBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "BongThumbLiveCache.h"

TSharedRef<IDetailCustomization> FBongThumbLiveSettingsCustomization::MakeInstance()
{
	return MakeShared<FBongThumbLiveSettingsCustomization>();
}

FLinearColor FBongThumbLiveSettingsCustomization::GetAssetTypeColor(const UClass* AssetClass, const FLinearColor& Fallback)
{
	if (!AssetClass)
	{
		return Fallback;
	}
	const UAssetDefinition* Definition = nullptr;
	if (UAssetDefinitionRegistry* Registry = UAssetDefinitionRegistry::Get())
	{
		Definition = Registry->GetAssetDefinitionForClass(AssetClass);
	}
	// 1) A definition registered for exactly this class.
	if (Definition && Definition->GetAssetClass().Get() == AssetClass)
	{
		return Definition->GetAssetColor();
	}
	// 2) Legacy asset type actions (e.g. types that still register IAssetTypeActions).
	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")) || FModuleManager::Get().ModuleExists(TEXT("AssetTools")))
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		if (TSharedPtr<IAssetTypeActions> Actions = AssetTools.GetAssetTypeActionsForClass(AssetClass).Pin())
		{
			return FLinearColor(Actions->GetTypeColor());
		}
	}
	// 3) Closest parent definition.
	if (Definition)
	{
		return Definition->GetAssetColor();
	}
	return Fallback;
}

void FBongThumbLiveSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	struct FTypedOption
	{
		FName Property;
		const UClass* AssetClass;
	};
	const FTypedOption Options[] =
	{
		{ GET_MEMBER_NAME_CHECKED(UBongThumbLiveSettings, bAnimateAnimations), UAnimSequence::StaticClass() },
		{ GET_MEMBER_NAME_CHECKED(UBongThumbLiveSettings, bAnimateMaterials), UMaterial::StaticClass() },
		{ GET_MEMBER_NAME_CHECKED(UBongThumbLiveSettings, bAnimateNiagara), UNiagaraSystem::StaticClass() },
		{ GET_MEMBER_NAME_CHECKED(UBongThumbLiveSettings, bAnimateCascade), UParticleSystem::StaticClass() },
	};

	for (const FTypedOption& Option : Options)
	{
		TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(Option.Property, UBongThumbLiveSettings::StaticClass());
		if (!Handle->IsValidHandle())
		{
			continue;
		}
		const FLinearColor Color = GetAssetTypeColor(Option.AssetClass, FLinearColor::White);
		UE_LOG(LogBongThumbLive, Verbose, TEXT("BongThumbLive settings: %s label colour from %s = %s"), *Option.Property.ToString(), *Option.AssetClass->GetName(), *Color.ToString());

		IDetailPropertyRow* Row = DetailBuilder.EditDefaultProperty(Handle);
		if (!Row)
		{
			continue;
		}
		TSharedPtr<SWidget> DefaultName;
		TSharedPtr<SWidget> DefaultValue;
		Row->GetDefaultWidgets(DefaultName, DefaultValue);

		Row->CustomWidget()
			.NameContent()
			[
				SNew(STextBlock)
				.Text(Handle->GetPropertyDisplayName())
				.ToolTipText(Handle->GetToolTipText())
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor(Color))
			]
			.ValueContent()
			[
				DefaultValue.IsValid() ? DefaultValue.ToSharedRef() : Handle->CreatePropertyValueWidget()
			];
	}

	// Category order on the page: 기본, 재생, 캐시, 성능, 고급.
	const TCHAR* const Order[] = { TEXT("기본"), TEXT("재생"), TEXT("캐시"), TEXT("성능"), TEXT("고급") };
	for (int32 i = 0; i < UE_ARRAY_COUNT(Order); ++i)
	{
		DetailBuilder.EditCategory(FName(Order[i])).SetSortOrder(i);
	}

	// 캐시: where the disk cache lives, open it, clear it.
	IDetailCategoryBuilder& CacheCategory = DetailBuilder.EditCategory(FName(TEXT("캐시")));
	CacheCategory.AddCustomRow(FText::FromString(TEXT("캐시 폴더")))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("캐시 폴더")))
			.ToolTipText(FText::FromString(TEXT("디스크 캐시 파일이 저장되는 폴더이며, 지워도 썸네일이 다시 만들어지므로 안전합니다.")))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(400.f)
		.MaxDesiredWidth(900.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.Text_Lambda([]()
				{
					FBongThumbCache& Cache = FBongThumbCache::Get();
					return FText::FromString(FString::Printf(TEXT("%s  (%s)"), *Cache.GetCacheDir(), *Cache.GetDiskSummary()));
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("폴더 열기")))
				.ToolTipText(FText::FromString(TEXT("캐시 폴더를 탐색기에서 엽니다.")))
				.OnClicked_Lambda([]()
				{
					const FString Dir = FBongThumbCache::Get().GetCacheDir();
					IFileManager::Get().MakeDirectory(*Dir, true);
					FPlatformProcess::ExploreFolder(*Dir);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("캐시 비우기")))
				.ToolTipText(FText::FromString(TEXT("저장된 썸네일 프레임을 모두 지우며, 다음에 보일 때 다시 만들어집니다(그동안 잠깐 느려질 수 있습니다).")))
				.OnClicked_Lambda([]()
				{
					FBongThumbCache::Get().ClearDisk();
					return FReply::Handled();
				})
			]
		];
}
