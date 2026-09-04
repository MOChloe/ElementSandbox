#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/BuildFragment.h"
#include "StructUtils/StructView.h"
#include "Templates/UniquePtr.h"

class FBuildEntityRegistryData;
class FBuildProcessorScheduler;
class UScriptStruct;

/** 连续 Building Fragment Pool 的只读视图；结构修改会改变行集合或行归属，禁止跨修改缓存。 */
template <typename FragmentType>
struct TBuildFragmentPoolView final
{
	TConstArrayView<FBuildEntityHandle> Entities;
	const uint8* Data = nullptr;
	int32 Stride = 0;

	bool IsValid() const
	{
		return Entities.IsEmpty() ? Data == nullptr || Stride > 0 : Data && Stride > 0;
	}

	int32 Num() const { return IsValid() ? Entities.Num() : 0; }

	const FragmentType* Get(const int32 Index) const
	{
		return Entities.IsValidIndex(Index) && Data
			? reinterpret_cast<const FragmentType*>(Data + static_cast<SIZE_T>(Index) * Stride)
			: nullptr;
	}

	int32 IndexOf(const FragmentType* Fragment) const
	{
		if (!Fragment || !Data || Stride <= 0)
		{
			return INDEX_NONE;
		}
		const ptrdiff_t Offset = reinterpret_cast<const uint8*>(Fragment) - Data;
		return Offset >= 0 && Offset % Stride == 0 && Entities.IsValidIndex(Offset / Stride)
			? static_cast<int32>(Offset / Stride)
			: INDEX_NONE;
	}
};

/** Game Thread 阶段拥有者使用的连续可写视图；不得跨结构修改或跨帧缓存。 */
template <typename FragmentType>
struct TBuildMutableFragmentPoolView final
{
	TConstArrayView<FBuildEntityHandle> Entities;
	uint8* Data = nullptr;
	int32 Stride = 0;

	bool IsValid() const
	{
		return Entities.IsEmpty() ? Data == nullptr || Stride > 0 : Data && Stride > 0;
	}

	int32 Num() const { return IsValid() ? Entities.Num() : 0; }

	FragmentType* Get(const int32 Index) const
	{
		return Entities.IsValidIndex(Index) && Data
			? reinterpret_cast<FragmentType*>(Data + static_cast<SIZE_T>(Index) * Stride)
			: nullptr;
	}

	int32 IndexOf(const FragmentType* Fragment) const
	{
		if (!Fragment || !Data || Stride <= 0)
		{
			return INDEX_NONE;
		}
		const ptrdiff_t Offset = reinterpret_cast<const uint8*>(Fragment) - Data;
		return Offset >= 0 && Offset % Stride == 0 && Entities.IsValidIndex(Offset / Stride)
			? static_cast<int32>(Offset / Stride)
			: INDEX_NONE;
	}
};

/**
 * Building ECS Entity 生命周期与 Fragment 数据的唯一所有者。
 *
 * Registry 是无 Tick 的纯 C++ 数据容器。所有结构修改限定在 Game Thread；添加或
 * 移除同类型 Fragment 会执行 Swap-Remove 并改变行归属，禁止跨结构修改缓存；单纯容量
 * Reserve 在稳定虚拟地址内提交新页，不再搬迁已有 Fragment。
 */
class ELEMENTSANDBOXBUILDING_API FBuildEntityRegistry final
{
public:
	FBuildEntityRegistry();
	~FBuildEntityRegistry();

	FBuildEntityRegistry(const FBuildEntityRegistry&) = delete;
	FBuildEntityRegistry& operator=(const FBuildEntityRegistry&) = delete;
	FBuildEntityRegistry(FBuildEntityRegistry&&) = delete;
	FBuildEntityRegistry& operator=(FBuildEntityRegistry&&) = delete;

	FBuildEntityHandle CreateEntity();
	void ReserveEntityCapacity(int32 EntityCapacity);

