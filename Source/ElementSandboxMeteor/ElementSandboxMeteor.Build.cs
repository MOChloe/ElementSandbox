using UnrealBuildTool;

public class ElementSandboxMeteor : ModuleRules
{
	public ElementSandboxMeteor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ElementSandboxDestruction",
			"ElementSandboxWorldStorage"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ElementSandboxBuilding",
			"ElementSandboxNetBulkTransfer",
			"ElementSandboxWorldObjects"
		});
	}
}
