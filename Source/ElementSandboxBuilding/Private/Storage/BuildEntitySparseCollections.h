#pragma once

#include "Entity/BuildEntityHandle.h"
#include "Storage/BuildStableArrayAllocator.h"

/**
 * 以 Entity Slot 直达 Dense 行的私有 Sparse Map。
 * Dense 与 Sparse 都保留 TArray 连续语义，并使用稳定虚拟地址分配器避免增长时搬迁历史行。
 */
template <typename ValueType>
class TBuildEntitySparseMap final
{
public:
	struct FEntry final
	{
		FBuildEntityHandle Entity;
		ValueType Value;
	};

	ValueType* Find(const FBuildEntityHandle Entity)
	{
		const int32 DenseIndex = FindDenseIndex(Entity);
		return DenseIndex != INDEX_NONE ? &DenseEntries[DenseIndex].Value : nullptr;
	}

	const ValueType* Find(const FBuildEntityHandle Entity) const
	{
		return const_cast<TBuildEntitySparseMap*>(this)->Find(Entity);
	}

	bool Contains(const FBuildEntityHandle Entity) const
	{
		return FindDenseIndex(Entity) != INDEX_NONE;
	}

	ValueType& FindOrAdd(const FBuildEntityHandle Entity)
	{
		if (ValueType* Existing = Find(Entity))
		{
			return *Existing;
		}
		return AddDefaulted(Entity);
	}

	ValueType& Add(const FBuildEntityHandle Entity, ValueType&& Value)
	{
		ValueType& Stored = AddDefaulted(Entity);
		Stored = MoveTemp(Value);
		return Stored;
	}

	int32 Remove(const FBuildEntityHandle Entity)
	{
		const int32 DenseIndex = FindDenseIndex(Entity);
		if (DenseIndex == INDEX_NONE)
		{
			return 0;
		}

		const int32 LastDenseIndex = DenseEntries.Num() - 1;
		const FBuildEntityHandle MovedEntity = DenseEntries[LastDenseIndex].Entity;
		DenseIndexBySlot[Entity.GetIndex()] = 0;
		DenseEntries.RemoveAtSwap(DenseIndex, 1, EAllowShrinking::No);
		if (DenseIndex != LastDenseIndex)
		{
			check(MovedEntity.IsSet());
			DenseIndexBySlot[MovedEntity.GetIndex()] = DenseIndex + 1;
		}
		return 1;
	}

	void Reset()
	{
		DenseEntries.Reset();
		DenseIndexBySlot.Reset();
	}

	int32 Num() const { return DenseEntries.Num(); }
	bool IsEmpty() const { return DenseEntries.IsEmpty(); }

	const TArray<FEntry, FBuildStableArrayAllocator>& GetEntries() const
	{
		return DenseEntries;
	}

	void GenerateKeyArray(TArray<FBuildEntityHandle>& OutEntities) const
	{
		OutEntities.Reset(DenseEntries.Num());
		for (const FEntry& Entry : DenseEntries)
		{
			OutEntities.Add(Entry.Entity);
		}
	}

	SIZE_T GetAllocatedSize() const
	{
		return DenseEntries.GetAllocatedSize() + DenseIndexBySlot.GetAllocatedSize();
	}

private:
	ValueType& AddDefaulted(const FBuildEntityHandle Entity)
	{
		check(Entity.IsSet());
		check(!Contains(Entity));
		EnsureSparseSlot(Entity.GetIndex());
		check(DenseIndexBySlot[Entity.GetIndex()] == 0);
		FEntry& Entry = DenseEntries.AddDefaulted_GetRef();
		Entry.Entity = Entity;
		// AddDefaulted 对 int32 等平凡类型不保证清零；Dense 行在 swap-remove 后会复用旧字节。
		// FindOrAdd 必须与 TMap::FindOrAdd 一样返回值初始化的新 Value，否则引用计数会继承残值。
		Entry.Value = ValueType{};
		DenseIndexBySlot[Entity.GetIndex()] = DenseEntries.Num();
		return Entry.Value;
	}

