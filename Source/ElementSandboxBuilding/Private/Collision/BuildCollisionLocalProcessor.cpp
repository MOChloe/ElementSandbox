#include "Collision/BuildCollisionProcessor.h"

#include "Collision/BuildCollisionHost.h"
#include "Definition/BuildingDefinition.h"
#include "ElementSandboxBuilding.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

namespace
{
	uint32 GNextBuildCollisionRuntimeId = 0;

	uint32 AllocateBuildCollisionRuntimeId()
	{
		check(IsInGameThread());
		++GNextBuildCollisionRuntimeId;
		if (GNextBuildCollisionRuntimeId == 0)
		{
			++GNextBuildCollisionRuntimeId;
		}
		return GNextBuildCollisionRuntimeId;
	}

	void AdvanceCollisionSourceGeneration(uint32& Generation)
	{
		++Generation;
		if (Generation == 0)
		{
			++Generation;
		}
	}

	bool AreBoundsEquivalent(const FBox& Left, const FBox& Right)
	{
		return Left.IsValid == Right.IsValid
			&& (!Left.IsValid
				|| (Left.Min.Equals(Right.Min, 0.01)
					&& Left.Max.Equals(Right.Max, 0.01)));
	}

	bool AreSourcesEquivalent(
		const FBuildCollisionSource& Left,
		const FBuildCollisionSource& Right)
	{
		return Left.Revision == Right.Revision
			&& Left.SubjectLocation.Equals(Right.SubjectLocation, 0.01)
			&& Left.Velocity.Equals(Right.Velocity, 0.01)
			&& AreBoundsEquivalent(Left.ImmediateBounds, Right.ImmediateBounds)
			&& AreBoundsEquivalent(Left.PrefetchBounds, Right.PrefetchBounds)
			&& AreBoundsEquivalent(Left.CameraBounds, Right.CameraBounds)
			&& AreBoundsEquivalent(Left.RetentionBounds, Right.RetentionBounds);
	}

	double SquaredDistanceFromBoundsToPoint(const FBox& Bounds, const FVector& Point)
	{
		const FVector Closest(
			FMath::Clamp(Point.X, Bounds.Min.X, Bounds.Max.X),
			FMath::Clamp(Point.Y, Bounds.Min.Y, Bounds.Max.Y),
			FMath::Clamp(Point.Z, Bounds.Min.Z, Bounds.Max.Z));
		return FVector::DistSquared(Closest, Point);
	}

	struct FBuildCollisionPartKey final
	{
		FBuildEntityHandle Entity;
		int32 CollisionPartId = INDEX_NONE;

		friend bool operator==(
			const FBuildCollisionPartKey& Left,
			const FBuildCollisionPartKey& Right)
		{
			return Left.Entity == Right.Entity
				&& Left.CollisionPartId == Right.CollisionPartId;
		}

		friend uint32 GetTypeHash(const FBuildCollisionPartKey& Key)
		{
			return HashCombineFast(GetTypeHash(Key.Entity), GetTypeHash(Key.CollisionPartId));
		}
	};

	bool IsPartKeyLess(
		const FBuildCollisionPartKey& Left,
		const FBuildCollisionPartKey& Right)
	{
		if (Left.Entity.GetIndex() != Right.Entity.GetIndex())
		{
			return Left.Entity.GetIndex() < Right.Entity.GetIndex();
		}
		if (Left.Entity.GetGeneration() != Right.Entity.GetGeneration())
		{
			return Left.Entity.GetGeneration() < Right.Entity.GetGeneration();
		}
		return Left.CollisionPartId < Right.CollisionPartId;
	}

	struct FBuildProjectedCollisionPart final
	{
		int32 CollisionPartId = INDEX_NONE;
		FBuildCollisionInstanceHandle Instance;
		FBuildCollisionClusterKey ClusterKey;
		FBox WorldBounds = FBox(ForceInit);
		double LastRequiredTimeSeconds = 0.0;
		double RetentionExitTimeSeconds = -1.0;
	};

	struct FBuildProjectedCollisionEntity final
	{
		FBuildEntityHandle Entity;
		TArray<FBuildProjectedCollisionPart, TInlineAllocator<4>> Parts;
	};

	struct FBuildCollisionSourceSlot final
	{
		FBuildCollisionSource Source;
		uint32 Generation = 1;
		int32 NextFreeIndex = INDEX_NONE;
		bool bAlive = false;
	};

	enum class EBuildCollisionDirtyMode : uint8
	{
		PartSet,
		Rebuild,
		Destroy
	};

	struct FBuildCollisionDirtyEntry final
	{
		FBuildEntityHandle Entity;
		EBuildCollisionDirtyMode Mode = EBuildCollisionDirtyMode::PartSet;
		TArray<int32, TInlineAllocator<8>> MeshPartIds;
	};

	struct FBuildCollisionSelectionEntry final
	{
		FBuildCollisionPartKey PartKey;
		FBox WorldBounds = FBox(ForceInit);
		double ExpectedContactTimeSeconds = TNumericLimits<double>::Max();
		double SquaredDistance = TNumericLimits<double>::Max();
		bool bRequired = false;
		bool bUrgent = false;
		bool bRetained = false;
	};

	struct FBuildCollisionPrefetchCandidate final
	{
		FBuildCollisionPartKey PartKey;
		double ExpectedContactTimeSeconds = TNumericLimits<double>::Max();
		double SquaredDistance = TNumericLimits<double>::Max();
	};

	struct FBuildPendingCollisionAdd final
	{
		FBuildCollisionPartKey PartKey;
		FBuildCollisionClusterKey ClusterKey;
		FTransform WorldTransform = FTransform::Identity;
		FBox WorldBounds = FBox(ForceInit);
		double LastRequiredTimeSeconds = 0.0;
	};

	struct FBuildCollisionAddBatch final
	{
		TArray<FBuildCollisionPartKey> PartKeys;
		TArray<FTransform> WorldTransforms;
	};

	struct FBuildCollisionUpdateBatch final
	{
		TArray<FBuildCollisionInstanceUpdate> Updates;
		TArray<FBuildCollisionPartKey> PartKeys;
	};

	struct FBuildCollisionMutationBatch final
	{
		TMap<FBuildCollisionPartKey, FBuildPendingCollisionAdd> PendingAdds;
		TArray<FBuildCollisionInstanceHandle> Removals;
		TArray<FBuildCollisionPartKey> RemovalPartKeys;
		TSet<FBuildCollisionInstanceHandle> RemovedInstances;
		TMap<FBuildCollisionClusterKey, FBuildCollisionUpdateBatch> Updates;
	};

	struct FBuildCollisionEvictionCandidate final
	{
		FBuildCollisionPartKey PartKey;
		double SquaredDistance = 0.0;
		double LastRequiredTimeSeconds = 0.0;
	};

