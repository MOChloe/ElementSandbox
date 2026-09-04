using UnrealBuildTool;

public class ElementSandboxSimulation : ModuleRules
{
	// Element Entity、集中空间查询、Processor 与 Authority Barrier 保持宿主中立。
	public ElementSandboxSimulation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ElementSandboxWorldStorage"
		});
	}
}
