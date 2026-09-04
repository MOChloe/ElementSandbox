#include "Rendering/BuildRenderDirtySet.h"

bool FBuildRenderDirtySet::MarkPartsDirty(
	const FBuildEntityHandle Entity,
	const TConstArrayView<int32> PartIds)
{
	check(IsInGameThread());
	if (!Entity.IsSet() || PartIds.IsEmpty())
	{
		return false;
	}

	for (const int32 PartId : PartIds)
	{
		if (PartId < 0)
		{
			return false;
		}
	}

	FBuildRenderDirtyEntry* Entry = FindOrAddEntry(Entity);
	check(Entry);
	if (Entry->Mode != EBuildRenderDirtyMode::PartSet)
	{
		return true;
	}

	for (const int32 PartId : PartIds)
	{
		Entry->PartIds.AddUnique(PartId);
	}
	return true;
}

bool FBuildRenderDirtySet::MarkAllPartsDirty(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	FBuildRenderDirtyEntry* Entry = FindOrAddEntry(Entity);
	if (!Entry)
	{
		return false;
	}

	if (Entry->Mode != EBuildRenderDirtyMode::Rebuild)
	{
		Entry->Mode = EBuildRenderDirtyMode::AllParts;
		Entry->PartIds.Reset();
	}
	return true;
}

bool FBuildRenderDirtySet::MarkRebuild(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	FBuildRenderDirtyEntry* Entry = FindOrAddEntry(Entity);
	if (!Entry)
	{
		return false;
	}

	Entry->Mode = EBuildRenderDirtyMode::Rebuild;
	Entry->PartIds.Reset();
	return true;
}

bool FBuildRenderDirtySet::MarkRebuild(
	const FBuildEntityHandle Entity,
	const bool bPackedStatic)
{
	if (!MarkRebuild(Entity))
	{
		return false;
	}
	FindOrAddEntry(Entity)->bPackedStatic = bPackedStatic;
	return true;
}

void FBuildRenderDirtySet::RequestClearAll()
{
	check(IsInGameThread());
	bClearAllRequested = true;
	Entries.Reset();
	EntryIndexByEntity.Reset();
}

bool FBuildRenderDirtySet::IsClearAllRequested() const
{
	check(IsInGameThread());
	return bClearAllRequested;
}

TConstArrayView<FBuildRenderDirtyEntry> FBuildRenderDirtySet::GetEntries() const
{
	check(IsInGameThread());
	return Entries;
}

int32 FBuildRenderDirtySet::Num() const
{
	check(IsInGameThread());
	return Entries.Num();
}

bool FBuildRenderDirtySet::IsEmpty() const
{
	check(IsInGameThread());
	return !bClearAllRequested && Entries.IsEmpty();
}

SIZE_T FBuildRenderDirtySet::GetEstimatedAllocatedSize() const
{
	check(IsInGameThread());
	SIZE_T AllocatedSize = Entries.GetAllocatedSize()
		+ EntryIndexByEntity.GetAllocatedSize();
	for (const FBuildRenderDirtyEntry& Entry : Entries)
	{
		AllocatedSize += Entry.PartIds.GetAllocatedSize();
	}
	return AllocatedSize;
}

void FBuildRenderDirtySet::Clear()
{
	check(IsInGameThread());
	bClearAllRequested = false;
	Entries.Reset();
	EntryIndexByEntity.Reset();
}

FBuildRenderDirtyEntry* FBuildRenderDirtySet::FindOrAddEntry(
	const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!Entity.IsSet())
	{
		return nullptr;
	}

	if (const int32* ExistingIndex = EntryIndexByEntity.Find(Entity))
	{
		return &Entries[*ExistingIndex];
	}

	FBuildRenderDirtyEntry NewEntry;
	NewEntry.Entity = Entity;
	const int32 NewIndex = Entries.Add(MoveTemp(NewEntry));
	EntryIndexByEntity.Add(Entity, NewIndex);
	return &Entries[NewIndex];
}
