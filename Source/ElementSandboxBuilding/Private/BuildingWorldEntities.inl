// UBuildingWorldSubsystem：Entity 创建/销毁、Transform/Part/CustomData 原子事务。
// GameplayDestroy、RuntimeEvict、LeaveInterest 与失败回滚保持独立语义入口。

FBuildEntityHandle UBuildingWorldSubsystem::CreateEntity(const UBuildingDefinition& Definition,
														 const FTransform& InitialWorldTransform,
														 const EBuildSpatialMobility Mobility)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || !Definition.HasValidDefinitionId() ||
		InitialWorldTransform.ContainsNaN() || !RegisterDefinition(const_cast<UBuildingDefinition&>(Definition)))
	{
		return {};
	}

	UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	if (!WorldStorage)
	{
		return {};
	}
	const FWorldEntityId WorldEntityId = WorldStorage->AllocateEntityId();
	if (!WorldEntityId.IsSet())
	{
		return {};
	}
	FScopedBuildQuerySnapshotTransaction JournalTransaction(Runtime->Core.QuerySnapshots);
	if (!JournalTransaction.IsValid())
	{
		return {};
	}

	const FBuildEntityHandle Entity = CreateEntityInternal(const_cast<UBuildingDefinition&>(Definition),
														   InitialWorldTransform, Mobility, WorldEntityId, 1, true);
	if (!Entity.IsSet())
	{
		return {};
	}
	const FWorldResidentEntityRegistration Registration{
		WorldEntityId, EWorldEntityDomain::Building,
		FWorldChunkCoord::FromWorldLocation(InitialWorldTransform.GetLocation()), 1};
	if (WorldStorage->RegisterResidentEntity(Registration) != EWorldResidentUpsertResult::Inserted ||
		!WorldStorage->MarkEntityDirty(WorldEntityId, 1))
	{
		verify(DestroyEntityInternal(Entity, ERemovalSemantic::FailedRegistrationRollback));
		return {};
	}
	return JournalTransaction.Finish(true) ? Entity : FBuildEntityHandle();
}

FBuildEntityHandle
UBuildingWorldSubsystem::CreateEntityInternal(UBuildingDefinition& Definition, const FTransform& InitialWorldTransform,
											  const EBuildSpatialMobility Mobility, const FWorldEntityId WorldEntityId,
											  const uint32 StateRevision, const bool bGameplayMutation)
{
	check(IsInGameThread());
	if (!Runtime || !Definition.HasValidDefinitionId() || !WorldEntityId.IsSet() || StateRevision == 0 ||
		Runtime->Core.EntityByWorldEntityId.Contains(WorldEntityId) || InitialWorldTransform.ContainsNaN() ||
		(Mobility != EBuildSpatialMobility::Static && Mobility != EBuildSpatialMobility::Dynamic))
	{
		return {};
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_CreateEntity);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CreateEntity);

	FBuildEntityHandle Entity;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Create_CoreFragments);
		Entity = Definition.CreateEntity(Runtime->Core.Registry, InitialWorldTransform);
	}
	if (!Entity.IsSet())
	{
		return {};
	}
	FBuildWorldIdentityFragment WorldEntityIdentity;
	WorldEntityIdentity.WorldEntityId = WorldEntityId;
	WorldEntityIdentity.StateRevision = StateRevision;
	if (!Runtime->Core.Registry.AddFragment(Entity, WorldEntityIdentity))
	{
		verify(Runtime->Core.Registry.DestroyEntity(Entity));
		return {};
	}

	FBox WorldBounds(ForceInit);
	const FBuildPartTransformFragment* PartTransforms =
		Runtime->Core.Registry.FindFragment<FBuildPartTransformFragment>(Entity);
	const TConstArrayView<FTransform> PartLocalTransforms =
		PartTransforms ? TConstArrayView<FTransform>(PartTransforms->LocalTransforms) : TConstArrayView<FTransform>();
	bool bRegisteredSpatialProjection = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Create_SpatialProjection);
		bRegisteredSpatialProjection =
			Definition.TryCalculateWorldBounds(InitialWorldTransform, PartLocalTransforms, WorldBounds) &&
			Runtime->Core.SpatialIndex.Insert(Entity, WorldBounds, Mobility) &&
			(GetWorldRef().IsNetMode(NM_DedicatedServer) ||
			 (Runtime->Presentation.RenderDirtySet.MarkRebuild(Entity, Mobility == EBuildSpatialMobility::Static)
				 && RequestPresentationProjection()));
	}
	if (!bRegisteredSpatialProjection)
	{
		if (Runtime->Core.SpatialIndex.Contains(Entity))
		{
			verify(Runtime->Core.SpatialIndex.Remove(Entity));
		}
		verify(Runtime->Core.Registry.DestroyEntity(Entity));
		return {};
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Create_CollisionAndIdentity);
		Runtime->Presentation.CollisionProcessor.NotifyEntityCreated(Entity, Runtime->Core.SpatialIndex);
		Runtime->Core.EntityByWorldEntityId.Add(WorldEntityId, Entity);
	}
	TArray<FBuildShapeInstanceSnapshot> Shapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, Shapes);
	if (!PublishBuildShapeTransition(Runtime->Core.QuerySnapshots, {}, Shapes, EBuildQuerySnapshotChangeKind::Upsert,
										 EBuildQuerySnapshotChangeKind::ShapeRemove, StateRevision,
										 GetBuildHostEffectiveTimeMilliseconds(*Runtime, GetWorldRef())))
	{
		Runtime->Core.EntityByWorldEntityId.Remove(WorldEntityId);
		Runtime->Presentation.CollisionProcessor.NotifyEntityDestroyed(Entity);
		verify(Runtime->Core.SpatialIndex.Remove(Entity));
		verify(Runtime->Core.Registry.DestroyEntity(Entity));
		return {};
	}
	if (FBuildDefinitionEntityUpsertedEvent* Event =
			Runtime->Core.DefinitionEntityUpsertedEvents.Find(Definition.DefinitionId))
	{
		Event->Broadcast(Entity);
	}

	return Entity;
}

