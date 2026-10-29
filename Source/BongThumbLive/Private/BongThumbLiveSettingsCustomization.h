// BongThumbLive - Project Settings page customization: per-asset-type option labels are drawn in the
// Content Browser colour of that asset type (read at runtime from the asset definitions / type actions).
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FBongThumbLiveSettingsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

	/**
	 * Content Browser type colour of AssetClass: UAssetDefinition::GetAssetColor() of the definition registered for
	 * exactly that class, else IAssetTypeActions::GetTypeColor(), else the closest parent definition, else Fallback.
	 */
	static FLinearColor GetAssetTypeColor(const UClass* AssetClass, const FLinearColor& Fallback);
};
