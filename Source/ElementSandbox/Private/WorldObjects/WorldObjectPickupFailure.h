#pragma once

#include "CoreMinimal.h"
#include "WorldObjectPickupFailure.generated.h"

/** Owner 的拾取事务结果；只用于确认与提示，不作为客户端修改 ECS 的命令。 */
UENUM()
enum class EWorldObjectPickupFailure : uint8
{
	None,
	PlayerUnavailable,
	InvalidTarget,
	OutOfRange,
	Obstructed,
	InventoryFull,
	DestroyRejected
};