bool UBuildingWorldSubsystem::DestroyEntity(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client)
	{
		return false;
	}
	FScopedBuildQuerySnapshotTransaction JournalTransaction(Runtime->Core.QuerySnapshots);
	return JournalTransaction.IsValid() &&
		   JournalTransaction.Finish(DestroyEntityInternal(Entity, ERemovalSemantic::GameplayDestroy));
}

bool UBuildingWorldSubsystem::BeginGameplayDestructionBatch()
{
	check(IsInGameThread());
	return Runtime && GetWorldRef().GetNetMode() != NM_Client
		&& !Runtime->Core.QuerySnapshots.IsInTransaction()
		&& Runtime->Core.QuerySnapshots.BeginTransaction();
}

bool UBuildingWorldSubsystem::EndGameplayDestructionBatch(const bool bCommit)
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->Core.QuerySnapshots.IsInTransaction())
	{
		return false;
	}
	if (!bCommit)
	{
		Runtime->Core.QuerySnapshots.CancelTransaction();
		return true;
	}
	return Runtime->Core.QuerySnapshots.CommitTransaction();
}

bool UBuildingWorldSubsystem::DestroyEntityInternal(const FBuildEntityHandle Entity, const ERemovalSemantic Semantic)
{
	check(IsInGameThread());
	check(Runtime);
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_DestroyEntity);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, DestroyEntity);
	if (!Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	const bool bGameplayDestroy = Semantic == ERemovalSemantic::GameplayDestroy;
	bool bCanDestroy = true;
	if (bGameplayDestroy)
	{
		EntityPreDestroyEvent.Broadcast(Entity, bCanDestroy);
	}
	if (!bCanDestroy || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	const FBuildWorldIdentityFragment* WorldEntityIdentity =
		Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
	if (!WorldEntityIdentity || !WorldEntityIdentity->WorldEntityId.IsSet())
	{
		return false;
	}
	const FWorldEntityId WorldEntityId = WorldEntityIdentity->WorldEntityId;
	const uint32 RemovedRevision =
		bGameplayDestroy ? NextBuildRevision(WorldEntityIdentity->StateRevision) : WorldEntityIdentity->StateRevision;
	TArray<FBuildShapeInstanceSnapshot> RemovedShapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, RemovedShapes);
	if (bGameplayDestroy && GetWorldRef().GetNetMode() != NM_Client)
	{
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		if (!WorldStorage || !WorldStorage->GameplayDestroy(WorldEntityId, RemovedRevision))
		{
			return false;
		}
	}

	if (!CommitPrevalidatedEntityRemoval(Entity, WorldEntityId, Semantic, false))
	{
		return false;
	}
	EBuildQuerySnapshotChangeKind RemovalKind = EBuildQuerySnapshotChangeKind::RuntimeEvict;
	switch (Semantic)
	{
	case ERemovalSemantic::GameplayDestroy:
		RemovalKind = EBuildQuerySnapshotChangeKind::GameplayDestroy;
		break;
	case ERemovalSemantic::RuntimeEvict:
		RemovalKind = EBuildQuerySnapshotChangeKind::RuntimeEvict;
		break;
	case ERemovalSemantic::LeaveInterest:
		RemovalKind = EBuildQuerySnapshotChangeKind::LeaveInterest;
		break;
	case ERemovalSemantic::FailedRegistrationRollback:
		RemovalKind = EBuildQuerySnapshotChangeKind::FailedRegistrationRollback;
		break;
	default:
		checkNoEntry();
		return false;
	}
	const bool bJournalPublished =
			PublishBuildShapeTransition(Runtime->Core.QuerySnapshots, RemovedShapes, {}, RemovalKind, RemovalKind,
									RemovedRevision, GetBuildHostEffectiveTimeMilliseconds(*Runtime, GetWorldRef()));
	return bJournalPublished;
}

