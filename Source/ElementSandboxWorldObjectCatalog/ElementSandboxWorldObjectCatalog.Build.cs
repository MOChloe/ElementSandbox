using UnrealBuildTool;

public class ElementSandboxWorldObjectCatalog : ModuleRules
{
	public ElementSandboxWorldObjectCatalog(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				"ElementSandboxCombustion",
				"ElementSandboxWorldObjects",
				"ElementSandboxWorldStorage"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ElementSandboxPresentation",
			"RenderCore",
			"MeshDescription",
			"MeshConversion",
			"StaticMeshDescription",
			"PhysicsCore"
		});
	}
}
