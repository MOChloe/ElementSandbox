#pragma once

#include "CoreMinimal.h"
#include "Placement/BuildPlacementTypes.h"
#include "Templates/Function.h"

class APawn;
class UBuildingWorldSubsystem;
class UInventoryComponent;
class UWorld;

/** 服务器同步摆放事务；调用方负责死亡、背包状态和请求频率门禁。 */
class FBuildingPlacementAuthorityService final
{
public:
	/** Controller 的服务器门禁也做成纯规则，便于死亡/背包/频率路径稳定回归。 */
	static EBuildPlacementFailure TryBeginRequest(
		bool bHealthDepleted,
		bool bInventoryOpen,
		double RequestTime,
		double& InOutLastRequestTime);

	static EBuildPlacementFailure TryPlace(
		UWorld& World,
		UBuildingWorldSubsystem& BuildingSubsystem,
		UInventoryComponent& Inventory,
		APawn& BuilderPawn,
		int32 QuickbarIndex,
		const FVector& SurfaceLocation,
		const FVector& ExpectedResolvedLocation,
		uint8 YawQuarterTurns,
		TFunctionRef<bool(const FVector& ResolvedLocation)> CanMutateResolvedLocation);
};
