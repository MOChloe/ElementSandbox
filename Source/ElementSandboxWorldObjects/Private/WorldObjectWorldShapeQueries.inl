// UWorldObjectWorldSubsystem：稳定身份、中性查询快照、空间查询、统计与 Adapter 测试门面。
// 查询只读快照，不泄露 Registry 容器所有权。

bool UWorldObjectWorldSubsystem::IsEntityAlive(const FWorldObjectEntityHandle Entity) const
{
	return Runtime && Runtime->Core.Registry.IsAlive(Entity);
}

FWorldObjectEntityHandle UWorldObjectWorldSubsystem::FindEntity(const FWorldEntityId WorldEntityId) const
{
	if (!Runtime)
	{
		return {};
	}
	const FWorldObjectEntityHandle* Entity = Runtime->Core.EntityByWorldEntityId.Find(WorldEntityId);
	return Entity && Runtime->Core.Registry.IsAlive(*Entity) ? *Entity : FWorldObjectEntityHandle();
}

FWorldEntityId UWorldObjectWorldSubsystem::GetWorldEntityId(const FWorldObjectEntityHandle Entity) const
{
	const FWorldObjectWorldIdentityFragment* WorldEntityIdentity =
		Runtime ? Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity) : nullptr;
	return WorldEntityIdentity ? WorldEntityIdentity->WorldEntityId : FWorldEntityId();
}

bool UWorldObjectWorldSubsystem::BuildLifecycleRecord(const FWorldObjectEntityHandle Entity,
													  FWorldObjectLifecycleRecord& OutRecord) const
{
	OutRecord = {};
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	const FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
	const FWorldObjectDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
	const FWorldObjectTransformFragment* Transform =
		Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
	const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	if (!Identity || !Definition || !Transform)
	{
		return false;
	}
	OutRecord.Entity = Entity;
	OutRecord.WorldEntityId = Identity->WorldEntityId;
	OutRecord.DefinitionId = Definition->DefinitionId;
	OutRecord.WorldTransform = Transform->WorldTransform;
	OutRecord.SpatialClass = Definition->SpatialClass;
	OutRecord.StateRevision = Identity->StateRevision;
	return OutRecord.IsValid();
}

bool UWorldObjectWorldSubsystem::BuildShapeSnapshot(const FWorldObjectEntityHandle Entity,
													FWorldObjectShapeInstanceSnapshot& OutSnapshot) const
{
	OutSnapshot = {};
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	const FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
	const FWorldObjectDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
	const FWorldObjectTransformFragment* Transform =
		Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
	const FWorldObjectMotionFragment* Motion = Runtime->Core.Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
	const FWorldObjectInstanceInteractionBoundsFragment* InstanceBounds =
		Runtime->Core.Registry.FindFragment<FWorldObjectInstanceInteractionBoundsFragment>(Entity);
	const FWorldObjectInstanceShapeFragment* InstanceShape =
		Runtime->Core.Registry.FindFragment<FWorldObjectInstanceShapeFragment>(Entity);
	const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	if (!Identity || !Definition || !Transform || !Motion)
	{
		return false;
	}
	const FBox& InteractionBounds =
		InstanceBounds ? InstanceBounds->InteractionLocalBounds : Definition->InteractionLocalBounds;
	const FWorldObjectShapeDefinition& Geometry =
		InstanceShape ? InstanceShape->ShapeGeometry : Definition->ShapeGeometry;
	if (InteractionBounds.IsValid == 0 || InteractionBounds.ContainsNaN() || !Geometry.IsValid() ||
		Transform->Revision == 0)
	{
		return false;
	}

	OutSnapshot.ShapeRef.WorldEntityId = Identity->WorldEntityId;
	OutSnapshot.ShapeRef.Entity = Entity;
	OutSnapshot.ShapeRef.ShapeId = 0;
	OutSnapshot.DefinitionId = Definition->DefinitionId;
	OutSnapshot.SurfaceProfileId = Definition->SurfaceProfileId;
	OutSnapshot.SpatialClass = Definition->SpatialClass;
	OutSnapshot.MotionState = Motion->State;
	OutSnapshot.TemplateRevision = Geometry.TemplateRevision;
	OutSnapshot.TransformRevision = Transform->Revision;
	OutSnapshot.ShapeRevision = InstanceShape ? InstanceShape->Revision : 1;
	OutSnapshot.StateRevision = Identity->StateRevision;
	OutSnapshot.LocalGeometry = Geometry;
	OutSnapshot.WorldTransform = Transform->WorldTransform;
	OutSnapshot.InteractionWorldBounds = InteractionBounds.TransformBy(Transform->WorldTransform);
	OutSnapshot.WorldBounds = Geometry.CalculateBroadphaseBounds(Transform->WorldTransform);
	return OutSnapshot.IsValid();
}

