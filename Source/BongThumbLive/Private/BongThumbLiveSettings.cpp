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
	// Settings now live engine-wide (config=EditorSettings). Older builds stored them per project in
	// EditorPerProjectUserSettings.ini (as BongThumbLive, or before that as AnimThumbLive).
	const FString& Ini = GEditorPerProjectIni;
	const FString NewSection = GetClass()->GetPathName(); // /Script/BongThumbLive.BongThumbLiveSettings
	const TCHAR* OldSection = TEXT("/Script/AnimThumbLive.AnimThumbLiveSettings");
	// Capture these before SaveConfig creates the engine-wide section during the schema upgrade.
	const bool bHasEngineWide = GConfig->DoesSectionExist(*NewSection, GEditorSettingsIni);
	const bool bImportProject = !bHasEngineWide && GConfig->DoesSectionExist(*NewSection, Ini);
	const bool bImportLegacy = !bHasEngineWide && !bImportProject && GConfig->DoesSectionExist(OldSection, Ini);
	bool bUpgraded = false;
	if (bImportProject)
	{
		// Same keys, different file: read this project's values, then persist them engine-wide below.
		LoadConfig(GetClass(), *Ini);
		bUpgraded = true;
		UE_LOG(LogBongThumbLive, Log, TEXT("BongThumbLive: imported per-project settings into engine-wide EditorSettings.ini."));
	}
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
	// Persist right away to the engine-wide per-user ini (the settings page also saves; this covers Details edits elsewhere).
	SaveConfig();
	if (FBongThumbLiveModule* Module = FBongThumbLiveModule::GetPtr())
	{
		Module->OnSettingsChanged();
	}
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

// The details panel shows FProperty::GetToolTipText() on hover; check every editable setting has a Korean tooltip and name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongSettingsTooltipTest, "BongThumbLive.Settings.KoreanTooltips", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongSettingsTooltipTest::RunTest(const FString&)
{
	auto HasHangul = [](const FString& S)
	{
		for (const TCHAR C : S) { if (C >= 0xAC00 && C <= 0xD7A3) { return true; } }
		return false;
	};
	int32 Checked = 0;
	for (TFieldIterator<FProperty> It(UBongThumbLiveSettings::StaticClass()); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop->HasAllPropertyFlags(CPF_Config | CPF_Edit)) { continue; }
		++Checked;
		const FString Tip = Prop->GetToolTipText().ToString();
		const FString Name = Prop->GetDisplayNameText().ToString();
		TestTrue(FString::Printf(TEXT("%s has a Korean tooltip"), *Prop->GetName()), HasHangul(Tip) && Tip.Len() >= 10);
		TestTrue(FString::Printf(TEXT("%s has a Korean display name"), *Prop->GetName()), HasHangul(Name));
		TestNotEqual(FString::Printf(TEXT("%s tooltip differs from its name"), *Prop->GetName()), Tip, Name);
	}
	TestTrue(TEXT("Settings were inspected"), Checked >= 30);
	AddInfo(FString::Printf(TEXT("Checked %d editable settings."), Checked));
	return true;
}
#endif
