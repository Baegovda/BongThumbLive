#include "BongThumbLiveSettings.h"
#include "BongThumbLiveModule.h"
#include "Misc/ConfigCacheIni.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BongThumbLiveSettings)

UBongThumbLiveSettings::UBongThumbLiveSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("BongThumbLive");
}

void UBongThumbLiveSettings::MigrateLegacyConfig()
{
	if (!GConfig)
	{
		return;
	}
	const FString& Ini = GEditorPerProjectIni;
	const FString NewSection = GetClass()->GetPathName(); // /Script/BongThumbLive.BongThumbLiveSettings
	const TCHAR* OldSection = TEXT("/Script/AnimThumbLive.AnimThumbLiveSettings");
	// Capture this before SaveConfig creates the new section during the schema upgrade.
	const bool bImportLegacy = !GConfig->DoesSectionExist(*NewSection, Ini) && GConfig->DoesSectionExist(OldSection, Ini);
	bool bUpgraded = false;
	// v2: memory budget default raised 256 -> 1024 MB (GPU atlas cache), held-mesh cap added.
	if (SettingsVersion < 2)
	{
		if (CacheMemoryMB < 1024)
		{
			CacheMemoryMB = 1024;
		}
		SettingsVersion = 2;
		bUpgraded = true;
	}
	if (!bImportLegacy)
	{
		if (bUpgraded) { SaveConfig(); }
		return;
	}
	bool B = false;
	int32 I = 0;
	float F = 0.f;
	if (GConfig->GetBool(OldSection, TEXT("bEnabled"), B, Ini)) { bEnabled = B; }
	if (GConfig->GetBool(OldSection, TEXT("bHoverOnly"), B, Ini)) { Mode = B ? EBongThumbLiveMode::OnHover : EBongThumbLiveMode::VisibleInBrowser; }
	if (GConfig->GetInt(OldSection, TEXT("MaxFPS"), I, Ini)) { MaxFPS = FMath::Clamp(I, 1, 60); }
	if (GConfig->GetFloat(OldSection, TEXT("PlaybackRate"), F, Ini)) { PlaybackRate = FMath::Clamp(F, 0.05f, 10.f); }
	
	if (GConfig->GetBool(OldSection, TEXT("bRespectRealTimeThumbnailsToggle"), B, Ini)) { bRespectRealTimeThumbnailsToggle = B; }
	if (GConfig->GetBool(OldSection, TEXT("bStableFraming"), B, Ini)) { bStableFraming = B; }
	if (GConfig->GetBool(OldSection, TEXT("bLoadAnimsOnSelection"), B, Ini)) { bLoadAnimsOnSelection = B; }
	if (GConfig->GetBool(OldSection, TEXT("bAutoLoadVisibleFolder"), B, Ini)) { bAutoLoadVisibleFolder = B; }
	if (GConfig->GetInt(OldSection, TEXT("MaxAutoLoadPerFolder"), I, Ini)) { MaxAutoLoadPerFolder = FMath::Clamp(I, 1, 4096); }
	if (GConfig->GetInt(OldSection, TEXT("AutoLoadBatchSize"), I, Ini)) { AutoLoadBatchSize = FMath::Clamp(I, 1, 64); }
	SaveConfig();
	UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: migrated settings from [%s] (Mode=%s)."), OldSection, IsContinuous() ? TEXT("VisibleInBrowser") : TEXT("OnHover"));
}

#if WITH_EDITOR
void UBongThumbLiveSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent); // broadcasts OnSettingChanged
	// Persist right away to the per-user ini (the settings page also saves; this covers Details edits elsewhere).
	SaveConfig();
	if (FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr())
	{
		Module->OnSettingsChanged();
	}
}
#endif