bool UWorldObjectWorldSubsystem::PublishShapeTransition(const TOptional<FWorldObjectShapeInstanceSnapshot>& Previous,
														const TOptional<FWorldObjectShapeInstanceSnapshot>& Current,
														const EWorldObjectQuerySnapshotChangeKind RetainedKind,
														const EWorldObjectQuerySnapshotChangeKind RemovedKind,
														const int64 EffectiveTimeMilliseconds)
{
	check(IsInGameThread());
	if (!Runtime || (!Previous.IsSet() && !Current.IsSet()) || (Previous.IsSet() && !Previous->IsValid()) ||
		(Current.IsSet() && !Current->IsValid()))
	{
		return false;
	}
	const bool bOwnTransaction = !Runtime->Core.QuerySnapshots.IsInTransaction();
	if (bOwnTransaction && !Runtime->Core.QuerySnapshots.BeginTransaction())
	{
		return false;
	}
	const FWorldObjectShapeInstanceSnapshot& IdentitySource =
		Current.IsSet() ? Current.GetValue() : Previous.GetValue();
	FWorldObjectQuerySnapshotChange Change;
	Change.Kind = Previous.IsSet() && !Current.IsSet()
		? RemovedKind : (!Previous.IsSet() && Current.IsSet()
			? EWorldObjectQuerySnapshotChangeKind::Upsert : RetainedKind);
	Change.WorldEntityId = IdentitySource.ShapeRef.WorldEntityId;
	Change.Entity = IdentitySource.ShapeRef.Entity;
	Change.StateRevision = IdentitySource.StateRevision;
	Change.EffectiveTimeMilliseconds = EffectiveTimeMilliseconds;
	Change.Previous = Previous;
	Change.Current = Current;
	if (!Runtime->Core.QuerySnapshots.Publish(MakeArrayView(&Change, 1)))
	{
		if (bOwnTransaction) Runtime->Core.QuerySnapshots.CancelTransaction();
		return false;
	}
	return !bOwnTransaction || Runtime->Core.QuerySnapshots.CommitTransaction();
}

bool UWorldObjectWorldSubsystem::CopyQuerySnapshotPage(
	const int32 Offset,
	const int32 MaximumShapes,
	FWorldObjectQuerySnapshotPage& OutPage) const
{
	check(IsInGameThread());
	return Runtime && Runtime->Core.QuerySnapshots.CopyPage(Offset, MaximumShapes, OutPage);
}

bool UWorldObjectWorldSubsystem::CopyEntityShapeSnapshot(const FWorldObjectEntityHandle Entity,
														 FWorldObjectShapeInstanceSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	return BuildShapeSnapshot(Entity, OutSnapshot);
}

void UWorldObjectWorldSubsystem::QueryShapeSnapshots(const FBox& Bounds,
													 TArray<FWorldObjectShapeInstanceSnapshot>& OutShapes) const
{
	check(IsInGameThread());
	OutShapes.Reset();
	if (!Runtime || Bounds.IsValid == 0 || Bounds.ContainsNaN())
		return;
	FWorldObjectSpatialQueryScratch Scratch;
	TArray<FWorldObjectEntityHandle> Entities;
	Runtime->Core.SpatialIndex.QueryOverlaps(Bounds, Scratch, Entities);
	for (const FWorldObjectEntityHandle Entity : Entities)
	{
		FWorldObjectShapeInstanceSnapshot Shape;
		if (BuildShapeSnapshot(Entity, Shape) && Shape.WorldBounds.Intersect(Bounds))
		{
			OutShapes.Add(MoveTemp(Shape));
		}
	}
	OutShapes.Sort(
		[](const FWorldObjectShapeInstanceSnapshot& Left, const FWorldObjectShapeInstanceSnapshot& Right)
		{
			if (Left.ShapeRef.WorldEntityId != Right.ShapeRef.WorldEntityId)
			{
				return Left.ShapeRef.WorldEntityId.GetValue() < Right.ShapeRef.WorldEntityId.GetValue();
			}
			if (Left.ShapeRef.Entity != Right.ShapeRef.Entity)
			{
				return Left.ShapeRef.Entity < Right.ShapeRef.Entity;
			}
			return Left.ShapeRef.ShapeId < Right.ShapeRef.ShapeId;
		});
}

