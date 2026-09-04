using UnrealBuildTool;

public class ElementSandboxCombustion : ModuleRules
{
	public ElementSandboxCombustion(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject"
		});
	}
}