	template <typename FragmentType>
	bool ReserveFragmentCapacity(const int32 FragmentCapacity)
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		return ReserveFragmentCapacityByType(
			*FragmentType::StaticStruct(), FragmentCapacity);
	}
	bool DestroyEntity(FBuildEntityHandle Entity);
	bool IsAlive(FBuildEntityHandle Entity) const;
	int32 GetEntityCount() const;
	SIZE_T GetEstimatedAllocatedSize() const;

	template <typename FragmentType>
	bool AddFragment(const FBuildEntityHandle Entity, const FragmentType& Fragment)
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		return AddFragmentView(Entity, FConstStructView(
			FragmentType::StaticStruct(), reinterpret_cast<const uint8*>(&Fragment)));
	}

	template <typename FragmentType>
	bool RemoveFragment(const FBuildEntityHandle Entity)
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		return RemoveFragmentByType(Entity, *FragmentType::StaticStruct());
	}

	template <typename FragmentType>
	const FragmentType* FindFragment(const FBuildEntityHandle Entity) const
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		return FindFragmentView(Entity, *FragmentType::StaticStruct())
			.template GetPtr<const FragmentType>();
	}

	template <typename FragmentType>
	FragmentType* FindMutableFragment(const FBuildEntityHandle Entity)
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		return FindMutableFragmentView(Entity, *FragmentType::StaticStruct())
			.template GetPtr<FragmentType>();
	}

	template <typename FragmentType>
	bool HasFragment(const FBuildEntityHandle Entity) const
	{
		return FindFragment<FragmentType>(Entity) != nullptr;
	}

	template <typename FragmentType>
	int32 GetFragmentCount() const
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		return GetFragmentCountByType(*FragmentType::StaticStruct());
	}

	/**
	 * 用指定 Fragment Pool 当前的稠密顺序复制 Entity Handle 快照。
	 * 不承诺排序；调用方应复用 OutEntities，并在后续访问时继续通过 Registry 校验 Handle。
	 */
	template <typename FragmentType>
	void GetEntitiesWithFragment(TArray<FBuildEntityHandle>& OutEntities) const
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		GetEntitiesWithFragmentByType(*FragmentType::StaticStruct(), OutEntities);
	}

	template <typename FragmentType>
	TBuildFragmentPoolView<FragmentType> GetFragmentPoolView() const
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		TConstArrayView<FBuildEntityHandle> Entities;
		const uint8* DataPtr = nullptr;
		int32 Stride = 0;
		GetFragmentPoolViewByType(*FragmentType::StaticStruct(), Entities, DataPtr, Stride);
		return {Entities, DataPtr, Stride};
	}

	template <typename FragmentType>
	TBuildMutableFragmentPoolView<FragmentType> GetMutableFragmentPoolView()
	{
		static_assert(std::is_base_of_v<FBuildFragment, FragmentType>,
			"Building ECS fragments must derive from FBuildFragment.");
		TConstArrayView<FBuildEntityHandle> Entities;
		uint8* DataPtr = nullptr;
		int32 Stride = 0;
		GetMutableFragmentPoolViewByType(*FragmentType::StaticStruct(), Entities, DataPtr, Stride);
		return {Entities, DataPtr, Stride};
	}

	/** 清空全部 Entity 和 Fragment，并使此前发出的所有 Handle 失效。 */
	void Reset();

private:
	/** 转换 Barrier 专用：结构冻结期间允许 Worker 进行只读类型化查找。 */
	void BeginFrozenParallelRead() const;
	void EndFrozenParallelRead() const;
	bool IsFrozenParallelReadActive() const;

	bool AddFragmentView(FBuildEntityHandle Entity, FConstStructView Fragment);
	bool ReserveFragmentCapacityByType(
		const UScriptStruct& FragmentType,
		int32 FragmentCapacity);
	bool RemoveFragmentByType(FBuildEntityHandle Entity, const UScriptStruct& FragmentType);
	FConstStructView FindFragmentView(FBuildEntityHandle Entity, const UScriptStruct& FragmentType) const;
	FStructView FindMutableFragmentView(FBuildEntityHandle Entity, const UScriptStruct& FragmentType);
	int32 GetFragmentCountByType(const UScriptStruct& FragmentType) const;
	void GetEntitiesWithFragmentByType(
		const UScriptStruct& FragmentType,
		TArray<FBuildEntityHandle>& OutEntities) const;
	void GetFragmentPoolViewByType(
		const UScriptStruct& FragmentType,
		TConstArrayView<FBuildEntityHandle>& OutEntities,
		const uint8*& OutData,
		int32& OutStride) const;
	void GetMutableFragmentPoolViewByType(
		const UScriptStruct& FragmentType,
		TConstArrayView<FBuildEntityHandle>& OutEntities,
		uint8*& OutData,
		int32& OutStride);

	TUniquePtr<FBuildEntityRegistryData> Data;

	friend FBuildProcessorScheduler;
};