	bool TryResolveCollisionDefinitionAndTransforms(
		const FBuildEntityRegistry& Registry,
		const FBuildEntityHandle Entity,
		const FBuildTransformFragment*& OutTransform,
		const UBuildingDefinition*& OutDefinition,
		const FBuildPartTransformFragment*& OutPartTransforms)
	{
		OutTransform = Registry.FindFragment<FBuildTransformFragment>(Entity);
		const FBuildDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(Entity);
		OutDefinition = DefinitionFragment
			? DefinitionFragment->Definition.Get()
			: nullptr;
		OutPartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Entity);
		return OutTransform
			&& OutDefinition
			&& (!OutPartTransforms
				|| OutPartTransforms->LocalTransforms.Num()
					== OutDefinition->MeshParts.Num());
	}

	TConstArrayView<FTransform> GetPartTransformView(
		const FBuildPartTransformFragment* PartTransforms)
	{
		return PartTransforms
			? TConstArrayView<FTransform>(PartTransforms->LocalTransforms)
			: TConstArrayView<FTransform>();
	}
}

class FBuildCollisionProcessorData final
{
public:
	explicit FBuildCollisionProcessorData(
		const FBuildCollisionActivationConfig& InActivationConfig);

	FBuildCollisionSourceSlot* FindSource(FBuildCollisionSourceHandle Handle);
	const FBuildCollisionSourceSlot* FindSource(FBuildCollisionSourceHandle Handle) const;
	FBuildProjectedCollisionEntity* FindEntity(FBuildEntityHandle Entity);
	const FBuildProjectedCollisionEntity* FindEntity(FBuildEntityHandle Entity) const;
	FBuildProjectedCollisionPart* FindPart(const FBuildCollisionPartKey& PartKey);
	const FBuildProjectedCollisionPart* FindPart(const FBuildCollisionPartKey& PartKey) const;
	bool IntersectsAnyRetentionBounds(const FBox& Bounds) const;
	bool IsEntityRelevant(
		FBuildEntityHandle Entity,
		const FBuildSpatialIndex& SpatialIndex) const;
	bool IsEntityTracked(FBuildEntityHandle Entity) const;
	void MarkRebuild(FBuildEntityHandle Entity);
	void MarkDestroy(FBuildEntityHandle Entity);
	void MarkParts(FBuildEntityHandle Entity, TConstArrayView<int32> MeshPartIds);
	void QueuePartRemoval(
		const FBuildCollisionPartKey& PartKey,
		FBuildCollisionMutationBatch& Mutation);
	void QueueEntityRemoval(
		FBuildEntityHandle Entity,
		FBuildCollisionMutationBatch& Mutation);
	bool QueuePartProjection(
		const FBuildEntityRegistry& Registry,
		const FBuildCollisionPartKey& PartKey,
		double CurrentTimeSeconds,
		FBuildCollisionMutationBatch& Mutation);
	bool QueuePartUpdates(
		const FBuildEntityRegistry& Registry,
		const FBuildCollisionDirtyEntry& Dirty,
		FBuildCollisionMutationBatch& Mutation);
	bool ProcessDirtyEntries(
		const FBuildEntityRegistry& Registry,
		FBuildCollisionMutationBatch& Mutation);
	bool RebuildSelection(
		const FBuildEntityRegistry& Registry,
		const FBuildSpatialIndex& SpatialIndex,
		double CurrentTimeSeconds,
		FBuildCollisionMutationBatch& Mutation);
	bool ProcessPrefetchAdds(
		const FBuildEntityRegistry& Registry,
		double CurrentTimeSeconds,
		FBuildCollisionMutationBatch& Mutation);
	void ProcessEvictions(
		double CurrentTimeSeconds,
		FBuildCollisionMutationBatch& Mutation);
		bool ApplyMutations(
			FBuildCollisionMutationBatch& Mutation,
			ABuildCollisionHost& CollisionHost);
		bool HasEvictionWork() const;
		void ClearFailuresForEntity(FBuildEntityHandle Entity);
		bool IsPendingPrefetch(const FBuildCollisionPartKey& PartKey) const;

	FBuildCollisionActivationConfig ActivationConfig;
	uint32 RuntimeId = 0;
	TArray<FBuildCollisionSourceSlot> SourceSlots;
	int32 FirstFreeSourceIndex = INDEX_NONE;
	int32 SourceCount = 0;
	TMap<FBuildEntityHandle, FBuildProjectedCollisionEntity> ProjectedEntities;
	TSet<FBuildCollisionPartKey> RequiredParts;
	TSet<FBuildCollisionPartKey> RetainedParts;
	TMap<FBuildEntityHandle, FBuildCollisionDirtyEntry> DirtyEntries;
		TArray<FBuildCollisionPrefetchCandidate> PrefetchCandidates;
		int32 PrefetchCandidateHead = 0;
		TMap<FBuildCollisionPartKey, EBuildCollisionProjectionFailure> LastFailures;
	FBuildSpatialQueryScratch QueryScratch;
	TArray<FBuildEntityHandle> QueryCandidates;
	bool bSelectionDirty = false;
	int32 LastQueriedEntityCount = 0;
	int32 LastInspectedPartCount = 0;
	int32 LastChangedPartCount = 0;
	int32 EvictionCandidateCount = 0;
};

FBuildCollisionProcessorData::FBuildCollisionProcessorData(
	const FBuildCollisionActivationConfig& InActivationConfig)
	: ActivationConfig(InActivationConfig)
	, RuntimeId(AllocateBuildCollisionRuntimeId())
{
	check(ActivationConfig.IsValid());
}

FBuildCollisionSourceSlot* FBuildCollisionProcessorData::FindSource(
	const FBuildCollisionSourceHandle Handle)
{
	if (!Handle.IsSet()
		|| Handle.GetRuntimeId() != RuntimeId
		|| !SourceSlots.IsValidIndex(Handle.GetIndex()))
	{
		return nullptr;
	}
	FBuildCollisionSourceSlot& Slot = SourceSlots[Handle.GetIndex()];
	return Slot.bAlive && Slot.Generation == Handle.GetGeneration()
		? &Slot
		: nullptr;
}

const FBuildCollisionSourceSlot* FBuildCollisionProcessorData::FindSource(
	const FBuildCollisionSourceHandle Handle) const
{
	return const_cast<FBuildCollisionProcessorData*>(this)->FindSource(Handle);
}

FBuildProjectedCollisionEntity* FBuildCollisionProcessorData::FindEntity(
	const FBuildEntityHandle Entity)
{
	return ProjectedEntities.Find(Entity);
}

const FBuildProjectedCollisionEntity* FBuildCollisionProcessorData::FindEntity(
	const FBuildEntityHandle Entity) const
{
	return ProjectedEntities.Find(Entity);
}

