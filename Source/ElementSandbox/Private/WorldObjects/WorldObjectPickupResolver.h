#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectEntityHandle.h"

class APawn;
class UWorld;
class UWorldObjectWorldSubsystem;
class UItemDefinition;
class UWorldObjectItemCatalogSubsystem;

namespace UE::ElementSandbox
{
	struct FWorldObjectPickupResolution final
	{
		UItemDefinition* ItemDefinition = nullptr;
		int32 Quantity = 0;
		FBox InteractionLocalBounds = FBox(ForceInit);
		FTransform WorldTransform = FTransform::Identity;
		TWeakObjectPtr<AActor> ProjectionActor;

		bool IsValid() const;
		/** AABB 只负责粗筛；用本次 ECS 位姿反变换射线，返回实际交互盒表面的世界距离。 */
		bool RaycastInteractionBounds(const FVector& Origin, const FVector& Direction,
			double MaxDistance, double& OutDistance) const;
		/** 客户端提示与 Authority 复验共用旋转盒最近点，不把空白 AABB 角落算成可触及表面。 */
		double ComputeSquaredInteractionDistance(const FVector& WorldPoint) const;
		FVector ClosestInteractionPoint(const FVector& WorldPoint) const;
	};

	/** 仅解析可拾取的 Portable；Physics 使用已绑定代理的当前位姿，Dormant 使用 ECS 稳定位姿。 */
	bool TryResolveWorldObjectPickup(
		const UWorldObjectWorldSubsystem& WorldObjects,
		FWorldObjectEntityHandle Entity,
		const UWorldObjectItemCatalogSubsystem& Catalog,
		FWorldObjectPickupResolution& OutResolution);

	/** 只读触及/可见性校验；Building 查 ECS Simple Collision，普通 PhysicsBody 残骸不互相遮挡。 */
	bool HasClearWorldObjectPickupPath(
		const UWorld& World,
		const APawn& Pawn,
		const FWorldObjectPickupResolution& Pickup,
		const FVector& Origin);
}
