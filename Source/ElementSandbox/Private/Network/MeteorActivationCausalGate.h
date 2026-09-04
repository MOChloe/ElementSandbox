#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"

class UWorldStorageSubsystem;
struct FWorldNetworkEntityRemoval;

/**
 * 幂等提交 Meteor 源 GameplayDestroy。该提交只保证领域 ECS 已退出；HISM/MeshPool 投影
 * 可能仍在预算队列中，调用方必须确认表现投影退出后才能发布对应 Lane。
 */
bool ApplyMeteorSourceTombstone(
	UWorldStorageSubsystem& Storage,
	FWorldEntityId SourceWorldEntityId,
	uint32 SourceTombstoneRevision);

/**
 * 将同一帧已经通过 Meteor 因果门的源销毁合并成一次 WorldStorage 事务。
 * 调用方仍负责在领域投影真正退出后再发布 Lane。
 */
bool ApplyMeteorSourceTombstones(
	UWorldStorageSubsystem& Storage,
	TConstArrayView<FWorldNetworkEntityRemoval> Removals);
