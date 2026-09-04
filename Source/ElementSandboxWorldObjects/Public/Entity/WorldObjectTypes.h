#pragma once

#include "CoreMinimal.h"

#include "WorldObjectTypes.generated.h"

/** 对象一生不变的空间类别。 */
UENUM(BlueprintType)
enum class EWorldObjectSpatialClass : uint8
{
	PermanentStatic,
	Portable
};

/** Portable 对象当前的运动驱动；少量 Physics/Attached 对象才拥有 Actor Proxy。 */
UENUM(BlueprintType)
enum class EWorldObjectMotionState : uint8
{
	Dormant,
	Attached,
	Physics
};