bool UBuildingWorldSubsystem::CommitPrevalidatedEntityRemoval(
	const FBuildEntityHandle Entity,
	const FWorldEntityId WorldEntityId,
	const ERemovalSemantic Semantic,
	const bool bPresentationProjectionAlreadyRequested)
{
	check(IsInGameThread());
	check(Runtime);
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_CommitPrevalidatedEntityRemoval);
	if (!Runtime->Core.Registry.IsAlive(Entity) || !WorldEntityId.IsSet())
	{
		return false;
	}
	if (Runtime->Core.SpatialIndex.Contains(Entity) && !Runtime->Core.SpatialIndex.Remove(Entity))
	{
		return false;
	}
	if (!Runtime->Core.Registry.DestroyEntity(Entity))
	{
		return false;
	}
	Runtime->Core.EntityByWorldEntityId.Remove(WorldEntityId);
	Runtime->Presentation.CollisionProcessor.NotifyEntityDestroyed(Entity);
	EntityLocalRemovedEvent.Broadcast(Entity);

	const bool bRenderRemovalQueued = GetWorldRef().IsNetMode(NM_DedicatedServer)
		|| (Runtime->Presentation.RenderDirtySet.MarkRebuild(Entity)
			&& (bPresentationProjectionAlreadyRequested || RequestPresentationProjection()));
	if (Semantic == ERemovalSemantic::GameplayDestroy)
	{
		EntityDestroyedEvent.Broadcast(WorldEntityId);
	}
	return bRenderRemovalQueued;
}

	#if WITH_DEV_AUTOMATION_TESTS
bool UBuildingWorldSubsystem::CapturePersistentBatchForTesting(const TConstArrayView<FWorldEntityId> EntityIds,
															   TArray<FWorldPersistentEntityRecord>& OutRecords,
															   FString& OutError) const
{
	check(IsInGameThread());
	return Runtime && Runtime->Persistence.Adapter.IsValid() &&
		   Runtime->Persistence.Adapter->CaptureBatch(EntityIds, OutRecords, OutError);
}

bool UBuildingWorldSubsystem::RestorePersistentBatchForTesting(
	const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldPersistentEntityRecord> Records, FString& OutError)
{
	check(IsInGameThread());
	return Runtime && Runtime->Persistence.Adapter.IsValid() &&
		   Runtime->Persistence.Adapter->RestoreBatch(HomeChunk, Records, OutError);
}

bool UBuildingWorldSubsystem::RuntimeEvictPersistentBatchForTesting(const FWorldChunkCoord& HomeChunk,
																	const TConstArrayView<FWorldEntityId> EntityIds,
																	FString& OutError)
{
	check(IsInGameThread());
	return Runtime && Runtime->Persistence.Adapter.IsValid() &&
		   Runtime->Persistence.Adapter->RuntimeEvictBatch(HomeChunk, EntityIds, OutError);
}
#endif

bool UBuildingWorldSubsystem::CommitEntityTransformChange(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client)
	{
		return false;
	}
	FScopedBuildQuerySnapshotTransaction JournalTransaction(Runtime->Core.QuerySnapshots);
	return JournalTransaction.IsValid() && JournalTransaction.Finish(CommitEntityTransformChangeInternal(Entity, true));
}

