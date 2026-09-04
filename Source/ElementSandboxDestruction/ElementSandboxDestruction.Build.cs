using UnrealBuildTool;

public class ElementSandboxDestruction : ModuleRules
{
	public ElementSandboxDestruction(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ElementSandboxBuilding",
			"ElementSandboxWorldObjects",
			"ElementSandboxWorldStorage"
		});
	}
}
