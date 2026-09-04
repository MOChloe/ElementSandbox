#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldObjectTypes.h"

/**
 * WorldObject 生命周期发布的稳定纯值行。接收者只能在回调期间使用 Local Handle；
 * RuntimeEvict/GameplayDestroy 回调中的 Handle 已死亡，WorldEntityId 仍可用于清理派生缓存。
 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectLifecycleRecord final
{
	FWorldObjectEntityHandle Entity;
	FWorldEntityId WorldEntityId;
	FName DefinitionId = NAME_None;
	FTransform WorldTransform = FTransform::Identity;
	EWorldObjectSpatialClass SpatialClass = EWorldObjectSpatialClass::Portable;
	uint32 StateRevision = 0;

	bool IsValid() const
	{
		return Entity.IsSet() && WorldEntityId.IsSet() && !DefinitionId.IsNone()
			&& !WorldTransform.ContainsNaN() && StateRevision != 0;
	}
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldObjectEntitiesUpsertedEvent,
	TConstArrayView<FWorldObjectLifecycleRecord>);
DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldObjectEntitiesRuntimeEvictedEvent,
	TConstArrayView<FWorldObjectLifecycleRecord>);
DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldObjectEntitiesGameplayDestroyedEvent,
	TConstArrayView<FWorldObjectLifecycleRecord>);