bool UBuildingWorldSubsystem::CommitEntityTransformChangeInternal(const FBuildEntityHandle Entity,
																  const bool bGameplayMutation)
{
	check(IsInGameThread());
	check(Runtime);
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_CommitEntityTransform);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CommitEntityTransform);
	if (!Runtime->Core.Registry.IsAlive(Entity) || !Runtime->Core.SpatialIndex.Contains(Entity))
	{
		return false;
	}

	FBuildTransformFragment* Transform = Runtime->Core.Registry.FindMutableFragment<FBuildTransformFragment>(Entity);
	const FBuildDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const FBuildPartTransformFragment* PartTransforms =
		Runtime->Core.Registry.FindFragment<FBuildPartTransformFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	FBuildWorldIdentityFragment* WorldEntityIdentity =
		Runtime->Core.Registry.FindMutableFragment<FBuildWorldIdentityFragment>(Entity);
	if (!Transform || !Definition || !WorldEntityIdentity || Transform->Revision == 0 ||
		WorldEntityIdentity->StateRevision == 0)
	{
		return false;
	}
	if (Transform->WorldTransform.Equals(Transform->CommittedWorldTransform, 0.01))
	{
		return true;
	}

	FBox PreviousWorldBounds(ForceInit);
	if (!Runtime->Core.SpatialIndex.TryGetBounds(Entity, PreviousWorldBounds))
	{
		return false;
	}
	TArray<FBuildShapeInstanceSnapshot> OldShapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, OldShapes);

	const TConstArrayView<FTransform> PartLocalTransforms =
		PartTransforms ? TConstArrayView<FTransform>(PartTransforms->LocalTransforms) : TConstArrayView<FTransform>();
	FBox WorldBounds(ForceInit);
	if (!Definition->TryCalculateWorldBounds(Transform->WorldTransform, PartLocalTransforms, WorldBounds) ||
		!Runtime->Core.SpatialIndex.Update(Entity, WorldBounds))
	{
		return false;
	}
	Runtime->Presentation.CollisionProcessor.NotifyEntityTransformChanged(Entity, Runtime->Core.SpatialIndex);

	const bool bRenderCommitted =
		GetWorldRef().IsNetMode(NM_DedicatedServer)
		|| (Runtime->Presentation.RenderDirtySet.MarkAllPartsDirty(Entity)
			&& RequestPresentationProjection());
	if (!bRenderCommitted)
	{
		return false;
	}
	if (!GetWorldRef().IsNetMode(NM_DedicatedServer) &&
		Runtime->Presentation.RenderProcessor.IsPresentationMotionActive(Entity) && !ResolveRenderChanges())
	{
		return false;
	}
	if (bGameplayMutation)
	{
		WorldEntityIdentity->StateRevision = NextBuildRevision(WorldEntityIdentity->StateRevision);
		if (UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get())
		{
			WorldStorage->UpdateEntityLocation(WorldEntityIdentity->WorldEntityId,
											   Transform->WorldTransform.GetLocation(),
											   WorldEntityIdentity->StateRevision);
		}
	}
	Transform->CommittedWorldTransform = Transform->WorldTransform;
	Transform->Revision = NextBuildTransformRevision(Transform->Revision);
	TArray<FBuildShapeInstanceSnapshot> NewShapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, NewShapes);
	return PublishBuildShapeTransition(Runtime->Core.QuerySnapshots, OldShapes, NewShapes, EBuildQuerySnapshotChangeKind::Motion,
									   EBuildQuerySnapshotChangeKind::ShapeRemove, WorldEntityIdentity->StateRevision,
									   GetBuildHostEffectiveTimeMilliseconds(*Runtime, GetWorldRef()));
}

