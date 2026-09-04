using UnrealBuildTool;

public class ElementSandboxBuilding : ModuleRules
{
	public ElementSandboxBuilding(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
					"ElementSandboxPresentation",
					"ElementSandboxWorldObjects",
				"ElementSandboxWorldStorage"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Chaos",
				"NetCore",
				"PhysicsCore"
			});
	}
}