FWorldObjectQuerySnapshotBatchCommittedEvent& UWorldObjectWorldSubsystem::OnQuerySnapshotBatchCommitted()
{
	check(IsInGameThread() && Runtime);
	return Runtime->Core.QuerySnapshots.OnBatchCommitted();
}

UWorldObjectProxyComponent* UWorldObjectWorldSubsystem::GetProxy(const FWorldObjectEntityHandle Entity) const
{
	return Runtime && Runtime->Core.Registry.IsAlive(Entity) &&
				   Runtime->Projection.ProxyBySlot.IsValidIndex(Entity.GetSlot())
			   ? Runtime->Projection.ProxyBySlot[Entity.GetSlot()].Get()
			   : nullptr;
}

FWorldObjectEntityRegistry& UWorldObjectWorldSubsystem::GetRegistry()
{
	check(IsInGameThread() && Runtime);
	return Runtime->Core.Registry;
}

const FWorldObjectEntityRegistry& UWorldObjectWorldSubsystem::GetRegistry() const
{
	check(IsInGameThread() && Runtime);
	return Runtime->Core.Registry;
}

void UWorldObjectWorldSubsystem::QueryOverlap(const FBox& Bounds, FWorldObjectSpatialQueryScratch& Scratch,
											  TArray<FWorldObjectEntityHandle>& OutEntities) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_QueryOverlap);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, QueryOverlap);
	if (!Runtime)
	{
		OutEntities.Reset();
		return;
	}
	Runtime->Core.SpatialIndex.QueryOverlaps(Bounds, Scratch, OutEntities);
}

void UWorldObjectWorldSubsystem::QueryPortableOverlap(const FBox& Bounds, FWorldObjectSpatialQueryScratch& Scratch,
													  TArray<FWorldObjectEntityHandle>& OutEntities) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_QueryPortableOverlap);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, QueryPortableOverlap);
	if (!Runtime)
	{
		OutEntities.Reset();
		return;
	}
	Runtime->Core.SpatialIndex.QueryPortableOverlaps(Bounds, Scratch, OutEntities);
}

void UWorldObjectWorldSubsystem::QueryRay(const FVector& Origin, const FVector& UnitDirection, const double MaxDistance,
										  FWorldObjectSpatialQueryScratch& Scratch,
										  TArray<FWorldObjectSpatialRayHit>& OutHits) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_QueryRay);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, QueryRay);
	if (!Runtime)
	{
		OutHits.Reset();
		return;
	}
	Runtime->Core.SpatialIndex.QueryRay(Origin, UnitDirection, MaxDistance, Scratch, OutHits);
}

void UWorldObjectWorldSubsystem::QueryPortableRay(const FVector& Origin, const FVector& UnitDirection,
												  const double MaxDistance, FWorldObjectSpatialQueryScratch& Scratch,
												  TArray<FWorldObjectSpatialRayHit>& OutHits) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_QueryPortableRay);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, QueryPortableRay);
	if (!Runtime)
	{
		OutHits.Reset();
		return;
	}
	Runtime->Core.SpatialIndex.QueryPortableRay(Origin, UnitDirection, MaxDistance, Scratch, OutHits);
}

FWorldObjectSpatialIndex& UWorldObjectWorldSubsystem::GetSpatialIndex()
{
	check(IsInGameThread() && Runtime);
	return Runtime->Core.SpatialIndex;
}

const FWorldObjectSpatialIndex& UWorldObjectWorldSubsystem::GetSpatialIndex() const
{
	check(IsInGameThread() && Runtime);
	return Runtime->Core.SpatialIndex;
}

