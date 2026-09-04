#include "Entity/WorldObjectEntityRegistry.h"

#include "HAL/ThreadSafeCounter.h"

namespace
{
	FThreadSafeCounter GNextWorldObjectRegistryId;

	uint32 AllocateWorldObjectRegistryId()
	{
		uint32 RegistryId = 0;
		do
		{
			RegistryId = static_cast<uint32>(GNextWorldObjectRegistryId.Increment());
		}
		while (RegistryId == 0);
		return RegistryId;
	}
}

FWorldObjectEntityRegistry::FWorldObjectEntityRegistry()
	: RegistryId(AllocateWorldObjectRegistryId())
{
}

FWorldObjectEntityRegistry::~FWorldObjectEntityRegistry() = default;

FWorldObjectEntityHandle FWorldObjectEntityRegistry::CreateEntity()
{
	int32 Slot = INDEX_NONE;
	if (FirstFreeSlot != INDEX_NONE)
	{
		Slot = FirstFreeSlot;
		FEntityRecord& ReusedRecord = Records[Slot];
		FirstFreeSlot = ReusedRecord.NextFreeSlot;
		ReusedRecord.NextFreeSlot = INDEX_NONE;
		ReusedRecord.bAlive = true;
	}
	else
	{
		Slot = Records.AddDefaulted();
		Records[Slot].bAlive = true;
	}

	++EntityCount;
	return FWorldObjectEntityHandle(RegistryId, Slot, Records[Slot].Generation);
}

void FWorldObjectEntityRegistry::ReserveEntities(const int32 AdditionalEntityCount)
{
	if (AdditionalEntityCount > 0)
	{
		const int32 RequiredCapacity = Records.Num() + AdditionalEntityCount;
		if (RequiredCapacity > Records.Max())
		{
			const int64 GeometricCapacity = FMath::Max<int64>(
				RequiredCapacity,
				FMath::Max<int64>(4096, static_cast<int64>(Records.Max()) * 2));
			Records.Reserve(static_cast<int32>(FMath::Min<int64>(GeometricCapacity, MAX_int32)));
		}
		if (PoolIdByType.IsEmpty())
		{
			PoolIdByType.Reserve(8);
		}
	}
}

bool FWorldObjectEntityRegistry::DestroyEntity(const FWorldObjectEntityHandle Entity)
{
	if (!IsAlive(Entity))
	{
		return false;
	}

	FEntityRecord& Record = Records[Entity.GetSlot()];
	for (const FWorldObjectFragmentPoolId PoolId : Record.OwnedFragmentPoolIds)
	{
		check(PoolsById.IsValidIndex(PoolId) && PoolsById[PoolId]);
		verify(PoolsById[PoolId]->Remove(Entity));
	}
	Record.OwnedFragmentPoolIds.Reset();
	Record.bAlive = false;
	Record.Generation = Record.Generation == MAX_uint32 ? 1 : Record.Generation + 1;
	Record.NextFreeSlot = FirstFreeSlot;
	FirstFreeSlot = Entity.GetSlot();
	--EntityCount;
	return true;
}

bool FWorldObjectEntityRegistry::IsAlive(const FWorldObjectEntityHandle Entity) const
{
	return Entity.GetRegistryId() == RegistryId
		&& Records.IsValidIndex(Entity.GetSlot())
		&& Records[Entity.GetSlot()].bAlive
		&& Records[Entity.GetSlot()].Generation == Entity.GetGeneration();
}

FWorldObjectEntityRegistryStorageStats FWorldObjectEntityRegistry::GetStorageStats() const
{
	FWorldObjectEntityRegistryStorageStats Stats;
	Stats.AllocatedBytes = Records.GetAllocatedSize()
		+ PoolsById.GetAllocatedSize()
		+ PoolIdByType.GetAllocatedSize();
	for (const FEntityRecord& Record : Records)
	{
		Stats.AllocatedBytes += Record.OwnedFragmentPoolIds.GetAllocatedSize();
	}
	for (const TUniquePtr<IWorldObjectFragmentPool>& Pool : PoolsById)
	{
		if (!Pool)
		{
			continue;
		}
		++Stats.FragmentPoolCount;
		Stats.SparseIndexPageCount += Pool->GetSparseIndexPageCount();
		Stats.DenseFragmentCapacity += Pool->GetDenseCapacity();
		Stats.AllocatedBytes += Pool->GetAllocatedSize();
	}
	return Stats;
}

void FWorldObjectEntityRegistry::Reset()
{
	PoolsById.Reset();
	PoolIdByType.Reset();
	Records.Reset();
	FirstFreeSlot = INDEX_NONE;
	EntityCount = 0;
	RegistryId = AllocateWorldObjectRegistryId();
}
