using UnrealBuildTool;

public class ElementSandboxBuildingCatalog : ModuleRules
{
	public ElementSandboxBuildingCatalog(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
				"Core",
				"CoreUObject",
				"Engine",
				"ElementSandboxBuilding",
				"ElementSandboxItems",
					"ElementSandboxCombustion",
					"ElementSandboxWorldObjects",
					"ElementSandboxWorldObjectCatalog"
				});

			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"ElementSandboxPresentation",
				"ElementSandboxWorldStorage"
			});
	}
}
