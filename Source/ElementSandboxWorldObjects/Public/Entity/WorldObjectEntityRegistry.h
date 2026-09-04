#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldObjectFragment.h"

class UScriptStruct;

using FWorldObjectFragmentPoolId = int32;
inline constexpr FWorldObjectFragmentPoolId InvalidWorldObjectFragmentPoolId = INDEX_NONE;

/** Registry 自身拥有的 CPU 存储统计，不包含空间、网络、资产或 RHI。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectEntityRegistryStorageStats final
{
	SIZE_T AllocatedBytes = 0;
	int32 FragmentPoolCount = 0;
	int32 SparseIndexPageCount = 0;
	int32 DenseFragmentCapacity = 0;
};

/** 连续 Pool 的只读成对视图；任意 Registry 结构修改后立即失效。 */
template <typename FragmentType>
struct TWorldObjectFragmentPoolView final
{
	TConstArrayView<FWorldObjectEntityHandle> Entities;
	TConstArrayView<FragmentType> Fragments;

	bool IsValid() const { return Entities.Num() == Fragments.Num(); }
	int32 Num() const { return IsValid() ? Fragments.Num() : 0; }
};

/** Game Thread 冻结结构后使用的连续可写视图；任意 Registry 结构修改后立即失效。 */
template <typename FragmentType>
struct TWorldObjectMutableFragmentPoolView final
{
	TConstArrayView<FWorldObjectEntityHandle> Entities;
	TArrayView<FragmentType> Fragments;

	bool IsValid() const { return Entities.Num() == Fragments.Num(); }
	int32 Num() const { return IsValid() ? Fragments.Num() : 0; }
};

/** 每 Fragment 类型一份连续数组；稀疏索引按 256 个 Entity Slot 分页。 */
class IWorldObjectFragmentPool
{
public:
	virtual ~IWorldObjectFragmentPool() = default;
	virtual bool Remove(FWorldObjectEntityHandle Entity) = 0;
	virtual void Reserve(int32 AdditionalCount) = 0;
	virtual int32 Num() const = 0;
	virtual int32 GetDenseCapacity() const = 0;
	virtual int32 GetSparseIndexPageCount() const = 0;
	virtual SIZE_T GetAllocatedSize() const = 0;
};

template <typename FragmentType>
class TWorldObjectFragmentPool final : public IWorldObjectFragmentPool
{
public:
	bool Add(const FWorldObjectEntityHandle Entity, const FragmentType& Fragment)
	{
		if (!Entity.IsSet())
		{
			return false;
		}
		FIndexPage& Page = FindOrAddIndexPage(Entity.GetSlot());
		int32& StoredIndex = Page.DenseIndices[Entity.GetSlot() & IndexPageMask];
		if (StoredIndex != INDEX_NONE)
		{
			return false;
		}

		const int32 DenseIndex = Values.Add(Fragment);
		Owners.Add(Entity);
		StoredIndex = DenseIndex;
		++Page.OccupiedSlotCount;
		return true;
	}

