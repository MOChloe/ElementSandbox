using UnrealBuildTool;

public class ElementSandboxUI : ModuleRules
{
	public ElementSandboxUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"ElementSandboxItems"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"Slate",
			"SlateCore"
		});
	}
}
