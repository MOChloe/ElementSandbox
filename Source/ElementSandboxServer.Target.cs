using UnrealBuildTool;
using System.Collections.Generic;

public class ElementSandboxServerTarget : TargetRules
{
	public ElementSandboxServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("ElementSandbox");
	}
}