	bool Remove(const FWorldObjectEntityHandle Entity) override
	{
		const int32 DenseIndex = FindDenseIndex(Entity);
		if (DenseIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 LastIndex = Values.Num() - 1;
		if (DenseIndex != LastIndex)
		{
			Values[DenseIndex] = MoveTemp(Values[LastIndex]);
			Owners[DenseIndex] = Owners[LastIndex];
			FIndexPage* MovedPage = FindIndexPage(Owners[DenseIndex].GetSlot());
			check(MovedPage);
			MovedPage->DenseIndices[Owners[DenseIndex].GetSlot() & IndexPageMask] = DenseIndex;
		}
		Values.RemoveAt(LastIndex, EAllowShrinking::No);
		Owners.RemoveAt(LastIndex, EAllowShrinking::No);

		const int32 PageIndex = Entity.GetSlot() >> IndexPageShift;
		FIndexPage* Page = FindIndexPage(Entity.GetSlot());
		check(Page);
		int32& StoredIndex = Page->DenseIndices[Entity.GetSlot() & IndexPageMask];
		check(StoredIndex != INDEX_NONE);
		StoredIndex = INDEX_NONE;
		--Page->OccupiedSlotCount;
		check(Page->OccupiedSlotCount >= 0);
		if (Page->OccupiedSlotCount == 0)
		{
			DenseIndexPages[PageIndex].Reset();
			TrimEmptyIndexPages();
		}
		return true;
	}

	void Reserve(const int32 AdditionalCount) override
	{
		if (AdditionalCount > 0)
		{
			const int32 RequiredCapacity = Values.Num() + AdditionalCount;
			const int32 CurrentCapacity = FMath::Min(Values.Max(), Owners.Max());
			if (RequiredCapacity > CurrentCapacity)
			{
				// WorldStorage 以很小的 Chunk slice 注入；按 Num+slice 精确 Reserve 会反复搬迁百万级连续 Pool。
				const int64 GeometricCapacity = FMath::Max<int64>(
					RequiredCapacity,
					FMath::Max<int64>(4096, static_cast<int64>(CurrentCapacity) * 2));
				const int32 NewCapacity = static_cast<int32>(FMath::Min<int64>(GeometricCapacity, MAX_int32));
				Values.Reserve(NewCapacity);
				Owners.Reserve(NewCapacity);
			}
		}
	}

	FragmentType* Find(const FWorldObjectEntityHandle Entity)
	{
		const int32 DenseIndex = FindDenseIndex(Entity);
		return DenseIndex != INDEX_NONE ? &Values[DenseIndex] : nullptr;
	}

	const FragmentType* Find(const FWorldObjectEntityHandle Entity) const
	{
		return const_cast<TWorldObjectFragmentPool*>(this)->Find(Entity);
	}

	int32 Num() const override { return Values.Num(); }
	int32 GetDenseCapacity() const override { return Values.Max(); }
	int32 GetSparseIndexPageCount() const override
	{
		int32 Count = 0;
		for (const TUniquePtr<FIndexPage>& Page : DenseIndexPages)
		{
			Count += Page ? 1 : 0;
		}
		return Count;
	}
	SIZE_T GetAllocatedSize() const override
	{
		SIZE_T AllocatedSize = Values.GetAllocatedSize()
			+ Owners.GetAllocatedSize()
			+ DenseIndexPages.GetAllocatedSize();
		for (const TUniquePtr<FIndexPage>& Page : DenseIndexPages)
		{
			AllocatedSize += Page ? sizeof(FIndexPage) : 0;
		}
		return AllocatedSize;
	}
	TConstArrayView<FragmentType> GetValues() const { return Values; }
	TArrayView<FragmentType> GetMutableValues() { return Values; }
	TConstArrayView<FWorldObjectEntityHandle> GetOwners() const { return Owners; }

private:
	static constexpr int32 IndexPageShift = 8;
	static constexpr int32 IndexPageSize = 1 << IndexPageShift;
	static constexpr int32 IndexPageMask = IndexPageSize - 1;

	struct FIndexPage final
	{
		FIndexPage()
		{
			for (int32& DenseIndex : DenseIndices)
			{
				DenseIndex = INDEX_NONE;
			}
		}

		TStaticArray<int32, IndexPageSize> DenseIndices;
		int32 OccupiedSlotCount = 0;
	};

	int32 FindDenseIndex(const FWorldObjectEntityHandle Entity) const
	{
		if (!Entity.IsSet())
		{
			return INDEX_NONE;
		}
		const FIndexPage* Page = FindIndexPage(Entity.GetSlot());
		if (!Page)
		{
			return INDEX_NONE;
		}
		const int32 DenseIndex = Page->DenseIndices[Entity.GetSlot() & IndexPageMask];
		return Owners.IsValidIndex(DenseIndex) && Owners[DenseIndex] == Entity
			? DenseIndex
			: INDEX_NONE;
	}

	FIndexPage* FindIndexPage(const int32 EntitySlot)
	{
		return const_cast<FIndexPage*>(
			static_cast<const TWorldObjectFragmentPool*>(this)->FindIndexPage(EntitySlot));
	}

	const FIndexPage* FindIndexPage(const int32 EntitySlot) const
	{
		if (EntitySlot < 0)
		{
			return nullptr;
		}
		const int32 PageIndex = EntitySlot >> IndexPageShift;
		return DenseIndexPages.IsValidIndex(PageIndex)
			? DenseIndexPages[PageIndex].Get()
			: nullptr;
	}

	FIndexPage& FindOrAddIndexPage(const int32 EntitySlot)
	{
		check(EntitySlot >= 0);
		const int32 PageIndex = EntitySlot >> IndexPageShift;
		if (!DenseIndexPages.IsValidIndex(PageIndex))
		{
			DenseIndexPages.SetNum(PageIndex + 1);
		}
		if (!DenseIndexPages[PageIndex])
		{
			DenseIndexPages[PageIndex] = MakeUnique<FIndexPage>();
		}
		return *DenseIndexPages[PageIndex];
	}

	void TrimEmptyIndexPages()
	{
		while (!DenseIndexPages.IsEmpty() && !DenseIndexPages.Last())
		{
			DenseIndexPages.Pop(EAllowShrinking::No);
		}
		if (DenseIndexPages.IsEmpty()
			|| DenseIndexPages.GetSlack() >= DenseIndexPages.Num())
		{
			DenseIndexPages.Shrink();
		}
	}

	TArray<FragmentType> Values;
	TArray<FWorldObjectEntityHandle> Owners;
	TArray<TUniquePtr<FIndexPage>> DenseIndexPages;
};

/** WorldObject 的纯值 Entity Registry；不创建 per-entity UObject。 */
class ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectEntityRegistry final
{
public:
	FWorldObjectEntityRegistry();
	~FWorldObjectEntityRegistry();
	FWorldObjectEntityRegistry(const FWorldObjectEntityRegistry&) = delete;
	FWorldObjectEntityRegistry& operator=(const FWorldObjectEntityRegistry&) = delete;