FBuildProjectedCollisionPart* FBuildCollisionProcessorData::FindPart(
	const FBuildCollisionPartKey& PartKey)
{
	FBuildProjectedCollisionEntity* Entity = FindEntity(PartKey.Entity);
	return Entity
		? Entity->Parts.FindByPredicate(
			[&PartKey](const FBuildProjectedCollisionPart& Part)
			{
				return Part.CollisionPartId == PartKey.CollisionPartId;
			})
		: nullptr;
}

const FBuildProjectedCollisionPart* FBuildCollisionProcessorData::FindPart(
	const FBuildCollisionPartKey& PartKey) const
{
	return const_cast<FBuildCollisionProcessorData*>(this)->FindPart(PartKey);
}

bool FBuildCollisionProcessorData::IntersectsAnyRetentionBounds(
	const FBox& Bounds) const
{
	for (const FBuildCollisionSourceSlot& Slot : SourceSlots)
	{
		if (Slot.bAlive && Bounds.Intersect(Slot.Source.RetentionBounds))
		{
			return true;
		}
	}
	return false;
}

bool FBuildCollisionProcessorData::IsEntityRelevant(
	const FBuildEntityHandle Entity,
	const FBuildSpatialIndex& SpatialIndex) const
{
	if (ProjectedEntities.Contains(Entity))
	{
		return true;
	}
	FBox EntityBounds(ForceInit);
	return SpatialIndex.TryGetBounds(Entity, EntityBounds)
		&& IntersectsAnyRetentionBounds(EntityBounds);
}

bool FBuildCollisionProcessorData::IsEntityTracked(
	const FBuildEntityHandle Entity) const
{
	if (ProjectedEntities.Contains(Entity) || DirtyEntries.Contains(Entity))
	{
		return true;
	}
	for (int32 Index = PrefetchCandidateHead; Index < PrefetchCandidates.Num(); ++Index)
	{
		if (PrefetchCandidates[Index].PartKey.Entity == Entity)
		{
			return true;
		}
	}
	return false;
}

void FBuildCollisionProcessorData::MarkRebuild(const FBuildEntityHandle Entity)
{
	FBuildCollisionDirtyEntry& Entry = DirtyEntries.FindOrAdd(Entity);
	Entry.Entity = Entity;
	if (Entry.Mode != EBuildCollisionDirtyMode::Destroy)
	{
		Entry.Mode = EBuildCollisionDirtyMode::Rebuild;
		Entry.MeshPartIds.Reset();
	}
	bSelectionDirty = true;
}

void FBuildCollisionProcessorData::MarkDestroy(const FBuildEntityHandle Entity)
{
	FBuildCollisionDirtyEntry& Entry = DirtyEntries.FindOrAdd(Entity);
	Entry.Entity = Entity;
	Entry.Mode = EBuildCollisionDirtyMode::Destroy;
	Entry.MeshPartIds.Reset();
	bSelectionDirty = true;
}

void FBuildCollisionProcessorData::MarkParts(
	const FBuildEntityHandle Entity,
	const TConstArrayView<int32> MeshPartIds)
{
	FBuildCollisionDirtyEntry& Entry = DirtyEntries.FindOrAdd(Entity);
	if (!Entry.Entity.IsSet())
	{
		Entry.Entity = Entity;
	}
	if (Entry.Mode == EBuildCollisionDirtyMode::Destroy
		|| Entry.Mode == EBuildCollisionDirtyMode::Rebuild)
	{
		return;
	}
	Entry.Mode = EBuildCollisionDirtyMode::PartSet;
	for (const int32 MeshPartId : MeshPartIds)
	{
		Entry.MeshPartIds.AddUnique(MeshPartId);
	}
	bSelectionDirty = true;
}

void FBuildCollisionProcessorData::QueuePartRemoval(
	const FBuildCollisionPartKey& PartKey,
	FBuildCollisionMutationBatch& Mutation)
{
	if (Mutation.PendingAdds.Remove(PartKey) > 0)
	{
		return;
	}
	FBuildProjectedCollisionEntity* EntityRecord = FindEntity(PartKey.Entity);
	if (!EntityRecord)
	{
		return;
	}
	const int32 PartIndex = EntityRecord->Parts.IndexOfByPredicate(
		[&PartKey](const FBuildProjectedCollisionPart& Part)
		{
			return Part.CollisionPartId == PartKey.CollisionPartId;
		});
	if (PartIndex == INDEX_NONE)
	{
		return;
	}
	const FBuildCollisionInstanceHandle Instance = EntityRecord->Parts[PartIndex].Instance;
	if (!Mutation.RemovedInstances.Contains(Instance))
	{
		Mutation.Removals.Add(Instance);
		Mutation.RemovalPartKeys.Add(PartKey);
		Mutation.RemovedInstances.Add(Instance);
	}
	EntityRecord->Parts.RemoveAtSwap(PartIndex, 1, EAllowShrinking::No);
	if (EntityRecord->Parts.IsEmpty())
	{
		ProjectedEntities.Remove(PartKey.Entity);
	}
	RequiredParts.Remove(PartKey);
	RetainedParts.Remove(PartKey);
	++LastChangedPartCount;
}

void FBuildCollisionProcessorData::QueueEntityRemoval(
	const FBuildEntityHandle Entity,
	FBuildCollisionMutationBatch& Mutation)
{
	TArray<FBuildCollisionPartKey, TInlineAllocator<4>> PartKeys;
	if (const FBuildProjectedCollisionEntity* EntityRecord = FindEntity(Entity))
	{
		for (const FBuildProjectedCollisionPart& Part : EntityRecord->Parts)
		{
			PartKeys.Add({Entity, Part.CollisionPartId});
		}
	}
	for (const TPair<FBuildCollisionPartKey, FBuildPendingCollisionAdd>& Pair
		: Mutation.PendingAdds)
	{
		if (Pair.Key.Entity == Entity && !PartKeys.Contains(Pair.Key))
		{
			PartKeys.Add(Pair.Key);
		}
	}
	for (const FBuildCollisionPartKey& PartKey : PartKeys)
	{
		QueuePartRemoval(PartKey, Mutation);
	}
}

