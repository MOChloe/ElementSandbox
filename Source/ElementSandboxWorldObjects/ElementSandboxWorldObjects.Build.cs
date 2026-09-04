using UnrealBuildTool;

public class ElementSandboxWorldObjects : ModuleRules
{
	public ElementSandboxWorldObjects(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
				"CoreUObject",
				"Engine",
				"ElementSandboxWorldStorage"
		});

		PrivateDependencyModuleNames.Add("PhysicsCore");
	}
}