FWorldObjectRuntimeStats UWorldObjectWorldSubsystem::GetRuntimeStats() const
{
	FWorldObjectRuntimeStats Stats;
	if (!Runtime)
	{
		return Stats;
	}
	Stats.EntityCount = Runtime->Core.Registry.GetEntityCount();
	Stats.PermanentStaticCount = Runtime->Core.SpatialIndex.GetPermanentStaticCount();
	Stats.PortableCount = Runtime->Core.SpatialIndex.GetPortableCount();
	Stats.ActorActiveCount = Runtime->Projection.ActorActiveEntities.Num();
	Stats.BoundProxyCount = Runtime->Projection.BoundProxyCount;
	Stats.WorldStorageOwnedEntityCount = Runtime->Persistence.OwnedEntities.Num();
	Stats.ActiveCount = Stats.ActorActiveCount;
	Stats.LastSampledActiveCount = Runtime->Projection.LastSampledActiveCount;
	Stats.LastChangedTransformCount = Runtime->Projection.LastChangedTransformCount;
	Stats.StaticBuildCount = static_cast<int64>(Runtime->Core.SpatialIndex.GetStaticBuildCount());
	Stats.StaticLinearChunkCount = Runtime->Core.SpatialIndex.GetStaticLinearChunkCount();
	Stats.StaticBVHChunkCount = Runtime->Core.SpatialIndex.GetStaticBVHChunkCount();
	Stats.DynamicReinsertCount = static_cast<int64>(Runtime->Core.SpatialIndex.GetDynamicReinsertCount());
	const FWorldObjectEntityRegistryStorageStats RegistryStats = Runtime->Core.Registry.GetStorageStats();
	Stats.RegistryAllocatedBytes = static_cast<int64>(RegistryStats.AllocatedBytes);
	Stats.FragmentPoolCount = RegistryStats.FragmentPoolCount;
	Stats.FragmentSparseIndexPageCount = RegistryStats.SparseIndexPageCount;
	Stats.SpatialAllocatedBytes = static_cast<int64>(Runtime->Core.SpatialIndex.GetEstimatedAllocatedSize());
	Stats.EstimatedAllocatedBytes =
		Stats.RegistryAllocatedBytes + Stats.SpatialAllocatedBytes +
		static_cast<int64>(Runtime->Core.QuerySnapshots.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Projection.ActorActiveEntities.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Projection.ActorActiveIndexBySlot.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Projection.ProxyBySlot.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Core.EntityByWorldEntityId.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Core.DefinitionById.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Projection.PendingProxies.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Projection.PendingMotionStates.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Projection.PendingAuthorityDestroys.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Persistence.OwnedEntities.GetAllocatedSize()) +
		static_cast<int64>(Runtime->Persistence.HomeChunks.GetAllocatedSize());
	return Stats;
}

#if WITH_DEV_AUTOMATION_TESTS
bool UWorldObjectWorldSubsystem::CapturePersistentBatchForTesting(const TConstArrayView<FWorldEntityId> EntityIds,
																  TArray<FWorldPersistentEntityRecord>& OutRecords,
																  FString& OutError) const
{
	check(IsInGameThread());
	return Runtime && Runtime->Persistence.Adapter.IsValid() &&
		   Runtime->Persistence.Adapter->CaptureBatch(EntityIds, OutRecords, OutError);
}

bool UWorldObjectWorldSubsystem::RestorePersistentBatchForTesting(
	const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldPersistentEntityRecord> Records, FString& OutError)
{
	check(IsInGameThread());
	return Runtime && Runtime->Persistence.Adapter.IsValid() &&
		   Runtime->Persistence.Adapter->RestoreBatch(HomeChunk, Records, OutError);
}

bool UWorldObjectWorldSubsystem::RuntimeEvictPersistentBatchForTesting(const FWorldChunkCoord& HomeChunk,
																	   const TConstArrayView<FWorldEntityId> EntityIds,
																	   FString& OutError)
{
	check(IsInGameThread());
	return Runtime && Runtime->Persistence.Adapter.IsValid() &&
		   Runtime->Persistence.Adapter->RuntimeEvictBatch(HomeChunk, EntityIds, OutError);
}
#endif
