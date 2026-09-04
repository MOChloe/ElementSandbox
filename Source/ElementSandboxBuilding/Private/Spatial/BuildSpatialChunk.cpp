#include "Spatial/BuildSpatialChunk.h"

#include "Spatial/BuildSpatialEntry.h"
#include "Spatial/BuildSpatialIndex.h"

#include "HAL/PlatformTime.h"

FBuildSpatialChunk::FBuildSpatialChunk(const double DynamicBoundsPadding)
	: StaticDeltaTree(DynamicBoundsPadding)
	, PersistentDynamicTree(DynamicBoundsPadding)
{
}

bool FBuildSpatialChunk::Insert(
	const FBuildEntityHandle Entity,
	const FBox& Bounds,
	const EBuildSpatialMobility Mobility)
{
	if (StaticBounds.Contains(Entity) || DynamicBounds.Contains(Entity))
	{
		return false;
	}

	switch (Mobility)
	{
	case EBuildSpatialMobility::Static:
		return InsertStatic(Entity, Bounds);
	case EBuildSpatialMobility::Dynamic:
		return InsertDynamic(Entity, Bounds);
	default:
		return false;
	}
}

bool FBuildSpatialChunk::Update(
	const FBuildEntityHandle Entity,
	const FBox& Bounds,
	const EBuildSpatialMobility Mobility)
{
	switch (Mobility)
	{
	case EBuildSpatialMobility::Static:
		return UpdateStatic(Entity, Bounds);
	case EBuildSpatialMobility::Dynamic:
		return UpdateDynamic(Entity, Bounds);
	default:
		return false;
	}
}

bool FBuildSpatialChunk::SetMobility(
	const FBuildEntityHandle Entity,
	const EBuildSpatialMobility Mobility)
{
	switch (Mobility)
	{
	case EBuildSpatialMobility::Static:
	{
		const FBox* ExistingBounds = DynamicBounds.Find(Entity);
		if (!ExistingBounds)
		{
			return StaticBounds.Contains(Entity);
		}

		const FBox Bounds = *ExistingBounds;
		if (StaticBounds.Contains(Entity) || !StaticDeltaTree.Insert(Entity, Bounds))
		{
			return false;
		}
		StaticBounds.Add(Entity, Bounds);
		verify(RemovePersistentDynamic(Entity));
		MarkStaticWrite();
		return true;
	}

	case EBuildSpatialMobility::Dynamic:
	{
		const FBox* ExistingBounds = StaticBounds.Find(Entity);
		if (!ExistingBounds)
		{
			return DynamicBounds.Contains(Entity);
		}

		const FBox Bounds = *ExistingBounds;
		if (DynamicBounds.Contains(Entity) || !PersistentDynamicTree.Insert(Entity, Bounds))
		{
			return false;
		}
		DynamicBounds.Add(Entity, Bounds);
		verify(RemoveStatic(Entity));
		return true;
	}

	default:
		return false;
	}
}

bool FBuildSpatialChunk::Remove(
	const FBuildEntityHandle Entity,
	const EBuildSpatialMobility Mobility)
{
	switch (Mobility)
	{
	case EBuildSpatialMobility::Static:
		return RemoveStatic(Entity);
	case EBuildSpatialMobility::Dynamic:
		return RemovePersistentDynamic(Entity);
	default:
		return false;
	}
}

