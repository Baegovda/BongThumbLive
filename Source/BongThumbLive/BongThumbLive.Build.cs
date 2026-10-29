using UnrealBuildTool;

public class BongThumbLive : ModuleRules
{
	public BongThumbLive(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });
		PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "RenderCore", "Renderer", "ContentBrowser", "ContentBrowserData", "AssetRegistry", "Slate", "SlateCore", "Settings", "Niagara", "NiagaraCore", "PropertyEditor", "AssetDefinition", "AssetTools", "ApplicationCore", "InputCore", "RHI", "TargetPlatform", "ToolMenus" });
	}
}
