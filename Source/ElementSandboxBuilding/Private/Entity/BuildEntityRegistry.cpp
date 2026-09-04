#include "Entity/BuildEntityRegistry.h"

#include "Storage/BuildFragmentStore.h"
#include "Storage/BuildStableArrayAllocator.h"
#include "Templates/Atomic.h"
#include "UObject/Class.h"

namespace
{
uint32 GNextBuildEntityRegistryId = 0;

uint32 AllocateBuildEntityRegistryId()
{
	check(IsInGameThread());
	++GNextBuildEntityRegistryId;
	if (GNextBuildEntityRegistryId == 0)
	{
		++GNextBuildEntityRegistryId;
	}
	return GNextBuildEntityRegistryId;
}

void AdvanceGeneration(uint32& Generation)
{
	++Generation;
	if (Generation == 0)
	{
		++Generation;
	}
}
}

class FBuildEntityRegistryData final
{
public:
	struct FEntitySlot
	{
		uint32 Generation = 1;
		int32 NextFreeIndex = INDEX_NONE;
		bool bAlive = false;
		/**
		 * 销毁时只访问 Entity 实际拥有的 Pool。七项内联覆盖城市普通部件的五项
		 * 与 Settlement Door 的七项，避免千万级基线为每个 Slot 单独分配小数组。
		 */
		TArray<FBuildFragmentPoolId, TInlineAllocator<7>> OwnedFragmentPoolIds;
	};

	FBuildEntityRegistryData()
		: RegistryId(AllocateBuildEntityRegistryId())
	{
	}

	uint32 RegistryId = 0;
	TArray<FEntitySlot, FBuildStableArrayAllocator> Slots;
	int32 FirstFreeIndex = INDEX_NONE;
	int32 AliveEntityCount = 0;
	FBuildFragmentStore FragmentStore;
	TAtomic<bool> bFrozenParallelReadActive { false };
};

FBuildEntityRegistry::FBuildEntityRegistry()
	: Data(MakeUnique<FBuildEntityRegistryData>())
{
	check(IsInGameThread());
}

FBuildEntityRegistry::~FBuildEntityRegistry() = default;

FBuildEntityHandle FBuildEntityRegistry::CreateEntity()
{
	check(IsInGameThread());
	check(Data);

	int32 EntityIndex = INDEX_NONE;
	if (Data->FirstFreeIndex != INDEX_NONE)
	{
		EntityIndex = Data->FirstFreeIndex;
		FBuildEntityRegistryData::FEntitySlot& Slot = Data->Slots[EntityIndex];
		Data->FirstFreeIndex = Slot.NextFreeIndex;
		Slot.NextFreeIndex = INDEX_NONE;
	}
	else
	{
		EntityIndex = Data->Slots.AddDefaulted();
	}

	FBuildEntityRegistryData::FEntitySlot& Slot = Data->Slots[EntityIndex];
	check(!Slot.bAlive);
	check(Slot.OwnedFragmentPoolIds.IsEmpty());
	check(Slot.Generation != 0);
	Slot.bAlive = true;
	++Data->AliveEntityCount;

	return FBuildEntityHandle(Data->RegistryId, EntityIndex, Slot.Generation);
}

void FBuildEntityRegistry::ReserveEntityCapacity(const int32 EntityCapacity)
{
	check(IsInGameThread());
	if (Data && EntityCapacity > 0)
	{
		Data->Slots.Reserve(EntityCapacity);
	}
}

bool FBuildEntityRegistry::ReserveFragmentCapacityByType(
	const UScriptStruct& FragmentType, const int32 FragmentCapacity)
{
	check(IsInGameThread());
	return Data && Data->FragmentStore.Reserve(FragmentType, FragmentCapacity);
}

bool FBuildEntityRegistry::DestroyEntity(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!IsAlive(Entity))
	{
		return false;
	}

	FBuildEntityRegistryData::FEntitySlot& Slot = Data->Slots[Entity.Index];
	Data->FragmentStore.RemoveEntity(Entity, Slot.OwnedFragmentPoolIds);
	Slot.OwnedFragmentPoolIds.Empty();
	Slot.bAlive = false;
	AdvanceGeneration(Slot.Generation);
	Slot.NextFreeIndex = Data->FirstFreeIndex;
	Data->FirstFreeIndex = Entity.Index;
	--Data->AliveEntityCount;

	check(Data->AliveEntityCount >= 0);
	return true;
}

bool FBuildEntityRegistry::IsAlive(const FBuildEntityHandle Entity) const
{
	check(IsInGameThread() || IsFrozenParallelReadActive());
	if (!Data || !Entity.IsSet() || Entity.RegistryId != Data->RegistryId || !Data->Slots.IsValidIndex(Entity.Index))
	{
		return false;
	}

	const FBuildEntityRegistryData::FEntitySlot& Slot = Data->Slots[Entity.Index];
	return Slot.bAlive && Slot.Generation == Entity.Generation;
}

int32 FBuildEntityRegistry::GetEntityCount() const
{
	check(IsInGameThread());
	return Data ? Data->AliveEntityCount : 0;
}

SIZE_T FBuildEntityRegistry::GetEstimatedAllocatedSize() const
{
	check(IsInGameThread());
	if (!Data)
	{
		return 0;
	}

	SIZE_T AllocatedSize = Data->Slots.GetAllocatedSize() + Data->FragmentStore.GetAllocatedSize();
	for (const FBuildEntityRegistryData::FEntitySlot& Slot : Data->Slots)
	{
		AllocatedSize += Slot.OwnedFragmentPoolIds.GetAllocatedSize();
	}
	return AllocatedSize;
}