bool UBuildingWorldSubsystem::CommitPartTransformChange(const FBuildEntityHandle Entity,
														const TConstArrayView<int32> PartIds)
{
	check(IsInGameThread());
	check(Runtime);
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_CommitPartTransform);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CommitPartTransform);
	if (!Runtime->Core.Registry.IsAlive(Entity) || !Runtime->Core.SpatialIndex.Contains(Entity) || PartIds.IsEmpty())
	{
		return false;
	}
	FScopedBuildQuerySnapshotTransaction JournalTransaction(Runtime->Core.QuerySnapshots);
	if (!JournalTransaction.IsValid())
	{
		return false;
	}

	const FBuildTransformFragment* Transform = Runtime->Core.Registry.FindFragment<FBuildTransformFragment>(Entity);
	const FBuildDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	FBuildPartTransformFragment* PartTransforms =
		Runtime->Core.Registry.FindMutableFragment<FBuildPartTransformFragment>(Entity);
	FBuildWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindMutableFragment<FBuildWorldIdentityFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	if (!Transform || !Definition || !Identity || !PartTransforms || Transform->Revision == 0 ||
		Identity->StateRevision == 0)
	{
		return false;
	}
	if (PartTransforms->LocalTransforms.Num() != Definition->MeshParts.Num())
	{
		return false;
	}
	if (PartTransforms->CommittedLocalTransforms.Num() != Definition->MeshParts.Num())
	{
		PartTransforms->CommittedLocalTransforms.Reset(Definition->MeshParts.Num());
		for (const FBuildMeshPartDefinition& Part : Definition->MeshParts)
		{
			PartTransforms->CommittedLocalTransforms.Add(Part.LocalTransform);
		}
	}
	if (PartTransforms->Revisions.Num() != Definition->MeshParts.Num())
	{
		PartTransforms->Revisions.Init(1, Definition->MeshParts.Num());
	}

	TArray<int32> ChangedPartIds;
	for (const int32 PartId : PartIds)
	{
		if (!Definition->MeshParts.IsValidIndex(PartId))
		{
			return false;
		}
		if (!ChangedPartIds.Contains(PartId) &&
			!PartTransforms->LocalTransforms[PartId].Equals(PartTransforms->CommittedLocalTransforms[PartId], 0.01))
		{
			ChangedPartIds.Add(PartId);
		}
	}
	if (ChangedPartIds.IsEmpty())
	{
		return JournalTransaction.Finish(true);
	}
	ChangedPartIds.Sort();

	FBox PreviousWorldBounds(ForceInit);
	if (!Runtime->Core.SpatialIndex.TryGetBounds(Entity, PreviousWorldBounds))
	{
		return false;
	}
	TArray<FBuildShapeInstanceSnapshot> OldShapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, ChangedPartIds, OldShapes);

	if (Definition->DoPartTransformChangesAffectSpatialBounds(ChangedPartIds))
	{
		FBox WorldBounds(ForceInit);
		if (!Definition->TryCalculateWorldBounds(Transform->WorldTransform, PartTransforms->LocalTransforms,
												 WorldBounds) ||
			!Runtime->Core.SpatialIndex.Update(Entity, WorldBounds))
		{
			return false;
		}
	}

	Runtime->Presentation.CollisionProcessor.NotifyPartTransformsChanged(Entity, ChangedPartIds, Runtime->Core.Registry,
																		 Runtime->Core.SpatialIndex);

	const bool bRenderCommitted = GetWorldRef().IsNetMode(NM_DedicatedServer)
		|| (Runtime->Presentation.RenderDirtySet.MarkPartsDirty(Entity, ChangedPartIds)
			&& RequestPresentationProjection());
	if (!bRenderCommitted)
	{
		return false;
	}
	if (!GetWorldRef().IsNetMode(NM_DedicatedServer) &&
		Runtime->Presentation.RenderProcessor.IsPresentationMotionActive(Entity) && !ResolveRenderChanges())
	{
		return false;
	}
	Identity->StateRevision = NextBuildRevision(Identity->StateRevision);
	if (UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get())
	{
		WorldStorage->MarkEntityDirty(Identity->WorldEntityId, Identity->StateRevision);
	}
	for (const int32 PartId : ChangedPartIds)
	{
		PartTransforms->CommittedLocalTransforms[PartId] = PartTransforms->LocalTransforms[PartId];
		PartTransforms->Revisions[PartId] = NextBuildTransformRevision(PartTransforms->Revisions[PartId]);
	}
	FBox CurrentWorldBounds(ForceInit);
	if (!Runtime->Core.SpatialIndex.TryGetBounds(Entity, CurrentWorldBounds))
	{
		return false;
	}
	TArray<FBuildShapeInstanceSnapshot> NewShapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, ChangedPartIds, NewShapes);
	return JournalTransaction.Finish(
		PublishBuildShapeTransition(Runtime->Core.QuerySnapshots, OldShapes, NewShapes, EBuildQuerySnapshotChangeKind::Motion,
									EBuildQuerySnapshotChangeKind::ShapeRemove, Identity->StateRevision,
									GetBuildHostEffectiveTimeMilliseconds(*Runtime, GetWorldRef())));
}

bool UBuildingWorldSubsystem::CommitRenderCustomDataChange(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	check(Runtime);
	if (!Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	if (GetWorldRef().IsNetMode(NM_DedicatedServer))
	{
		return true;
	}
	if (!Runtime->Presentation.RenderCustomDataDirtySet.Contains(Entity))
	{
		Runtime->Presentation.RenderCustomDataDirtySet.Add(Entity);
		Runtime->Presentation.RenderCustomDataDirtyEntities.Add(Entity);
	}
	return RequestPresentationProjection()
		&& (!Runtime->Presentation.RenderProcessor.IsPresentationMotionActive(Entity) || ResolveRenderChanges());
}
