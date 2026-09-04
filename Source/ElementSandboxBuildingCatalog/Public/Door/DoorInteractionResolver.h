#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

class FBuildEntityRegistry;

enum class EBuildDoorInteractionIntent : uint8
{
	None,
	Open,
	Close
};

/** 只从当前 Door Fragment 组合解析可提交的稳定交互意图。 */
ELEMENTSANDBOXBUILDINGCATALOG_API bool TryResolveBuildDoorInteraction(
	const FBuildEntityRegistry& Registry,
	FBuildEntityHandle Entity,
	EBuildDoorInteractionIntent& OutIntent);