	FWorldObjectEntityHandle CreateEntity();
	/** 为批量 Restore 的 Entity 行与核心 Fragment 连续区预留空间。 */
	void ReserveEntities(int32 AdditionalEntityCount);

	template <typename FragmentType>
	void ReserveFragments(const int32 AdditionalFragmentCount)
	{
		static_assert(TIsDerivedFrom<FragmentType, FWorldObjectFragment>::IsDerived);
		const FWorldObjectFragmentPoolId PoolId = FindOrCreatePool<FragmentType>();
		PoolsById[PoolId]->Reserve(AdditionalFragmentCount);
	}
	bool DestroyEntity(FWorldObjectEntityHandle Entity);
	bool IsAlive(FWorldObjectEntityHandle Entity) const;
	int32 GetEntityCount() const { return EntityCount; }
	uint32 GetRegistryId() const { return RegistryId; }
	FWorldObjectEntityRegistryStorageStats GetStorageStats() const;
	void Reset();

	template <typename FragmentType>
	bool AddFragment(const FWorldObjectEntityHandle Entity, const FragmentType& Fragment)
	{
		static_assert(TIsDerivedFrom<FragmentType, FWorldObjectFragment>::IsDerived);
		if (!IsAlive(Entity))
		{
			return false;
		}

		FEntityRecord& Record = Records[Entity.GetSlot()];
		const FWorldObjectFragmentPoolId PoolId = FindOrCreatePool<FragmentType>();
		if (Record.OwnedFragmentPoolIds.Contains(PoolId))
		{
			return false;
		}

		TWorldObjectFragmentPool<FragmentType>& Pool =
			*static_cast<TWorldObjectFragmentPool<FragmentType>*>(PoolsById[PoolId].Get());
		if (!Pool.Add(Entity, Fragment))
		{
			return false;
		}
		Record.OwnedFragmentPoolIds.Add(PoolId);
		return true;
	}

	template <typename FragmentType>
	bool RemoveFragment(const FWorldObjectEntityHandle Entity)
	{
		static_assert(TIsDerivedFrom<FragmentType, FWorldObjectFragment>::IsDerived);
		if (!IsAlive(Entity))
		{
			return false;
		}

		const UScriptStruct* Type = FragmentType::StaticStruct();
		const FWorldObjectFragmentPoolId* PoolId = PoolIdByType.Find(Type);
		if (!PoolId || !PoolsById.IsValidIndex(*PoolId)
			|| !PoolsById[*PoolId]->Remove(Entity))
		{
			return false;
		}
		const int32 Removed = Records[Entity.GetSlot()].OwnedFragmentPoolIds.RemoveSingleSwap(
			*PoolId,
			EAllowShrinking::No);
		check(Removed == 1);
		return true;
	}

