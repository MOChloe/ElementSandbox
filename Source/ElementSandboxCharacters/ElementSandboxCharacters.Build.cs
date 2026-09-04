using UnrealBuildTool;

public class ElementSandboxCharacters : ModuleRules
{
	public ElementSandboxCharacters(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
				"Engine",
				"ElementSandboxWorldStorage",
				"GameplayAbilities"
		});
	}
}
