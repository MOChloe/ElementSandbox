using UnrealBuildTool;

public class ElementSandboxElementGameplay : ModuleRules
{
	public ElementSandboxElementGameplay(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ElementSandboxCombustion"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ElementSandboxAbilities",
			"ElementSandboxDestruction",
			"ElementSandboxBuilding",
			"ElementSandboxBuildingCatalog",
			"ElementSandboxCharacters",
			"ElementSandboxSimulation",
			"ElementSandboxWorldObjectCatalog",
			"ElementSandboxWorldObjects",
			"ElementSandboxWorldStorage",
			"GameplayAbilities",
			"GameplayTags"
		});
	}
}
