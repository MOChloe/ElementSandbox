using UnrealBuildTool;

public class ElementSandboxNetBulkTransfer : ModuleRules
{
	public ElementSandboxNetBulkTransfer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});
	}
}
