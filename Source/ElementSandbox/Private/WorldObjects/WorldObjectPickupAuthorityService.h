#pragma once

#include "Entity/WorldEntityId.h"
#include "WorldObjects/WorldObjectPickupFailure.h"

class APawn;
class UInventoryComponent;
class UWorldObjectWorldSubsystem;
class UWorldObjectItemCatalogSubsystem;

/** 服务器同步拾取事务；失败保留世界物件，成功只提交一次背包增加与 GameplayDestroy。 */
class FWorldObjectPickupAuthorityService final
{
public:
	static EWorldObjectPickupFailure TryPickup(
		UWorldObjectWorldSubsystem& WorldObjects,
		const UWorldObjectItemCatalogSubsystem& Catalog,
		UInventoryComponent& Inventory,
		APawn& Pawn,
		FWorldEntityId WorldEntityId,
		double MaximumDistance);
};
