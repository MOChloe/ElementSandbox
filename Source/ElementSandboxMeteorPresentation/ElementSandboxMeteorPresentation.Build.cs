using UnrealBuildTool;

public class ElementSandboxMeteorPresentation : ModuleRules
{
	public ElementSandboxMeteorPresentation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
				"Core", "CoreUObject", "Engine", "ElementSandboxMeteor"
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
				"ElementSandboxWorldObjectCatalog"
		});
	}
}
