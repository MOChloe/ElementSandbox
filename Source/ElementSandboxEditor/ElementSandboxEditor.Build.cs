using UnrealBuildTool;

public class ElementSandboxEditor : ModuleRules
{
	public ElementSandboxEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"ElementSandboxMeteor",
				"UnrealEd",
				"AssetRegistry",
			"ElementSandboxBuilding",
			"ElementSandboxBuildingCatalog",
				"ElementSandboxElementGameplay",
				"ElementSandboxWorldObjectCatalog",
				"ElementSandboxWorldObjects",
				"ElementSandboxWorldStorage"
		});
	}
}
