using UnrealBuildTool;

public class ElementSandboxElementPresentation : ModuleRules
{
	public ElementSandboxElementPresentation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ElementSandboxSimulation",
			"ElementSandboxPresentation"
		});
	}
}
