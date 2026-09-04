#pragma once

#include "CoreMinimal.h"

#include "BuildCollisionPartDefinition.generated.h"

class UStaticMesh;

/** Collision Part 在 Chaos 中的长期运动策略。 */
UENUM()
enum class EBuildCollisionMobility : uint8
{
	/** 创建 WorldStatic Body；Transform 变化时通过重建更新。 */
	Static,

	/** 创建可由 ECS Transform 驱动的 Kinematic WorldDynamic Body。 */
	Kinematic
};

/** 一种建筑共享的简单碰撞代理配置；不代表独立 Entity。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionPartDefinition
{
	GENERATED_BODY()

	/** 必须具有 Simple Collision；Collision Projection 不执行运行时 Cook。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Collision")
	TObjectPtr<UStaticMesh> CollisionMesh = nullptr;

	/** 无 Driver 时相对 Entity；有 Driver 时相对被驱动 Mesh Part。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Collision")
	FTransform LocalTransform = FTransform::Identity;

	/** INDEX_NONE 表示直接使用 LocalTransform，否则跟随对应 Mesh Part Transform。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Collision")
	int32 DrivenMeshPartId = INDEX_NONE;

	UPROPERTY(EditDefaultsOnly, Category="Building|Collision")
	EBuildCollisionMobility Mobility = EBuildCollisionMobility::Static;

	/** NAME_None 使用 Mobility 默认 Profile：Static=BlockAll，Kinematic=BlockAllDynamic。 */
	UPROPERTY(EditDefaultsOnly, Category="Building|Collision")
	FName CollisionProfileName = NAME_None;

	FName GetEffectiveCollisionProfileName() const;
};
