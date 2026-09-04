#pragma once

#include "CoreMinimal.h"

/**
 * Fire host 投影会在大城加载时达到百万级。单个 TMap 的几何扩容会在一次 Shape 写入中搬迁
 * 整张表，帧预算无法打断；固定分片把最坏扩容限制在一个小 Shard 内，同时保留 O(1) 查找语义。
 */
template <typename KeyType, typename ValueType, uint32 ShardCount = 256>
class TElementFireShardedMap final
{
	static_assert(ShardCount > 0 && (ShardCount & (ShardCount - 1)) == 0,
		"ShardCount must be a power of two.");

public:
	ValueType* Find(const KeyType& Key)
	{
		return GetShard(Key).Find(Key);
	}

	const ValueType* Find(const KeyType& Key) const
	{
		return GetShard(Key).Find(Key);
	}

	ValueType& FindOrAdd(const KeyType& Key)
	{
		FShard& Shard = GetShard(Key);
		if (ValueType* Existing = Shard.Find(Key))
		{
			return *Existing;
		}
		++EntryCount;
		return Shard.Add(Key);
	}

	ValueType& Add(const KeyType& Key, const ValueType& Value)
	{
		FShard& Shard = GetShard(Key);
		EntryCount += Shard.Contains(Key) ? 0 : 1;
		return Shard.Add(Key, Value);
	}

	ValueType& Add(const KeyType& Key, ValueType&& Value)
	{
		FShard& Shard = GetShard(Key);
		EntryCount += Shard.Contains(Key) ? 0 : 1;
		return Shard.Add(Key, MoveTemp(Value));
	}

	bool Contains(const KeyType& Key) const
	{
		return GetShard(Key).Contains(Key);
	}

	int32 Remove(const KeyType& Key)
	{
		const int32 Removed = GetShard(Key).Remove(Key);
		EntryCount -= Removed;
		check(EntryCount >= 0);
		return Removed;
	}

	void Reset()
	{
		for (FShard& Shard : Shards)
		{
			Shard.Reset();
		}
		EntryCount = 0;
	}

	int32 Num() const
	{
		return EntryCount;
	}

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
	using FShard = TMap<KeyType, ValueType>;

	static uint32 GetShardIndex(const KeyType& Key)
	{
		return GetTypeHash(Key) & (ShardCount - 1);
	}

	FShard& GetShard(const KeyType& Key)
	{
		return Shards[GetShardIndex(Key)];
	}

	const FShard& GetShard(const KeyType& Key) const
	{
		return Shards[GetShardIndex(Key)];
	}

	TStaticArray<FShard, ShardCount> Shards;
	int32 EntryCount = 0;
};
