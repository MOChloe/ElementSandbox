#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "Entity/WorldObjectTypes.h"
#include "Shape/WorldObjectShapeTypes.h"

class UWorldObjectDefinition;
class UWorldObjectProxyComponent;
class UWorldObjectWorldSubsystem;

/** Authority 创建一份世界投影所需的完整初始状态。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectCreateDesc final
{
	UWorldObjectDefinition* Definition = nullptr;
	/** 仅 Authority 可传入统一分配器已预留且未实体化的身份；空值由创建入口正常分配。 */
	FWorldEntityId ReservedWorldEntityId;
	FTransform WorldTransform = FTransform::Identity;
	EWorldObjectMotionState MotionState = EWorldObjectMotionState::Dormant;
	UWorldObjectProxyComponent* Proxy = nullptr;
	/** 运行时独有内容可覆盖 Definition 的交互/空间查询包络。 */
	TOptional<FBox> InstanceInteractionBounds;
	/** 运行时独有内容必须显式提供纯值几何，不能从表现资产或碰撞反推。 */
	TOptional<FWorldObjectShapeDefinition> InstanceShapeGeometry;
	uint64 InstanceShapeRevision = 1;
	/** 仅 Physics 状态使用；Subsystem 会为该配置创建一个 Box Physics Proxy。 */
	TOptional<FWorldObjectPhysicsBodyInit> PhysicsBody;
};

/**
 * 跨域破坏事务使用的未发布创建批次。只有 UWorldObjectWorldSubsystem 能写入；
 * 调用方必须在源 GameplayDestroy 成功后 Commit，否则 Rollback。
 */
class ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectStagedCreateBatch final
{
public:
	bool IsPrepared() const { return bPrepared && !Entities.IsEmpty(); }
	TConstArrayView<FWorldObjectEntityHandle> GetEntities() const { return Entities; }

private:
	TArray<FWorldObjectEntityHandle> Entities;
	TArray<FWorldEntityId> EntityIds;
	bool bPrepared = false;

	void Reset()
	{
		Entities.Reset();
		EntityIds.Reset();
		bPrepared = false;
	}

	friend UWorldObjectWorldSubsystem;
};