bool FBuildEntityRegistry::AddFragmentView(const FBuildEntityHandle Entity, const FConstStructView Fragment)
{
	check(IsInGameThread());
	if (!IsAlive(Entity))
	{
		return false;
	}

	FBuildFragmentPoolId PoolId = InvalidBuildFragmentPoolId;
	if (!Data->FragmentStore.Add(Entity, Fragment, PoolId))
	{
		return false;
	}
	check(PoolId != InvalidBuildFragmentPoolId);
	Data->Slots[Entity.Index].OwnedFragmentPoolIds.Add(PoolId);
	return true;
}

bool FBuildEntityRegistry::RemoveFragmentByType(const FBuildEntityHandle Entity, const UScriptStruct& FragmentType)
{
	check(IsInGameThread());
	if (!IsAlive(Entity))
	{
		return false;
	}

	FBuildFragmentPoolId PoolId = InvalidBuildFragmentPoolId;
	if (!Data->FragmentStore.Remove(Entity, FragmentType, PoolId))
	{
		return false;
	}
	const int32 RemovedCount
		= Data->Slots[Entity.Index].OwnedFragmentPoolIds.RemoveSingleSwap(PoolId, EAllowShrinking::No);
	check(RemovedCount == 1);
	return true;
}

FConstStructView FBuildEntityRegistry::FindFragmentView(
	const FBuildEntityHandle Entity, const UScriptStruct& FragmentType) const
{
	check(IsInGameThread() || IsFrozenParallelReadActive());
	return IsAlive(Entity) ? Data->FragmentStore.Find(Entity, FragmentType) : FConstStructView();
}

FStructView FBuildEntityRegistry::FindMutableFragmentView(
	const FBuildEntityHandle Entity, const UScriptStruct& FragmentType)
{
	check(IsInGameThread());
	return IsAlive(Entity) ? Data->FragmentStore.FindMutable(Entity, FragmentType) : FStructView();
}

int32 FBuildEntityRegistry::GetFragmentCountByType(const UScriptStruct& FragmentType) const
{
	check(IsInGameThread());
	return Data ? Data->FragmentStore.GetFragmentCount(FragmentType) : 0;
}

void FBuildEntityRegistry::GetEntitiesWithFragmentByType(
	const UScriptStruct& FragmentType, TArray<FBuildEntityHandle>& OutEntities) const
{
	check(IsInGameThread());
	if (Data)
	{
		Data->FragmentStore.GetEntities(FragmentType, OutEntities);
	}
	else
	{
		OutEntities.Reset();
	}
}

void FBuildEntityRegistry::GetFragmentPoolViewByType(const UScriptStruct& FragmentType,
	TConstArrayView<FBuildEntityHandle>& OutEntities, const uint8*& OutData, int32& OutStride) const
{
	check(IsInGameThread() || IsFrozenParallelReadActive());
	OutEntities = {};
	OutData = nullptr;
	OutStride = 0;
	const FBuildFragmentPool* Pool = Data ? Data->FragmentStore.GetPool(FragmentType) : nullptr;
	if (Pool)
	{
		OutEntities = Pool->GetEntityView();
		OutData = Pool->GetData();
		OutStride = Pool->GetStride();
	}
}

void FBuildEntityRegistry::GetMutableFragmentPoolViewByType(const UScriptStruct& FragmentType,
	TConstArrayView<FBuildEntityHandle>& OutEntities, uint8*& OutData, int32& OutStride)
{
	check(IsInGameThread());
	OutEntities = {};
	OutData = nullptr;
	OutStride = 0;
	FBuildFragmentPool* Pool = Data ? Data->FragmentStore.GetMutablePool(FragmentType) : nullptr;
	if (Pool)
	{
		OutEntities = Pool->GetEntityView();
		OutData = Pool->GetMutableData();
		OutStride = Pool->GetStride();
	}
}

void FBuildEntityRegistry::Reset()
{
	check(IsInGameThread());
	check(Data);

	Data->FragmentStore.Reset();
	Data->AliveEntityCount = 0;
	Data->FirstFreeIndex = INDEX_NONE;

	for (int32 Index = Data->Slots.Num() - 1; Index >= 0; --Index)
	{
		FBuildEntityRegistryData::FEntitySlot& Slot = Data->Slots[Index];
		Slot.OwnedFragmentPoolIds.Empty();
		if (Slot.bAlive)
		{
			AdvanceGeneration(Slot.Generation);
		}

		Slot.bAlive = false;
		Slot.NextFreeIndex = Data->FirstFreeIndex;
		Data->FirstFreeIndex = Index;
	}
}

void FBuildEntityRegistry::BeginFrozenParallelRead() const
{
	check(IsInGameThread());
	check(Data && !Data->bFrozenParallelReadActive.Load());
	Data->bFrozenParallelReadActive.Store(true);
}

void FBuildEntityRegistry::EndFrozenParallelRead() const
{
	check(IsInGameThread());
	check(Data && Data->bFrozenParallelReadActive.Load());
	Data->bFrozenParallelReadActive.Store(false);
}

bool FBuildEntityRegistry::IsFrozenParallelReadActive() const { return Data && Data->bFrozenParallelReadActive.Load(); }
