#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Storage/BuildStableArrayAllocator.h"
#include "StructUtils/StructView.h"

class UScriptStruct;

using FBuildFragmentPoolId = int32;
inline constexpr FBuildFragmentPoolId InvalidBuildFragmentPoolId = INDEX_NONE;

/** 单一 Fragment 类型的连续稠密 Pool；只由 FBuildFragmentStore 创建。 */
class FBuildFragmentPool final
{
public:
	explicit FBuildFragmentPool(const UScriptStruct& InFragmentType);
	~FBuildFragmentPool();

	FBuildFragmentPool(const FBuildFragmentPool&) = delete;
	FBuildFragmentPool& operator=(const FBuildFragmentPool&) = delete;

	bool Add(FBuildEntityHandle Entity, FConstStructView Fragment);
	bool Remove(FBuildEntityHandle Entity);
	FConstStructView Find(FBuildEntityHandle Entity) const;
	FStructView FindMutable(FBuildEntityHandle Entity);
	int32 Num() const;
	void Reserve(int32 NewCapacity);
	SIZE_T GetAllocatedSize() const;
	void GetEntities(TArray<FBuildEntityHandle>& OutEntities) const;
	TConstArrayView<FBuildEntityHandle> GetEntityView() const
	{
		return TConstArrayView<FBuildEntityHandle>(Entities.GetData(), Entities.Num());
	}
	const uint8* GetData() const { return FragmentCount > 0 ? FragmentMemory : nullptr; }
	uint8* GetMutableData() { return FragmentCount > 0 ? FragmentMemory : nullptr; }
	int32 GetStride() const { return FragmentStride; }

private:
	static constexpr int32 IndexPageShift = 8;
	static constexpr int32 IndexPageSize = 1 << IndexPageShift;
	static constexpr int32 IndexPageMask = IndexPageSize - 1;

	struct FIndexPage final
	{
		FIndexPage();

		TStaticArray<int32, IndexPageSize> DenseIndices;
		int32 OccupiedSlotCount = 0;
	};

	int32 FindDenseIndex(FBuildEntityHandle Entity) const;
	void AddDenseIndex(int32 EntitySlot, int32 DenseIndex);
	void UpdateDenseIndex(int32 EntitySlot, int32 DenseIndex);
	void RemoveDenseIndex(int32 EntitySlot);
	FIndexPage* FindIndexPage(int32 EntitySlot);
	const FIndexPage* FindIndexPage(int32 EntitySlot) const;
	FIndexPage& FindOrAddIndexPage(int32 EntitySlot);
	void TrimEmptyIndexPages();
	void EnsureFragmentAddressCapacity(int32 RequiredCapacity);
	uint8* GetFragmentMemory(int32 Index);
	const uint8* GetFragmentMemory(int32 Index) const;
	void Empty();

	const UScriptStruct* FragmentType = nullptr;
	FBuildStableVirtualMemory FragmentStorage;
	uint8* FragmentMemory = nullptr;
	int32 FragmentStride = 0;
	int32 FragmentAlignment = 0;
	int32 FragmentCount = 0;
	int32 FragmentCapacity = 0;
	int32 FragmentAddressCapacity = 0;
	TArray<FBuildEntityHandle, FBuildStableArrayAllocator> Entities;
	/** 256 个 Entity Slot 一页；Generation 仍由 Dense row 中的完整 Handle 校验。 */
	TArray<TUniquePtr<FIndexPage>> DenseIndexPages;
};

/** Registry 私有的多类型 Fragment Pool 所有者。 */
class FBuildFragmentStore final
{
public:
	bool Add(
		FBuildEntityHandle Entity,
		FConstStructView Fragment,
		FBuildFragmentPoolId& OutPoolId);
	bool Reserve(const UScriptStruct& FragmentType, int32 NewCapacity);
	bool Remove(
		FBuildEntityHandle Entity,
		const UScriptStruct& FragmentType,
		FBuildFragmentPoolId& OutPoolId);
	void RemoveEntity(
		FBuildEntityHandle Entity,
		TConstArrayView<FBuildFragmentPoolId> OwnedPoolIds);

	FConstStructView Find(FBuildEntityHandle Entity, const UScriptStruct& FragmentType) const;
	FStructView FindMutable(FBuildEntityHandle Entity, const UScriptStruct& FragmentType);
	int32 GetFragmentCount(const UScriptStruct& FragmentType) const;
	SIZE_T GetAllocatedSize() const;
	void GetEntities(
		const UScriptStruct& FragmentType,
		TArray<FBuildEntityHandle>& OutEntities) const;
	const FBuildFragmentPool* GetPool(const UScriptStruct& FragmentType) const
	{
		return FindPool(FragmentType);
	}
	FBuildFragmentPool* GetMutablePool(const UScriptStruct& FragmentType)
	{
		return FindPool(FragmentType);
	}
	void Reset();

private:
	static bool IsSupportedFragmentType(const UScriptStruct& FragmentType);
	FBuildFragmentPoolId FindPoolId(const UScriptStruct& FragmentType) const;
	FBuildFragmentPool* FindPool(const UScriptStruct& FragmentType);
	const FBuildFragmentPool* FindPool(const UScriptStruct& FragmentType) const;

	/** PoolId 在一次 Registry 生命周期内稳定，空 Pool 也保留到 Reset。 */
	TArray<TUniquePtr<FBuildFragmentPool>> PoolsById;
	TMap<const UScriptStruct*, FBuildFragmentPoolId> PoolIdByType;
};
