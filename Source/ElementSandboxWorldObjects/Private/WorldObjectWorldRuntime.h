#pragma once

#include "Entity/WorldObjectEntityRegistry.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "Storage/WorldObjectPersistenceExtension.h"
#include "WorldStorageSubsystem.h"

class UWorldObjectDefinition;
class UWorldObjectProxyComponent;

/** 将百万级 WorldEntityId 索引分成固定小表，避免单个 TMap 扩容时在 Game Thread 重哈希全部驻留实体。 */
template <typename ValueType>
class TWorldObjectWorldEntityShardedMap final
{
public:
	ValueType* Find(const FWorldEntityId EntityId)
	{
		return Shards[GetShardIndex(EntityId)].Find(EntityId);
	}

	const ValueType* Find(const FWorldEntityId EntityId) const
	{
		return Shards[GetShardIndex(EntityId)].Find(EntityId);
	}

	bool Contains(const FWorldEntityId EntityId) const { return Find(EntityId) != nullptr; }

	void Add(const FWorldEntityId EntityId, const ValueType& Value)
	{
		TMap<FWorldEntityId, ValueType>& Shard = Shards[GetShardIndex(EntityId)];
		EntryCount += Shard.Contains(EntityId) ? 0 : 1;
		Shard.Add(EntityId, Value);
	}

	int32 Remove(const FWorldEntityId EntityId)
	{
		const int32 Removed = Shards[GetShardIndex(EntityId)].Remove(EntityId);
		EntryCount -= Removed;
		return Removed;
	}

	void Reserve(const int32 TotalCapacity)
	{
		const int32 PerShardCapacity = FMath::DivideAndRoundUp(FMath::Max(0, TotalCapacity), ShardCount);
		for (TMap<FWorldEntityId, ValueType>& Shard : Shards)
		{
			Shard.Reserve(PerShardCapacity);
		}
	}

	int32 Num() const { return EntryCount; }

	SIZE_T GetAllocatedSize() const
	{
		SIZE_T Bytes = 0;
		for (const TMap<FWorldEntityId, ValueType>& Shard : Shards)
		{
			Bytes += Shard.GetAllocatedSize();
		}
		return Bytes;
	}

private:
	static constexpr int32 ShardCount = 256;
	static int32 GetShardIndex(const FWorldEntityId EntityId)
	{
		return static_cast<int32>(GetTypeHash(EntityId) & (ShardCount - 1));
	}

	TStaticArray<TMap<FWorldEntityId, ValueType>, ShardCount> Shards;
	int32 EntryCount = 0;
};

class FWorldObjectWorldEntityShardedSet final
{
public:
	bool Contains(const FWorldEntityId EntityId) const
	{
		return Shards[GetShardIndex(EntityId)].Contains(EntityId);
	}

	void Add(const FWorldEntityId EntityId)
	{
		TSet<FWorldEntityId>& Shard = Shards[GetShardIndex(EntityId)];
		if (!Shard.Contains(EntityId))
		{
			Shard.Add(EntityId);
			++EntryCount;
		}
	}

	int32 Remove(const FWorldEntityId EntityId)
	{
		const int32 Removed = Shards[GetShardIndex(EntityId)].Remove(EntityId);
		EntryCount -= Removed;
		return Removed;
	}

	void Reserve(const int32 TotalCapacity)
	{
		const int32 PerShardCapacity = FMath::DivideAndRoundUp(FMath::Max(0, TotalCapacity), ShardCount);
		for (TSet<FWorldEntityId>& Shard : Shards)
		{
			Shard.Reserve(PerShardCapacity);
		}
	}

	int32 Num() const { return EntryCount; }

	SIZE_T GetAllocatedSize() const
	{
		SIZE_T Bytes = 0;
		for (const TSet<FWorldEntityId>& Shard : Shards)
		{
			Bytes += Shard.GetAllocatedSize();
		}
		return Bytes;
	}

private:
	static constexpr int32 ShardCount = 256;
	static int32 GetShardIndex(const FWorldEntityId EntityId)
	{
		return static_cast<int32>(GetTypeHash(EntityId) & (ShardCount - 1));
	}

	TStaticArray<TSet<FWorldEntityId>, ShardCount> Shards;
	int32 EntryCount = 0;
};

/** ECS 真值、空间索引和中性查询快照流必须在同一事务边界内更新。 */
struct FWorldObjectCoreState final
{
	FWorldObjectEntityRegistry Registry;
	FWorldObjectSpatialIndex SpatialIndex;
	FWorldObjectQuerySnapshotStream QuerySnapshots;
	TWorldObjectWorldEntityShardedMap<FWorldObjectEntityHandle> EntityByWorldEntityId;
	TMap<FName, TWeakObjectPtr<UWorldObjectDefinition>> DefinitionById;
};

struct FWorldObjectPendingMotionState final
{
	FWorldEntityId WorldEntityId;
	EWorldObjectMotionState State = EWorldObjectMotionState::Dormant;
};

/** Actor Proxy 与 Active Array 的可丢弃运行时状态。 */
struct FWorldObjectProjectionState final
{
	TArray<FWorldObjectEntityHandle> ActorActiveEntities;
	TArray<int32> ActorActiveIndexBySlot;
	TArray<TWeakObjectPtr<UWorldObjectProxyComponent>> ProxyBySlot;
	TMap<FWorldEntityId, TWeakObjectPtr<UWorldObjectProxyComponent>> PendingProxies;
	TArray<FWorldObjectPendingMotionState> PendingMotionStates;
	TArray<FWorldEntityId> PendingAuthorityDestroys;
	FDelegateHandle PostActorTickHandle;
	int32 LastSampledActiveCount = 0;
	int32 LastChangedTransformCount = 0;
	uint64 LastPostActorSyncFrame = MAX_uint64;
	float CurrentPostActorDeltaSeconds = 0.0f;
	int32 BoundProxyCount = 0;
};

/** WorldStorage 身份所有权、HomeChunk 与可插拔 Section 扩展。 */
struct FWorldObjectPersistenceState final
{
	FWorldObjectWorldEntityShardedSet OwnedEntities;
	TWorldObjectWorldEntityShardedMap<FWorldChunkCoord> HomeChunks;
	TWeakObjectPtr<UWorldStorageSubsystem> WorldStorage;
	TMap<FName, TSharedRef<IWorldObjectPersistenceExtension>> Extensions;
	TArray<FName> ExtensionOrder;
	TSharedPtr<IWorldStorageDomainAdapter> Adapter;
};

/** 薄协调对象：Gameplay 真值、Actor 投影和持久化所有权互不混放。 */
class FWorldObjectWorldRuntime final
{
public:
	using FPendingMotionState = FWorldObjectPendingMotionState;

	void EnsureSlotCapacity(const int32 Slot)
	{
		if (Projection.ActorActiveIndexBySlot.Num() <= Slot)
		{
			const int32 OldSize = Projection.ActorActiveIndexBySlot.Num();
			Projection.ActorActiveIndexBySlot.SetNum(Slot + 1);
			Projection.ProxyBySlot.SetNum(Slot + 1);
			for (int32 Index = OldSize; Index <= Slot; ++Index)
			{
				Projection.ActorActiveIndexBySlot[Index] = INDEX_NONE;
			}
		}
	}

	FWorldObjectCoreState Core;
	FWorldObjectProjectionState Projection;
	FWorldObjectPersistenceState Persistence;
};
