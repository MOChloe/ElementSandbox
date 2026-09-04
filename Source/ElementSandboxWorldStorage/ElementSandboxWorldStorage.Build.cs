using UnrealBuildTool;

public class ElementSandboxWorldStorage : ModuleRules
{
	public ElementSandboxWorldStorage(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ElementSandboxNetBulkTransfer",
			"NetCore"
		});
	}
}
