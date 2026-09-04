#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"

class APawn;
class UBuildingWorldSubsystem;
class UInventoryComponent;

enum class EBuildingDismantleFailure : uint8
{
	None,
	PlayerUnavailable,
	InventoryOpen,
	RateLimited,
	NoDemolitionTool,
	InvalidTarget,
	NotDismantleable,
	OutOfRange,
	InventoryFull,
	RewardConfigurationFailed,
	DestroyFailed
};

/** 服务器同步拆除事务；只允许当前拆除锤显式配置的 Building 返还到权威背包。 */
class FBuildingDismantleAuthorityService final
{
public:
	static EBuildingDismantleFailure TryBeginRequest(
		bool bHealthDepleted,
		bool bInventoryOpen,
		double RequestTime,
		double& InOutLastRequestTime);

	static EBuildingDismantleFailure TryDismantle(
		UBuildingWorldSubsystem& BuildingSubsystem,
		UInventoryComponent& Inventory,
		APawn& Pawn,
		FWorldEntityId WorldEntityId,
		double MaximumDistance);
};
