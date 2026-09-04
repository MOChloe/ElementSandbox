// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ElementSandbox : ModuleRules
{
	public ElementSandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
			{
				"ElementSandboxAbilities",
				"ElementSandboxCharacters",
					"ElementSandboxDestruction",
					"ElementSandboxMeteor",
					"ElementSandboxNetBulkTransfer",
				"ElementSandboxBuilding",
				"ElementSandboxBuildingCatalog",
				"ElementSandboxPresentation",
					"ElementSandboxItems",
					"ElementSandboxSimulation",
					"ElementSandboxElementGameplay",
					"ElementSandboxElementPresentation",
				"ElementSandboxUI",
				"ElementSandboxWorldObjects",
				"ElementSandboxWorldObjectCatalog",
				"ElementSandboxWorldStorage",
			"InputCore",
			"EnhancedInput",
			"GameplayAbilities",
			"GameplayTags",
				"GameplayTasks",
				"AnimGraphRuntime",
				"UMG",
					"Slate",
					"SlateCore",
						"RHI",
						"RenderCore"
		});
	}
}
