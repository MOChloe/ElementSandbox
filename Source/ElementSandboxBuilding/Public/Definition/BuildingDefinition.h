#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Definition/WorldDestructionDefinition.h"
#include "Engine/DataAsset.h"
#include "Entity/BuildEntityHandle.h"

#include "BuildingDefinition.generated.h"

class FBuildEntityRegistry;

/**
 * 一种建筑的共享 OOP 配置与创建入口，不代表任何已放置实例。
 * 每次 CreateEntity 都只向 Building ECS 写入纯数据；运行时状态留在 Fragment Pool。
 */
UCLASS(Abstract, BlueprintType, NotBlueprintable)
class ELEMENTSANDBOXBUILDING_API UBuildingDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 跨服务器和客户端解析同一共享 Definition 的稳定标识。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Building")
	FName DefinitionId = NAME_None;

	bool HasValidDefinitionId() const { return !DefinitionId.IsNone(); }

	/** 同类建筑共享的预配置 Mesh Part；数组序号是运行时稳定 PartId。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Rendering")
	TArray<FBuildMeshPartDefinition> MeshParts;

	/** 同类建筑共享的 Simple Collision 代理；数组序号是稳定 CollisionPartId。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Collision")
	TArray<FBuildCollisionPartDefinition> CollisionParts;

	/** 未配置时不可被斧头破坏；累计伤害只存在稀疏 RuntimeOnly Fragment。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Building|Destruction")
	FWorldDestructionDefinition Destruction;

	/**
	 * 创建 Entity 后先写入权威 World Transform 与通用 Definition Fragment，再调用派生
	 * 配置钩子。任一步失败都会立即销毁 Entity，调用方不会看到半初始化实例。
	 */
	FBuildEntityHandle CreateEntity(
		FBuildEntityRegistry& Registry,
		const FTransform& InitialWorldTransform) const;

	/** 合并全部有效 Mesh Part 的世界 AABB，供空间索引注册使用。 */
	bool TryCalculateWorldBounds(
		const FTransform& WorldTransform,
		FBox& OutWorldBounds) const;

	/** 只合并当前真实 Mesh Part，不包含 Collision 或派生类的保守 Swept Bounds。 */
	bool TryCalculateCurrentVisualWorldBounds(
		const FTransform& WorldTransform,
		TConstArrayView<FTransform> PartLocalTransforms,
		FBox& OutWorldBounds) const;

	/** Collision 配置只接受已有 Simple Collision 的代理，不在运行时 Cook。 */
	bool HasValidCollisionDefinition() const;

	/** 解析一个 Collision Part 的最终世界 Transform。 */
	bool TryCalculateCollisionPartWorldTransform(
		int32 CollisionPartId,
		const FTransform& WorldTransform,
		TConstArrayView<FTransform> PartLocalTransforms,
		FTransform& OutPartWorldTransform) const;

	/** 使用代理的 Simple Collision 计算一个 Collision Part 的精确世界 AABB。 */
	bool TryCalculateCollisionPartWorldBounds(
		int32 CollisionPartId,
		const FTransform& WorldTransform,
		TConstArrayView<FTransform> PartLocalTransforms,
		FBox& OutPartWorldBounds) const;

	/**
	 * 使用每实例 Part Transform 计算合并 Bounds。非空数组必须与 MeshParts 等长；
	 * 空 View 等同于读取共享 MeshParts 配置。
	 */
	virtual bool TryCalculateWorldBounds(
		const FTransform& WorldTransform,
		TConstArrayView<FTransform> PartLocalTransforms,
		FBox& OutWorldBounds) const;

	/**
	 * 指定 Part 的局部 Transform 变化是否需要重算空间边界。
	 * 默认按真实当前姿态计算 Bounds，因此任何 Part 变化都返回 true；使用保守 Swept
	 * Bounds 的 Definition 可仅对已覆盖的 Part 返回 false。
	 */
	virtual bool DoPartTransformChangesAffectSpatialBounds(
		TConstArrayView<int32> PartIds) const;

protected:
	/**
	 * 派生建筑在这里写入自己的初始 Fragment；
	 * 不能保存 Registry 或 Handle 的裸引用。
	 */
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const PURE_VIRTUAL(
		UBuildingDefinition::ConfigureEntity,
		return false;);
};