bool FBuildCollisionProcessorData::QueuePartProjection(
	const FBuildEntityRegistry& Registry,
	const FBuildCollisionPartKey& PartKey,
	const double CurrentTimeSeconds,
	FBuildCollisionMutationBatch& Mutation)
{
	if (FindPart(PartKey) || Mutation.PendingAdds.Contains(PartKey))
	{
		return true;
	}
	const FBuildTransformFragment* Transform = nullptr;
	const UBuildingDefinition* Definition = nullptr;
	const FBuildPartTransformFragment* PartTransforms = nullptr;
	if (!TryResolveCollisionDefinitionAndTransforms(
			Registry,
			PartKey.Entity,
			Transform,
			Definition,
			PartTransforms)
			)
	{
		LastFailures.Add(
			PartKey,
			EBuildCollisionProjectionFailure::MissingDefinitionOrTransform);
		return false;
	}
	if (!Definition->CollisionParts.IsValidIndex(PartKey.CollisionPartId))
	{
		LastFailures.Add(
			PartKey,
			EBuildCollisionProjectionFailure::InvalidCollisionPart);
		return false;
	}

	const FBuildCollisionPartDefinition& Part =
		Definition->CollisionParts[PartKey.CollisionPartId];
	FBuildPendingCollisionAdd Pending;
	Pending.PartKey = PartKey;
	Pending.ClusterKey.Mesh = Part.CollisionMesh;
	Pending.ClusterKey.Mobility = Part.Mobility;
	Pending.ClusterKey.CollisionProfileName = Part.GetEffectiveCollisionProfileName();
	Pending.LastRequiredTimeSeconds = CurrentTimeSeconds;
	if (!Pending.ClusterKey.IsSet())
	{
		LastFailures.Add(
			PartKey,
			EBuildCollisionProjectionFailure::InvalidClusterConfiguration);
		return false;
	}
	if (!Definition->TryCalculateCollisionPartWorldTransform(
				PartKey.CollisionPartId,
			Transform->WorldTransform,
			GetPartTransformView(PartTransforms),
			Pending.WorldTransform)
		|| !Definition->TryCalculateCollisionPartWorldBounds(
			PartKey.CollisionPartId,
			Transform->WorldTransform,
				GetPartTransformView(PartTransforms),
				Pending.WorldBounds))
	{
		LastFailures.Add(
			PartKey,
			EBuildCollisionProjectionFailure::InvalidCollisionGeometry);
		return false;
	}
	Mutation.PendingAdds.Add(PartKey, MoveTemp(Pending));
	++LastChangedPartCount;
	return true;
}

bool FBuildCollisionProcessorData::QueuePartUpdates(
	const FBuildEntityRegistry& Registry,
	const FBuildCollisionDirtyEntry& Dirty,
	FBuildCollisionMutationBatch& Mutation)
{
	if (!Registry.IsAlive(Dirty.Entity))
	{
		QueueEntityRemoval(Dirty.Entity, Mutation);
		return true;
	}
	const FBuildTransformFragment* Transform = nullptr;
	const UBuildingDefinition* Definition = nullptr;
	const FBuildPartTransformFragment* PartTransforms = nullptr;
	if (!TryResolveCollisionDefinitionAndTransforms(
			Registry,
			Dirty.Entity,
			Transform,
			Definition,
				PartTransforms))
	{
		if (const FBuildProjectedCollisionEntity* Existing = FindEntity(Dirty.Entity))
		{
			for (const FBuildProjectedCollisionPart& Part : Existing->Parts)
			{
				LastFailures.Add(
					{Dirty.Entity, Part.CollisionPartId},
					EBuildCollisionProjectionFailure::MissingDefinitionOrTransform);
			}
		}
		return false;
	}

	for (int32 CollisionPartId = 0;
		CollisionPartId < Definition->CollisionParts.Num();
		++CollisionPartId)
	{
		const FBuildCollisionPartDefinition& Part =
			Definition->CollisionParts[CollisionPartId];
		if (Part.DrivenMeshPartId == INDEX_NONE
			|| !Dirty.MeshPartIds.Contains(Part.DrivenMeshPartId))
		{
			continue;
		}
		const FBuildCollisionPartKey PartKey{Dirty.Entity, CollisionPartId};
		FTransform WorldTransform;
		FBox WorldBounds(ForceInit);
			if (!Definition->TryCalculateCollisionPartWorldTransform(
				CollisionPartId,
				Transform->WorldTransform,
				GetPartTransformView(PartTransforms),
				WorldTransform)
			|| !Definition->TryCalculateCollisionPartWorldBounds(
				CollisionPartId,
				Transform->WorldTransform,
				GetPartTransformView(PartTransforms),
					WorldBounds))
			{
				LastFailures.Add(
					PartKey,
					EBuildCollisionProjectionFailure::InvalidCollisionGeometry);
				return false;
		}
		if (FBuildPendingCollisionAdd* Pending = Mutation.PendingAdds.Find(PartKey))
		{
			Pending->WorldTransform = WorldTransform;
			Pending->WorldBounds = WorldBounds;
			continue;
		}
		FBuildProjectedCollisionPart* Existing = FindPart(PartKey);
		if (!Existing || Mutation.RemovedInstances.Contains(Existing->Instance))
		{
			continue;
		}
		Existing->WorldBounds = WorldBounds;
		FBuildCollisionInstanceUpdate Update;
		Update.Instance = Existing->Instance;
		Update.WorldTransform = WorldTransform;
			FBuildCollisionUpdateBatch& UpdateBatch =
				Mutation.Updates.FindOrAdd(Existing->ClusterKey);
			UpdateBatch.Updates.Add(Update);
			UpdateBatch.PartKeys.Add(PartKey);
		++LastChangedPartCount;
	}
	return true;
}

bool FBuildCollisionProcessorData::ProcessDirtyEntries(
	const FBuildEntityRegistry& Registry,
	FBuildCollisionMutationBatch& Mutation)
{
	for (const TPair<FBuildEntityHandle, FBuildCollisionDirtyEntry>& Pair
		: DirtyEntries)
	{
		const FBuildCollisionDirtyEntry& Dirty = Pair.Value;
		if (Dirty.Mode == EBuildCollisionDirtyMode::Destroy
			|| Dirty.Mode == EBuildCollisionDirtyMode::Rebuild)
		{
			QueueEntityRemoval(Dirty.Entity, Mutation);
			continue;
		}
		if (!QueuePartUpdates(Registry, Dirty, Mutation))
		{
			return false;
		}
	}
	DirtyEntries.Reset();
	return true;
}