	template <typename FragmentType>
	FragmentType* FindMutableFragment(const FWorldObjectEntityHandle Entity)
	{
		static_assert(TIsDerivedFrom<FragmentType, FWorldObjectFragment>::IsDerived);
		if (!IsAlive(Entity))
		{
			return nullptr;
		}
		const FWorldObjectFragmentPoolId* PoolId =
			PoolIdByType.Find(FragmentType::StaticStruct());
		return PoolId && PoolsById.IsValidIndex(*PoolId)
			? static_cast<TWorldObjectFragmentPool<FragmentType>*>(PoolsById[*PoolId].Get())->Find(Entity)
			: nullptr;
	}

	template <typename FragmentType>
	const FragmentType* FindFragment(const FWorldObjectEntityHandle Entity) const
	{
		return const_cast<FWorldObjectEntityRegistry*>(this)
			->FindMutableFragment<FragmentType>(Entity);
	}

	template <typename FragmentType>
	bool HasFragment(const FWorldObjectEntityHandle Entity) const
	{
		return FindFragment<FragmentType>(Entity) != nullptr;
	}

	template <typename FragmentType>
	int32 GetFragmentCount() const
	{
		const FWorldObjectFragmentPoolId* PoolId =
			PoolIdByType.Find(FragmentType::StaticStruct());
		return PoolId && PoolsById.IsValidIndex(*PoolId) ? PoolsById[*PoolId]->Num() : 0;
	}

	/**
	 * Game Thread 批量读取接口，供下一层 Provider 等消费者连续访问。
	 * 返回视图只在下一次 Registry 结构修改前有效，调用方不得跨帧缓存。
	 */
	template <typename FragmentType>
	TWorldObjectFragmentPoolView<FragmentType> GetFragmentPoolView() const
	{
		static_assert(TIsDerivedFrom<FragmentType, FWorldObjectFragment>::IsDerived);
		const FWorldObjectFragmentPoolId* PoolId =
			PoolIdByType.Find(FragmentType::StaticStruct());
		if (!PoolId || !PoolsById.IsValidIndex(*PoolId))
		{
			return {};
		}
		const TWorldObjectFragmentPool<FragmentType>* Pool =
			static_cast<const TWorldObjectFragmentPool<FragmentType>*>(PoolsById[*PoolId].Get());
		return {Pool->GetOwners(), Pool->GetValues()};
	}

	/**
	 * Provider 等 Game Thread 阶段拥有者使用的批量可写接口。返回视图只在下一次
	 * Registry 结构修改前有效，不允许跨 Barrier 或跨帧缓存。
	 */
	template <typename FragmentType>
	TWorldObjectMutableFragmentPoolView<FragmentType> GetMutableFragmentPoolView()
	{
		static_assert(TIsDerivedFrom<FragmentType, FWorldObjectFragment>::IsDerived);
		const FWorldObjectFragmentPoolId* PoolId =
			PoolIdByType.Find(FragmentType::StaticStruct());
		if (!PoolId || !PoolsById.IsValidIndex(*PoolId))
		{
			return {};
		}
		TWorldObjectFragmentPool<FragmentType>* Pool =
			static_cast<TWorldObjectFragmentPool<FragmentType>*>(PoolsById[*PoolId].Get());
		return {Pool->GetOwners(), Pool->GetMutableValues()};
	}

private:
	struct FEntityRecord final
	{
		uint32 Generation = 1;
		int32 NextFreeSlot = INDEX_NONE;
		bool bAlive = false;
		TArray<FWorldObjectFragmentPoolId, TInlineAllocator<4>> OwnedFragmentPoolIds;
	};

	template <typename FragmentType>
	FWorldObjectFragmentPoolId FindOrCreatePool()
	{
		const UScriptStruct* Type = FragmentType::StaticStruct();
		if (const FWorldObjectFragmentPoolId* Existing = PoolIdByType.Find(Type))
		{
			return *Existing;
		}
		const FWorldObjectFragmentPoolId PoolId =
			PoolsById.Add(MakeUnique<TWorldObjectFragmentPool<FragmentType>>());
		PoolIdByType.Add(Type, PoolId);
		return PoolId;
	}

	uint32 RegistryId = 0;
	TArray<FEntityRecord> Records;
	TArray<TUniquePtr<IWorldObjectFragmentPool>> PoolsById;
	TMap<const UScriptStruct*, FWorldObjectFragmentPoolId> PoolIdByType;
	int32 FirstFreeSlot = INDEX_NONE;
	int32 EntityCount = 0;
};