void FBuildSpatialChunk::Query(
	const FBox& QueryBounds,
	TArray<FBuildEntityHandle>& OutCandidates) const
{
	const int32 SnapshotResultStart = OutCandidates.Num();
	StaticSnapshot.Query(QueryBounds, OutCandidates);
	for (int32 Index = OutCandidates.Num() - 1; Index >= SnapshotResultStart; --Index)
	{
		if (StaticSnapshotTombstones.Contains(OutCandidates[Index]))
		{
			OutCandidates.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
	}

	StaticDeltaTree.Query(QueryBounds, OutCandidates);
	PersistentDynamicTree.Query(QueryBounds, OutCandidates);
}

void FBuildSpatialChunk::QueryRay(
	const FVector& Origin,
	const FVector& UnitDirection,
	const double MaxDistance,
	TArray<FBuildEntityHandle>& OutCandidates) const
{
	const int32 SnapshotResultStart = OutCandidates.Num();
	StaticSnapshot.QueryRay(Origin, UnitDirection, MaxDistance, OutCandidates);
	for (int32 Index = OutCandidates.Num() - 1; Index >= SnapshotResultStart; --Index)
	{
		if (StaticSnapshotTombstones.Contains(OutCandidates[Index]))
		{
			OutCandidates.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
	}

	StaticDeltaTree.QueryRay(Origin, UnitDirection, MaxDistance, OutCandidates);
	PersistentDynamicTree.QueryRay(Origin, UnitDirection, MaxDistance, OutCandidates);
}

void FBuildSpatialChunk::RebuildStaticSnapshot()
{
	TArray<FBuildSpatialEntry> Entries;
	CaptureStaticEntries(Entries);

	FBuildAABBTree NewSnapshot;
	NewSnapshot.Build(Entries);
	StaticSnapshot = MoveTemp(NewSnapshot);
	StaticDeltaTree.Reset();
	StaticSnapshotTombstones.Reset();
}

void FBuildSpatialChunk::CaptureStaticEntries(
	TArray<FBuildSpatialEntry>& OutEntries) const
{
	OutEntries.Reset(StaticBounds.Num());
	for (const TPair<FBuildEntityHandle, FBox>& Pair : StaticBounds)
	{
		OutEntries.Add({Pair.Key, Pair.Value});
	}
}

bool FBuildSpatialChunk::PublishStaticSnapshot(
	const uint64 Version,
	FBuildAABBTree&& NewSnapshot)
{
	if (Version != StaticVersion || NewSnapshot.Num() != StaticBounds.Num())
	{
		return false;
	}

	StaticSnapshot = MoveTemp(NewSnapshot);
	StaticDeltaTree.Reset();
	StaticSnapshotTombstones.Reset();
	return true;
}

bool FBuildSpatialChunk::InsertStatic(
	const FBuildEntityHandle Entity,
	const FBox& Bounds)
{
	if (StaticBounds.Contains(Entity)
		|| DynamicBounds.Contains(Entity)
		|| !StaticDeltaTree.Insert(Entity, Bounds))
	{
		return false;
	}

	StaticBounds.Add(Entity, Bounds);
	MarkStaticWrite();
	return true;
}

bool FBuildSpatialChunk::InsertDynamic(
	const FBuildEntityHandle Entity,
	const FBox& Bounds)
{
	if (StaticBounds.Contains(Entity)
		|| DynamicBounds.Contains(Entity)
		|| !PersistentDynamicTree.Insert(Entity, Bounds))
	{
		return false;
	}

	DynamicBounds.Add(Entity, Bounds);
	return true;
}

bool FBuildSpatialChunk::UpdateStatic(
	const FBuildEntityHandle Entity,
	const FBox& Bounds)
{
	FBox* ExistingBounds = StaticBounds.Find(Entity);
	if (!ExistingBounds)
	{
		return false;
	}

	if (*ExistingBounds == Bounds)
	{
		return true;
	}

	if (StaticDeltaTree.Contains(Entity))
	{
		if (!StaticDeltaTree.Update(Entity, Bounds))
		{
			return false;
		}
	}
	else
	{
		StaticSnapshotTombstones.Add(Entity);
		if (!StaticDeltaTree.Insert(Entity, Bounds))
		{
			StaticSnapshotTombstones.Remove(Entity);
			return false;
		}
	}

	*ExistingBounds = Bounds;
	MarkStaticWrite();
	return true;
}

bool FBuildSpatialChunk::UpdateDynamic(
	const FBuildEntityHandle Entity,
	const FBox& Bounds)
{
	FBox* ExistingBounds = DynamicBounds.Find(Entity);
	if (!ExistingBounds || !PersistentDynamicTree.Update(Entity, Bounds))
	{
		return false;
	}

	*ExistingBounds = Bounds;
	return true;
}

bool FBuildSpatialChunk::RemoveStatic(const FBuildEntityHandle Entity)
{
	if (StaticBounds.Remove(Entity) == 0)
	{
		return false;
	}

	if (!StaticDeltaTree.Remove(Entity))
	{
		// 不可变 Snapshot 不原地删除；Query 用 Tombstone 屏蔽旧叶。
		StaticSnapshotTombstones.Add(Entity);
	}
	MarkStaticWrite();
	return true;
}

bool FBuildSpatialChunk::RemovePersistentDynamic(const FBuildEntityHandle Entity)
{
	if (!DynamicBounds.Contains(Entity) || !PersistentDynamicTree.Remove(Entity))
	{
		return false;
	}

	DynamicBounds.Remove(Entity);
	return true;
}

void FBuildSpatialChunk::MarkStaticWrite()
{
	++StaticVersion;
	if (StaticVersion == 0)
	{
		++StaticVersion;
	}
	LastStaticWriteTimeSeconds = FPlatformTime::Seconds();
}