bool FBuildCollisionProcessorData::RebuildSelection(
	const FBuildEntityRegistry& Registry,
	const FBuildSpatialIndex& SpatialIndex,
	const double CurrentTimeSeconds,
	FBuildCollisionMutationBatch& Mutation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Collision_LocalSelection);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CollisionLocalSelection);
	TMap<FBuildCollisionPartKey, FBuildCollisionSelectionEntry> Selection;
	LastQueriedEntityCount = 0;
	LastInspectedPartCount = 0;

	for (const FBuildCollisionSourceSlot& SourceSlot : SourceSlots)
	{
		if (!SourceSlot.bAlive)
		{
			continue;
		}
		const FBuildCollisionSource& Source = SourceSlot.Source;
		SpatialIndex.QueryOverlaps(
			Source.RetentionBounds,
			QueryScratch,
			QueryCandidates);
		LastQueriedEntityCount += QueryCandidates.Num();
		for (const FBuildEntityHandle Entity : QueryCandidates)
		{
			const FBuildTransformFragment* Transform = nullptr;
			const UBuildingDefinition* Definition = nullptr;
			const FBuildPartTransformFragment* PartTransforms = nullptr;
			if (!TryResolveCollisionDefinitionAndTransforms(
					Registry,
					Entity,
					Transform,
					Definition,
					PartTransforms))
			{
				continue;
			}

			for (int32 CollisionPartId = 0;
				CollisionPartId < Definition->CollisionParts.Num();
				++CollisionPartId)
			{
				++LastInspectedPartCount;
				FBox PartBounds(ForceInit);
					if (!Definition->TryCalculateCollisionPartWorldBounds(
						CollisionPartId,
						Transform->WorldTransform,
						GetPartTransformView(PartTransforms),
							PartBounds))
					{
						LastFailures.Add(
							{Entity, CollisionPartId},
							EBuildCollisionProjectionFailure::InvalidCollisionGeometry);
						return false;
				}
				if (!PartBounds.Intersect(Source.RetentionBounds))
				{
					continue;
				}

				const FBuildCollisionPartKey PartKey{Entity, CollisionPartId};
				FBuildCollisionSelectionEntry& Entry = Selection.FindOrAdd(PartKey);
				Entry.PartKey = PartKey;
				Entry.WorldBounds = PartBounds;
				Entry.bRetained = true;
				const bool bUrgent = PartBounds.Intersect(Source.ImmediateBounds)
					|| PartBounds.Intersect(Source.CameraBounds);
				const bool bRequired = bUrgent
					|| PartBounds.Intersect(Source.PrefetchBounds);
				Entry.bUrgent = Entry.bUrgent || bUrgent;
				Entry.bRequired = Entry.bRequired || bRequired;
				if (!bRequired)
				{
					continue;
				}

				const double SquaredDistance = SquaredDistanceFromBoundsToPoint(
					PartBounds,
					Source.SubjectLocation);
				const double Speed = Source.Velocity.Size();
				const double ExpectedTime = Speed > UE_KINDA_SMALL_NUMBER
					? FMath::Sqrt(SquaredDistance) / Speed
					: TNumericLimits<double>::Max();
				Entry.SquaredDistance = FMath::Min(
					Entry.SquaredDistance,
					SquaredDistance);
				Entry.ExpectedContactTimeSeconds = FMath::Min(
					Entry.ExpectedContactTimeSeconds,
					ExpectedTime);
			}
		}
	}

	TSet<FBuildCollisionPartKey> NewRequiredParts;
	TSet<FBuildCollisionPartKey> NewRetainedParts;
	NewRequiredParts.Reserve(Selection.Num());
	NewRetainedParts.Reserve(Selection.Num());
	for (const TPair<FBuildCollisionPartKey, FBuildCollisionSelectionEntry>& Pair
		: Selection)
	{
		if (Pair.Value.bRequired)
		{
			NewRequiredParts.Add(Pair.Key);
		}
		if (Pair.Value.bRetained)
		{
			NewRetainedParts.Add(Pair.Key);
		}
	}

	for (TPair<FBuildEntityHandle, FBuildProjectedCollisionEntity>& EntityPair
		: ProjectedEntities)
	{
		for (FBuildProjectedCollisionPart& Part : EntityPair.Value.Parts)
		{
			const FBuildCollisionPartKey PartKey{
				EntityPair.Key,
				Part.CollisionPartId};
			if (const FBuildCollisionSelectionEntry* Entry = Selection.Find(PartKey))
			{
				Part.WorldBounds = Entry->WorldBounds;
			}
			if (NewRequiredParts.Contains(PartKey))
			{
				Part.LastRequiredTimeSeconds = CurrentTimeSeconds;
				Part.RetentionExitTimeSeconds = -1.0;
			}
			else if (NewRetainedParts.Contains(PartKey))
			{
				Part.RetentionExitTimeSeconds = -1.0;
			}
			else if (Part.RetentionExitTimeSeconds < 0.0)
			{
				Part.RetentionExitTimeSeconds = CurrentTimeSeconds;
			}
		}
	}

	RequiredParts = MoveTemp(NewRequiredParts);
	RetainedParts = MoveTemp(NewRetainedParts);
	PrefetchCandidates.Reset();
	PrefetchCandidateHead = 0;
	for (const TPair<FBuildCollisionPartKey, FBuildCollisionSelectionEntry>& Pair
		: Selection)
	{
		if (!Pair.Value.bRequired
			|| FindPart(Pair.Key)
			|| Mutation.PendingAdds.Contains(Pair.Key))
		{
			continue;
		}
		if (Pair.Value.bUrgent)
		{
			if (!QueuePartProjection(
					Registry,
					Pair.Key,
					CurrentTimeSeconds,
					Mutation))
			{
				return false;
			}
			continue;
		}

		PrefetchCandidates.Add({
			Pair.Key,
			Pair.Value.ExpectedContactTimeSeconds,
			Pair.Value.SquaredDistance});
	}
	PrefetchCandidates.Sort(
		[](const FBuildCollisionPrefetchCandidate& Left,
			const FBuildCollisionPrefetchCandidate& Right)
		{
			if (Left.ExpectedContactTimeSeconds != Right.ExpectedContactTimeSeconds)
			{
				return Left.ExpectedContactTimeSeconds < Right.ExpectedContactTimeSeconds;
			}
			if (Left.SquaredDistance != Right.SquaredDistance)
			{
				return Left.SquaredDistance < Right.SquaredDistance;
			}
			return IsPartKeyLess(Left.PartKey, Right.PartKey);
		});
	bSelectionDirty = false;
	return true;
}

bool FBuildCollisionProcessorData::ProcessPrefetchAdds(
	const FBuildEntityRegistry& Registry,
	const double CurrentTimeSeconds,
	FBuildCollisionMutationBatch& Mutation)
{
	int32 AddedCount = 0;
	while (PrefetchCandidateHead < PrefetchCandidates.Num()
		&& AddedCount < ActivationConfig.MaxPrefetchAddsPerFrame)
	{
		const FBuildCollisionPartKey PartKey =
			PrefetchCandidates[PrefetchCandidateHead++].PartKey;
		if (!RequiredParts.Contains(PartKey)
			|| FindPart(PartKey)
			|| Mutation.PendingAdds.Contains(PartKey))
		{
			continue;
		}
		if (!QueuePartProjection(
				Registry,
				PartKey,
				CurrentTimeSeconds,
				Mutation))
		{
			return false;
		}
		++AddedCount;
	}
	if (PrefetchCandidateHead >= PrefetchCandidates.Num())
	{
		PrefetchCandidates.Reset();
		PrefetchCandidateHead = 0;
	}
	return true;
}

