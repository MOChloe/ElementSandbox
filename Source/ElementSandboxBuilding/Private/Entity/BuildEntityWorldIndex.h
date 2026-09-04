#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/WorldEntityId.h"

/** WorldEntityId 到本地 Generation Handle 的唯一运行期索引；分片限制单次扩容规模。 */
class FBuildEntityWorldIndex final
{
public:
	const FBuildEntityHandle* Find(const FWorldEntityId WorldEntityId) const
	{
		return Shards[GetShardIndex(WorldEntityId)].Find(WorldEntityId);
	}

	bool Contains(const FWorldEntityId WorldEntityId) const { return Find(WorldEntityId) != nullptr; }

	void Add(const FWorldEntityId WorldEntityId, const FBuildEntityHandle Entity)
	{
		check(WorldEntityId.IsSet() && Entity.IsSet() && !Contains(WorldEntityId));
		Shards[GetShardIndex(WorldEntityId)].Add(WorldEntityId, Entity);
		++EntityCount;
	}

	int32 Remove(const FWorldEntityId WorldEntityId)
	{
		const int32 Removed = Shards[GetShardIndex(WorldEntityId)].Remove(WorldEntityId);
		EntityCount -= Removed;
		return Removed;
	}

	int32 Num() const { return EntityCount; }

	SIZE_T GetAllocatedSize() const
	{
		SIZE_T Bytes = 0;
		for (const FShard& Shard : Shards)
		{
			Bytes += Shard.GetAllocatedSize();
		}
		return Bytes;
	}

private:
	friend class FBuildEntityWorldIndexHashDistributionTest;
	static constexpr uint32 ShardBits = 8;
	static constexpr uint32 ShardCount = 1u << ShardBits;
	struct FShardKeyFuncs : TDefaultMapHashableKeyFuncs<FWorldEntityId, FBuildEntityHandle, false>
	{
		static uint32 GetKeyHash(const FWorldEntityId WorldEntityId)
		{
			// 低八位已经选定分片，分片内它们完全相同。TMap 也用低位选桶，
			// 必须移除已消费的位，否则连续 ID 会退化成长达数百项的冲突链。
			return GetTypeHash(WorldEntityId) >> ShardBits;
		}
	};
	using FShard = TMap<FWorldEntityId, FBuildEntityHandle, FDefaultSetAllocator, FShardKeyFuncs>;

	static uint32 GetShardIndex(const FWorldEntityId WorldEntityId)
	{
		return GetTypeHash(WorldEntityId) & (ShardCount - 1);
	}

	TStaticArray<FShard, ShardCount> Shards;
	int32 EntityCount = 0;
};
