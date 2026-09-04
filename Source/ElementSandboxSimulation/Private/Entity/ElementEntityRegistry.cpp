#include "Entity/ElementEntityRegistry.h"

#include "HAL/PlatformAtomics.h"

namespace
{
	TAtomic<uint32> NextElementRegistryId{1};

	uint32 AllocateRegistryId()
	{
		uint32 Id = NextElementRegistryId++;
		if (Id == 0) Id = NextElementRegistryId++;
		return Id;
	}

	uint32 AdvanceGeneration(const uint32 Generation)
	{
		return Generation == MAX_uint32 ? 1 : Generation + 1;
	}

	uint64 AdvanceNonZero(const uint64 Revision)
	{
		return Revision == MAX_uint64 ? 1 : Revision + 1;
	}
}

FElementEntityRegistry::FElementEntityRegistry()
	: RegistryId(AllocateRegistryId())
{
}

FElementEntityRegistry::~FElementEntityRegistry() = default;

FElementEntityHandle FElementEntityRegistry::CreateEntity(const FWorldEntityId PersistentId)
{
	check(IsInGameThread());
	if (PersistentId.IsSet() && EntityByPersistentId.Contains(PersistentId)) return {};
	int32 Slot = INDEX_NONE;
	if (!FreeSlots.IsEmpty())
	{
		Slot = FreeSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		Slot = Records.AddDefaulted();
	}
	FEntityRecord& Record = Records[Slot];
	check(!Record.bAlive);
	Record.bAlive = true;
	Record.Revision = 1;
	Record.PersistentId = PersistentId;
	const FElementEntityHandle Handle(RegistryId, Slot, Record.Generation);
	if (PersistentId.IsSet()) EntityByPersistentId.Add(PersistentId, Handle);
	MarkDirty(Handle);
	return Handle;
}

bool FElementEntityRegistry::DestroyEntity(const FElementEntityHandle Entity)
{
	check(IsInGameThread());
	if (!IsAlive(Entity)) return false;
	for (TPair<const void*, TUniquePtr<UE::ElementSandbox::ElementRegistry::Private::IFragmentPool>>& Pair : Pools)
	{
		Pair.Value->RemoveSlot(Entity.GetSlot());
	}
	NextDirty.Remove(Entity);
	FEntityRecord& Record = Records[Entity.GetSlot()];
	if (Record.PersistentId.IsSet()) EntityByPersistentId.Remove(Record.PersistentId);
	Record.bAlive = false;
	Record.Revision = 0;
	Record.PersistentId = {};
	Record.Generation = AdvanceGeneration(Record.Generation);
	FreeSlots.Add(Entity.GetSlot());
	return true;
}

bool FElementEntityRegistry::IsAlive(const FElementEntityHandle Entity) const
{
	return Entity.GetRegistryId() == RegistryId
		&& Records.IsValidIndex(Entity.GetSlot())
		&& Records[Entity.GetSlot()].bAlive
		&& Records[Entity.GetSlot()].Generation == Entity.GetGeneration();
}

FWorldEntityId FElementEntityRegistry::GetPersistentId(const FElementEntityHandle Entity) const
{
	return IsAlive(Entity) ? Records[Entity.GetSlot()].PersistentId : FWorldEntityId();
}

FElementEntityHandle FElementEntityRegistry::FindByPersistentId(const FWorldEntityId PersistentId) const
{
	const FElementEntityHandle* Entity = PersistentId.IsSet() ? EntityByPersistentId.Find(PersistentId) : nullptr;
	return Entity && IsAlive(*Entity) ? *Entity : FElementEntityHandle();
}

uint64 FElementEntityRegistry::GetEntityRevision(const FElementEntityHandle Entity) const
{
	return IsAlive(Entity) ? Records[Entity.GetSlot()].Revision : 0;
}

void FElementEntityRegistry::AdvanceRevision(const FElementEntityHandle Entity)
{
	check(IsAlive(Entity));
	Records[Entity.GetSlot()].Revision = AdvanceNonZero(Records[Entity.GetSlot()].Revision);
}

void FElementEntityRegistry::MarkDirty(const FElementEntityHandle Entity)
{
	bool bAlreadyInSet = false;
	NextDirty.Add(Entity, &bAlreadyInSet);
	if (bAlreadyInSet)
	{
		++DirtyDeduplicatedCount;
		return;
	}
	++DirtyEnqueueCount;
}

bool FElementEntityRegistry::SealDirtyPage(FElementDirtyPage& OutPage)
{
	check(IsInGameThread());
	OutPage = {};
	if (NextDirty.IsEmpty()) return false;
	OutPage.Epoch = NextDirtyEpoch;
	NextDirtyEpoch = NextDirtyEpoch == MAX_uint64 ? 1 : NextDirtyEpoch + 1;
	OutPage.Entities = NextDirty.Array();
	OutPage.Entities.Sort();
	NextDirty.Reset();
	return true;
}

FElementEntityRegistryStats FElementEntityRegistry::GetStats() const
{
	FElementEntityRegistryStats Stats;
	Stats.PendingDirtyCount = NextDirty.Num();
	Stats.DirtyEnqueueCount = DirtyEnqueueCount;
	Stats.DirtyDeduplicatedCount = DirtyDeduplicatedCount;
	for (const FEntityRecord& Record : Records) Stats.EntityCount += Record.bAlive ? 1 : 0;
	return Stats;
}

SIZE_T FElementEntityRegistry::GetAllocatedSize() const
{
	SIZE_T Size = Records.GetAllocatedSize() + FreeSlots.GetAllocatedSize() + EntityByPersistentId.GetAllocatedSize()
		+ Pools.GetAllocatedSize() + NextDirty.GetAllocatedSize();
	for (const TPair<const void*, TUniquePtr<UE::ElementSandbox::ElementRegistry::Private::IFragmentPool>>& Pair : Pools)
	{
		Size += Pair.Value->GetAllocatedSize();
	}
	return Size;
}