void FBuildCollisionProcessorData::ProcessEvictions(
	const double CurrentTimeSeconds,
	FBuildCollisionMutationBatch& Mutation)
{
	TArray<FBuildCollisionEvictionCandidate> Eligible;
	EvictionCandidateCount = 0;
	for (const TPair<FBuildEntityHandle, FBuildProjectedCollisionEntity>& EntityPair
		: ProjectedEntities)
	{
		for (const FBuildProjectedCollisionPart& Part : EntityPair.Value.Parts)
		{
			const FBuildCollisionPartKey PartKey{
				EntityPair.Key,
				Part.CollisionPartId};
			if (RequiredParts.Contains(PartKey)
				|| RetainedParts.Contains(PartKey)
				|| Part.RetentionExitTimeSeconds < 0.0)
			{
				continue;
			}
			++EvictionCandidateCount;
			if (CurrentTimeSeconds - Part.RetentionExitTimeSeconds
				< ActivationConfig.EvictionGraceSeconds)
			{
				continue;
			}

			double ClosestSquaredDistance = TNumericLimits<double>::Max();
			for (const FBuildCollisionSourceSlot& Source : SourceSlots)
			{
				if (Source.bAlive)
				{
					ClosestSquaredDistance = FMath::Min(
						ClosestSquaredDistance,
						SquaredDistanceFromBoundsToPoint(
							Part.WorldBounds,
							Source.Source.SubjectLocation));
				}
			}
			Eligible.Add({
				PartKey,
				ClosestSquaredDistance,
				Part.LastRequiredTimeSeconds});
		}
	}

	Eligible.Sort(
		[](const FBuildCollisionEvictionCandidate& Left,
			const FBuildCollisionEvictionCandidate& Right)
		{
			if (Left.SquaredDistance != Right.SquaredDistance)
			{
				return Left.SquaredDistance > Right.SquaredDistance;
			}
			if (Left.LastRequiredTimeSeconds != Right.LastRequiredTimeSeconds)
			{
				return Left.LastRequiredTimeSeconds < Right.LastRequiredTimeSeconds;
			}
			return IsPartKeyLess(Left.PartKey, Right.PartKey);
		});

	const int32 RemoveCount = FMath::Min(
		Eligible.Num(),
		ActivationConfig.MaxRemovesPerFrame);
	for (int32 Index = 0; Index < RemoveCount; ++Index)
	{
		QueuePartRemoval(Eligible[Index].PartKey, Mutation);
	}
	EvictionCandidateCount -= RemoveCount;
}