	int32 FindDenseIndex(const FBuildEntityHandle Entity) const
	{
		if (!Entity.IsSet() || !DenseIndexBySlot.IsValidIndex(Entity.GetIndex()))
		{
			return INDEX_NONE;
		}
		const int32 EncodedDenseIndex = DenseIndexBySlot[Entity.GetIndex()];
		const int32 DenseIndex = EncodedDenseIndex - 1;
		return EncodedDenseIndex > 0
			&& DenseEntries.IsValidIndex(DenseIndex)
			&& DenseEntries[DenseIndex].Entity == Entity
			? DenseIndex
			: INDEX_NONE;
	}

	void EnsureSparseSlot(const int32 EntitySlot)
	{
		check(EntitySlot >= 0);
		if (!DenseIndexBySlot.IsValidIndex(EntitySlot))
		{
			DenseIndexBySlot.SetNumZeroed(EntitySlot + 1, EAllowShrinking::No);
		}
	}

	TArray<FEntry, FBuildStableArrayAllocator> DenseEntries;
	/** 0 表示空；非零值为 DenseIndex + 1。 */
	TArray<int32, FBuildStableArrayAllocator> DenseIndexBySlot;
};

/** 只保存 Entity 身份的同构 Sparse Set。 */
class FBuildEntitySparseSet final
{
public:
	bool Contains(const FBuildEntityHandle Entity) const
	{
		return FindDenseIndex(Entity) != INDEX_NONE;
	}

	bool Add(const FBuildEntityHandle Entity)
	{
		if (!Entity.IsSet() || Contains(Entity))
		{
			return false;
		}
		EnsureSparseSlot(Entity.GetIndex());
		check(DenseIndexBySlot[Entity.GetIndex()] == 0);
		DenseEntities.Add(Entity);
		DenseIndexBySlot[Entity.GetIndex()] = DenseEntities.Num();
		return true;
	}

	int32 Remove(const FBuildEntityHandle Entity)
	{
		const int32 DenseIndex = FindDenseIndex(Entity);
		if (DenseIndex == INDEX_NONE)
		{
			return 0;
		}

		const int32 LastDenseIndex = DenseEntities.Num() - 1;
		const FBuildEntityHandle MovedEntity = DenseEntities[LastDenseIndex];
		DenseIndexBySlot[Entity.GetIndex()] = 0;
		DenseEntities.RemoveAtSwap(DenseIndex, 1, EAllowShrinking::No);
		if (DenseIndex != LastDenseIndex)
		{
			check(MovedEntity.IsSet());
			DenseIndexBySlot[MovedEntity.GetIndex()] = DenseIndex + 1;
		}
		return 1;
	}

	void Reset()
	{
		DenseEntities.Reset();
		DenseIndexBySlot.Reset();
	}

	int32 Num() const { return DenseEntities.Num(); }
	bool IsEmpty() const { return DenseEntities.IsEmpty(); }

	SIZE_T GetAllocatedSize() const
	{
		return DenseEntities.GetAllocatedSize() + DenseIndexBySlot.GetAllocatedSize();
	}

private:
	int32 FindDenseIndex(const FBuildEntityHandle Entity) const
	{
		if (!Entity.IsSet() || !DenseIndexBySlot.IsValidIndex(Entity.GetIndex()))
		{
			return INDEX_NONE;
		}
		const int32 EncodedDenseIndex = DenseIndexBySlot[Entity.GetIndex()];
		const int32 DenseIndex = EncodedDenseIndex - 1;
		return EncodedDenseIndex > 0
			&& DenseEntities.IsValidIndex(DenseIndex)
			&& DenseEntities[DenseIndex] == Entity
			? DenseIndex
			: INDEX_NONE;
	}

	void EnsureSparseSlot(const int32 EntitySlot)
	{
		check(EntitySlot >= 0);
		if (!DenseIndexBySlot.IsValidIndex(EntitySlot))
		{
			DenseIndexBySlot.SetNumZeroed(EntitySlot + 1, EAllowShrinking::No);
		}
	}

	TArray<FBuildEntityHandle, FBuildStableArrayAllocator> DenseEntities;
	/** 0 表示空；非零值为 DenseIndex + 1。 */
	TArray<int32, FBuildStableArrayAllocator> DenseIndexBySlot;
};