bool FBuildCollisionProcessorData::ApplyMutations(
	FBuildCollisionMutationBatch& Mutation,
	ABuildCollisionHost& CollisionHost)
{
	if (!CollisionHost.RemoveInstances(Mutation.Removals))
	{
		for (const FBuildCollisionPartKey& PartKey : Mutation.RemovalPartKeys)
		{
			LastFailures.Add(
				PartKey,
				EBuildCollisionProjectionFailure::HostRemoveFailed);
		}
		return false;
	}
	for (const FBuildCollisionPartKey& PartKey : Mutation.RemovalPartKeys)
	{
		LastFailures.Remove(PartKey);
	}

	TMap<FBuildCollisionClusterKey, FBuildCollisionAddBatch> AddBatches;
	for (const TPair<FBuildCollisionPartKey, FBuildPendingCollisionAdd>& Pair
		: Mutation.PendingAdds)
	{
		FBuildCollisionAddBatch& Batch = AddBatches.FindOrAdd(Pair.Value.ClusterKey);
		Batch.PartKeys.Add(Pair.Key);
		Batch.WorldTransforms.Add(Pair.Value.WorldTransform);
	}
	for (TPair<FBuildCollisionClusterKey, FBuildCollisionAddBatch>& Pair
		: AddBatches)
	{
		TArray<FBuildCollisionInstanceHandle> AddedInstances;
		if (!CollisionHost.AddInstances(
				Pair.Key,
				Pair.Value.WorldTransforms,
				AddedInstances)
				|| AddedInstances.Num() != Pair.Value.PartKeys.Num())
		{
			for (const FBuildCollisionPartKey& PartKey : Pair.Value.PartKeys)
			{
				LastFailures.Add(
					PartKey,
					EBuildCollisionProjectionFailure::HostAddFailed);
			}
			return false;
		}
		for (int32 Index = 0; Index < AddedInstances.Num(); ++Index)
		{
			const FBuildCollisionPartKey& PartKey = Pair.Value.PartKeys[Index];
			const FBuildPendingCollisionAdd* Pending = Mutation.PendingAdds.Find(PartKey);
			if (!Pending)
			{
				LastFailures.Add(
					PartKey,
					EBuildCollisionProjectionFailure::ProcessorStateInvalid);
				return false;
			}
			FBuildProjectedCollisionEntity& EntityRecord =
				ProjectedEntities.FindOrAdd(PartKey.Entity);
			EntityRecord.Entity = PartKey.Entity;
			FBuildProjectedCollisionPart& Part =
				EntityRecord.Parts.AddDefaulted_GetRef();
			Part.CollisionPartId = PartKey.CollisionPartId;
			Part.Instance = AddedInstances[Index];
			Part.ClusterKey = Pending->ClusterKey;
			Part.WorldBounds = Pending->WorldBounds;
			Part.LastRequiredTimeSeconds = Pending->LastRequiredTimeSeconds;
			Part.RetentionExitTimeSeconds = -1.0;
			LastFailures.Remove(PartKey);
		}
	}

	for (TPair<FBuildCollisionClusterKey, FBuildCollisionUpdateBatch>& Pair
		: Mutation.Updates)
	{
		for (int32 Index = Pair.Value.Updates.Num() - 1; Index >= 0; --Index)
		{
			if (Mutation.RemovedInstances.Contains(Pair.Value.Updates[Index].Instance))
			{
				Pair.Value.Updates.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				Pair.Value.PartKeys.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
		if (!CollisionHost.UpdateInstances(Pair.Key, Pair.Value.Updates))
		{
			for (const FBuildCollisionPartKey& PartKey : Pair.Value.PartKeys)
			{
				LastFailures.Add(
					PartKey,
					EBuildCollisionProjectionFailure::HostUpdateFailed);
			}
			return false;
		}
		for (const FBuildCollisionPartKey& PartKey : Pair.Value.PartKeys)
		{
			LastFailures.Remove(PartKey);
		}
	}
	return true;
}

bool FBuildCollisionProcessorData::HasEvictionWork() const
{
	for (const TPair<FBuildEntityHandle, FBuildProjectedCollisionEntity>& EntityPair
		: ProjectedEntities)
	{
		for (const FBuildProjectedCollisionPart& Part : EntityPair.Value.Parts)
		{
			const FBuildCollisionPartKey PartKey{
				EntityPair.Key,
				Part.CollisionPartId};
			if (!RequiredParts.Contains(PartKey)
				&& !RetainedParts.Contains(PartKey)
				&& Part.RetentionExitTimeSeconds >= 0.0)
			{
				return true;
			}
		}
	}
	return false;
}

void FBuildCollisionProcessorData::ClearFailuresForEntity(
	const FBuildEntityHandle Entity)
{
	for (auto It = LastFailures.CreateIterator(); It; ++It)
	{
		if (It.Key().Entity == Entity)
		{
			It.RemoveCurrent();
		}
	}
}

bool FBuildCollisionProcessorData::IsPendingPrefetch(
	const FBuildCollisionPartKey& PartKey) const
{
	for (int32 Index = PrefetchCandidateHead; Index < PrefetchCandidates.Num(); ++Index)
	{
		if (PrefetchCandidates[Index].PartKey == PartKey)
		{
			return true;
		}
	}
	return false;
}

FBuildCollisionProcessor::FBuildCollisionProcessor(
	const FBuildCollisionActivationConfig& InActivationConfig)
	: Data(MakeUnique<FBuildCollisionProcessorData>(InActivationConfig))
{
}

FBuildCollisionProcessor::~FBuildCollisionProcessor() = default;

FBuildCollisionSourceHandle FBuildCollisionProcessor::RegisterSource(
	const FBuildCollisionSource& Source)
{
	check(IsInGameThread());
	if (!Data || !Source.IsValid())
	{
		return {};
	}

	int32 SlotIndex = INDEX_NONE;
	if (Data->FirstFreeSourceIndex != INDEX_NONE)
	{
		SlotIndex = Data->FirstFreeSourceIndex;
		FBuildCollisionSourceSlot& Slot = Data->SourceSlots[SlotIndex];
		Data->FirstFreeSourceIndex = Slot.NextFreeIndex;
		Slot.NextFreeIndex = INDEX_NONE;
	}
	else
	{
		SlotIndex = Data->SourceSlots.AddDefaulted();
	}

	FBuildCollisionSourceSlot& Slot = Data->SourceSlots[SlotIndex];
	check(!Slot.bAlive && Slot.Generation != 0);
	Slot.bAlive = true;
	Slot.Source = Source;
	++Data->SourceCount;
	Data->bSelectionDirty = true;
	return FBuildCollisionSourceHandle(
		Data->RuntimeId,
		SlotIndex,
		Slot.Generation);
}

bool FBuildCollisionProcessor::UpdateSource(
	const FBuildCollisionSourceHandle Source,
	const FBuildCollisionSource& SourceData)
{
	check(IsInGameThread());
	FBuildCollisionSourceSlot* Slot = Data ? Data->FindSource(Source) : nullptr;
	if (!Slot || !SourceData.IsValid())
	{
		return false;
	}
	if (AreSourcesEquivalent(Slot->Source, SourceData))
	{
		return true;
	}
	Slot->Source = SourceData;
	Data->bSelectionDirty = true;
	return true;
}

bool FBuildCollisionProcessor::UnregisterSource(
	const FBuildCollisionSourceHandle Source)
{
	check(IsInGameThread());
	FBuildCollisionSourceSlot* Slot = Data ? Data->FindSource(Source) : nullptr;
	if (!Slot)
	{
		return false;
	}
	Slot->bAlive = false;
	Slot->Source = {};
	AdvanceCollisionSourceGeneration(Slot->Generation);
	Slot->NextFreeIndex = Data->FirstFreeSourceIndex;
	Data->FirstFreeSourceIndex = Source.GetIndex();
	--Data->SourceCount;
	Data->bSelectionDirty = true;
	return true;
}

bool FBuildCollisionProcessor::IsValidSource(
	const FBuildCollisionSourceHandle Source) const
{
	check(IsInGameThread());
	return Data && Data->FindSource(Source) != nullptr;
}

void FBuildCollisionProcessor::NotifyEntityCreated(
	const FBuildEntityHandle Entity,
	const FBuildSpatialIndex& SpatialIndex)
{
	check(IsInGameThread());
	if (Data && Data->IsEntityRelevant(Entity, SpatialIndex))
	{
		Data->bSelectionDirty = true;
	}
}

void FBuildCollisionProcessor::NotifyEntityDestroyed(
	const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!Data)
	{
		return;
	}
	Data->ClearFailuresForEntity(Entity);
	if (Data->IsEntityTracked(Entity))
	{
		Data->MarkDestroy(Entity);
	}
}

void FBuildCollisionProcessor::NotifyEntityTransformChanged(
	const FBuildEntityHandle Entity,
	const FBuildSpatialIndex& SpatialIndex)
{
	check(IsInGameThread());
	if (Data && Data->IsEntityRelevant(Entity, SpatialIndex))
	{
		Data->MarkRebuild(Entity);
	}
}

void FBuildCollisionProcessor::NotifyPartTransformsChanged(
	const FBuildEntityHandle Entity,
	const TConstArrayView<int32> MeshPartIds,
	const FBuildEntityRegistry& Registry,
	const FBuildSpatialIndex& SpatialIndex)
{
	check(IsInGameThread());
	if (!Data || MeshPartIds.IsEmpty()
		|| !Data->IsEntityRelevant(Entity, SpatialIndex))
	{
		return;
	}
	const FBuildDefinitionFragment* DefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment
		? DefinitionFragment->Definition.Get()
		: nullptr;
	if (!Definition)
	{
		return;
	}
	if (Definition->DoPartTransformChangesAffectSpatialBounds(MeshPartIds))
	{
		Data->MarkRebuild(Entity);
	}
	else
	{
		Data->MarkParts(Entity, MeshPartIds);
	}
}

bool FBuildCollisionProcessor::Execute(
	const FBuildEntityRegistry& Registry,
	const FBuildSpatialIndex& SpatialIndex,
	ABuildCollisionHost& CollisionHost,
	const double CurrentTimeSeconds)
{
	check(IsInGameThread());
	if (!Data || !FMath::IsFinite(CurrentTimeSeconds))
	{
		return false;
	}
	if (!HasPendingWork())
	{
		return true;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Collision_Flush);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CollisionFlush);
	Data->LastQueriedEntityCount = 0;
	Data->LastInspectedPartCount = 0;
	Data->LastChangedPartCount = 0;
	FBuildCollisionMutationBatch Mutation;
	if (!Data->ProcessDirtyEntries(Registry, Mutation)
		|| (Data->bSelectionDirty
			&& !Data->RebuildSelection(
				Registry,
				SpatialIndex,
				CurrentTimeSeconds,
				Mutation))
		|| !Data->ProcessPrefetchAdds(Registry, CurrentTimeSeconds, Mutation))
	{
		RequestFullReprojection(CollisionHost);
		return false;
	}
	Data->ProcessEvictions(CurrentTimeSeconds, Mutation);
	if (!Data->ApplyMutations(Mutation, CollisionHost))
	{
		RequestFullReprojection(CollisionHost);
		return false;
	}

	CSV_CUSTOM_STAT(
		ElementSandboxBuilding,
		CollisionRequiredParts,
		Data->RequiredParts.Num(),
		ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(
		ElementSandboxBuilding,
		CollisionPendingPrefetchAdds,
		GetPendingPrefetchAddCount(),
		ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(
		ElementSandboxBuilding,
		CollisionEvictionCandidates,
		Data->EvictionCandidateCount,
		ECsvCustomStatOp::Set);
	return true;
}

bool FBuildCollisionProcessor::HasPendingWork() const
{
	check(IsInGameThread());
	return Data
		&& (Data->bSelectionDirty
			|| !Data->DirtyEntries.IsEmpty()
			|| Data->PrefetchCandidateHead < Data->PrefetchCandidates.Num()
			|| Data->HasEvictionWork());
}

int32 FBuildCollisionProcessor::GetSourceCount() const
{
	check(IsInGameThread());
	return Data ? Data->SourceCount : 0;
}

int32 FBuildCollisionProcessor::GetProjectedEntityCount() const
{
	check(IsInGameThread());
	return Data ? Data->ProjectedEntities.Num() : 0;
}

int32 FBuildCollisionProcessor::GetProjectedPartCount(
	const FBuildEntityHandle Entity) const
{
	check(IsInGameThread());
	const FBuildProjectedCollisionEntity* Record = Data
		? Data->FindEntity(Entity)
		: nullptr;
	return Record ? Record->Parts.Num() : 0;
}

int32 FBuildCollisionProcessor::GetRequiredPartCount() const
{
	check(IsInGameThread());
	return Data ? Data->RequiredParts.Num() : 0;
}

int32 FBuildCollisionProcessor::GetCachedOnlyPartCount() const
{
	check(IsInGameThread());
	if (!Data)
	{
		return 0;
	}
	int32 CachedCount = 0;
	for (const TPair<FBuildEntityHandle, FBuildProjectedCollisionEntity>& EntityPair
		: Data->ProjectedEntities)
	{
		for (const FBuildProjectedCollisionPart& Part : EntityPair.Value.Parts)
		{
			const FBuildCollisionPartKey PartKey{EntityPair.Key, Part.CollisionPartId};
			CachedCount += Data->RequiredParts.Contains(PartKey) ? 0 : 1;
		}
	}
	return CachedCount;
}

int32 FBuildCollisionProcessor::GetPendingPrefetchAddCount() const
{
	check(IsInGameThread());
	return Data
		? FMath::Max(0, Data->PrefetchCandidates.Num() - Data->PrefetchCandidateHead)
		: 0;
}

int32 FBuildCollisionProcessor::GetEvictionCandidateCount() const
{
	check(IsInGameThread());
	return Data ? Data->EvictionCandidateCount : 0;
}

int32 FBuildCollisionProcessor::GetLastQueriedEntityCount() const
{
	check(IsInGameThread());
	return Data ? Data->LastQueriedEntityCount : 0;
}

int32 FBuildCollisionProcessor::GetLastInspectedPartCount() const
{
	check(IsInGameThread());
	return Data ? Data->LastInspectedPartCount : 0;
}

int32 FBuildCollisionProcessor::GetLastChangedPartCount() const
{
	check(IsInGameThread());
	return Data ? Data->LastChangedPartCount : 0;
}

FBuildCollisionActivationConfig FBuildCollisionProcessor::GetActivationConfig() const
{
	check(IsInGameThread());
	return Data ? Data->ActivationConfig : FBuildCollisionActivationConfig();
}

SIZE_T FBuildCollisionProcessor::GetEstimatedAllocatedSize() const
{
	check(IsInGameThread());
	if (!Data)
	{
		return 0;
	}
	SIZE_T Size = Data->SourceSlots.GetAllocatedSize()
		+ Data->ProjectedEntities.GetAllocatedSize()
		+ Data->RequiredParts.GetAllocatedSize()
		+ Data->RetainedParts.GetAllocatedSize()
			+ Data->DirtyEntries.GetAllocatedSize()
			+ Data->PrefetchCandidates.GetAllocatedSize()
			+ Data->LastFailures.GetAllocatedSize()
		+ Data->QueryCandidates.GetAllocatedSize()
		+ Data->QueryScratch.Candidates.GetAllocatedSize()
		+ Data->QueryScratch.UniqueEntities.GetAllocatedSize();
	for (const TPair<FBuildEntityHandle, FBuildProjectedCollisionEntity>& Pair
		: Data->ProjectedEntities)
	{
		Size += Pair.Value.Parts.GetAllocatedSize();
	}
	return Size;
}

bool FBuildCollisionProcessor::TryGetInstanceHandle(
	const FBuildEntityHandle Entity,
	const int32 CollisionPartId,
	FBuildCollisionInstanceHandle& OutInstance) const
{
	check(IsInGameThread());
	OutInstance = {};
	const FBuildProjectedCollisionPart* Part = Data
		? Data->FindPart({Entity, CollisionPartId})
		: nullptr;
	if (!Part)
	{
		return false;
	}
	OutInstance = Part->Instance;
	return true;
}

bool FBuildCollisionProcessor::GetPartProjectionState(
	const FBuildEntityHandle Entity,
	const int32 CollisionPartId,
	FBuildCollisionPartProjectionState& OutState) const
{
	check(IsInGameThread());
	OutState = {};
	if (!Data || !Entity.IsSet() || CollisionPartId < 0)
	{
		return false;
	}

	const FBuildCollisionPartKey PartKey{Entity, CollisionPartId};
	OutState.bRequired = Data->RequiredParts.Contains(PartKey);
	OutState.bRetained = Data->RetainedParts.Contains(PartKey);
	OutState.bSelectionPending = Data->bSelectionDirty;
	OutState.bPendingPrefetch = Data->IsPendingPrefetch(PartKey);
	if (const FBuildProjectedCollisionPart* Part = Data->FindPart(PartKey))
	{
		OutState.ClusterKey = Part->ClusterKey;
		OutState.Instance = Part->Instance;
	}
	if (const EBuildCollisionProjectionFailure* Failure = Data->LastFailures.Find(PartKey))
	{
		OutState.LastFailure = *Failure;
	}
	return true;
}

void FBuildCollisionProcessor::RequestFullReprojection(
	ABuildCollisionHost& CollisionHost)
{
	check(IsInGameThread());
	CollisionHost.ClearInstances();
	Data->ProjectedEntities.Reset();
	Data->RequiredParts.Reset();
	Data->RetainedParts.Reset();
	Data->DirtyEntries.Reset();
	Data->PrefetchCandidates.Reset();
	Data->PrefetchCandidateHead = 0;
	Data->EvictionCandidateCount = 0;
	Data->bSelectionDirty = true;
}
