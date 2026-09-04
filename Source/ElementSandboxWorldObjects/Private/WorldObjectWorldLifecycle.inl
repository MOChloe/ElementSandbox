// UWorldObjectWorldSubsystem：依赖装配、Definition/Extension 注册及 Entity/Motion 生命周期。
// Registry、空间索引、WorldStorage 所有权与中性查询快照在同一事务中更新。

UWorldObjectWorldSubsystem::UWorldObjectWorldSubsystem() = default;
UWorldObjectWorldSubsystem::~UWorldObjectWorldSubsystem() = default;

void UWorldObjectWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldStorageSubsystem>();
	check(!Runtime);
	Runtime = MakePimpl<FWorldObjectWorldRuntime>();
	Runtime->Persistence.WorldStorage = GetWorldRef().GetSubsystem<UWorldStorageSubsystem>();
	if (UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get())
	{
		const TSharedRef<IWorldStorageDomainAdapter> Adapter = MakeWorldObjectWorldStorageAdapter(*this);
		const bool bRegistered =
			WorldStorage->RegisterDomainAdapter(Adapter) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
													  *FWorldObjectTransformFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
													  *FWorldObjectDefinitionFragment::StaticStruct(),
													  EWorldFragmentPersistence::Derived) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
													  *FWorldObjectMotionFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
													  *FWorldObjectWorldIdentityFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
													  *FWorldObjectInstanceInteractionBoundsFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
													  *FWorldObjectInstanceShapeFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
				WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
												  *FWorldObjectPhysicsBodyFragment::StaticStruct(),
												  EWorldFragmentPersistence::Persistent) &&
				WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::WorldObject,
												  *FWorldObjectDamageFragment::StaticStruct(),
												  EWorldFragmentPersistence::RuntimeOnly);
		if (bRegistered)
		{
			Runtime->Persistence.Adapter = Adapter;
		}
		else
		{
			UE_LOG(LogElementSandboxWorldObjects, Error,
				   TEXT("WorldObject WorldStorage Adapter 或 Fragment 分类注册失败。"));
		}
	}
	Runtime->Projection.PostActorTickHandle =
		FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UWorldObjectWorldSubsystem::HandleWorldPostActorTick);
}

void UWorldObjectWorldSubsystem::PostInitialize()
{
	Super::PostInitialize();
}

void UWorldObjectWorldSubsystem::Deinitialize()
{
	if (Runtime && Runtime->Projection.PostActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(Runtime->Projection.PostActorTickHandle);
		Runtime->Projection.PostActorTickHandle.Reset();
	}
	if (Runtime)
	{
		if (UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
			WorldStorage && Runtime->Persistence.Adapter.IsValid())
		{
			WorldStorage->UnregisterDomainAdapter(EWorldEntityDomain::WorldObject, *Runtime->Persistence.Adapter);
		}
		Runtime->Persistence.Adapter.Reset();
		Runtime->Persistence.Extensions.Reset();
	}
	Runtime.Reset();
	Super::Deinitialize();
}

FWorldObjectEntityHandle UWorldObjectWorldSubsystem::CreateEntity(const FWorldObjectCreateDesc& Desc)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_CreateEntity);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, CreateEntity);
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || !IsValid(Desc.Definition) ||
		(IsActorActiveState(Desc.MotionState) && !IsValid(Desc.Proxy) && !Desc.PhysicsBody.IsSet()) ||
			(Desc.MotionState == EWorldObjectMotionState::Physics && Desc.PhysicsBody.IsSet() &&
			 (!Desc.PhysicsBody->IsValid() || IsValid(Desc.Proxy))) ||
			(Desc.PhysicsBody.IsSet() && !Desc.PhysicsBody->IsValid()) ||
			(Desc.InstanceShapeGeometry.IsSet() &&
		 (!Desc.InstanceShapeGeometry->IsValid() || Desc.InstanceShapeRevision == 0)) ||
		(!Desc.InstanceShapeGeometry.IsSet() && Desc.InstanceShapeRevision != 1) ||
		(Desc.PhysicsBody.IsSet() && Desc.MotionState != EWorldObjectMotionState::Physics))
	{
		return {};
	}
	if (!RegisterDefinition(*Desc.Definition))
	{
		return {};
	}

	UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	if (!WorldStorage)
	{
		return {};
	}
	const FWorldEntityId WorldEntityId = Desc.ReservedWorldEntityId.IsSet()
		? Desc.ReservedWorldEntityId : WorldStorage->AllocateEntityId();
	if (!WorldEntityId.IsSet() || WorldStorage->IsResident(WorldEntityId)
		|| Runtime->Core.EntityByWorldEntityId.Contains(WorldEntityId))
	{
		return {};
	}

	AWorldObjectPhysicsProxyActor* SpawnedPhysicsProxy = nullptr;
	UWorldObjectProxyComponent* Proxy = Desc.Proxy;
	if (Desc.PhysicsBody.IsSet() && !Proxy)
	{
		if (Desc.MotionState != EWorldObjectMotionState::Physics || !Desc.InstanceInteractionBounds.IsSet() ||
			Desc.InstanceInteractionBounds->IsValid == 0)
		{
			return {};
		}
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnedPhysicsProxy = GetWorldRef().SpawnActor<AWorldObjectPhysicsProxyActor>(SpawnParameters);
		if (!IsValid(SpawnedPhysicsProxy))
		{
			return {};
		}
		SpawnedPhysicsProxy->SetActorTransform(AWorldObjectPhysicsProxyActor::MakeActorTransform(
												   Desc.WorldTransform, Desc.InstanceInteractionBounds.GetValue()),
											   false, nullptr, ETeleportType::TeleportPhysics);
			if (!SpawnedPhysicsProxy->ConfigurePhysics(Desc.InstanceInteractionBounds.GetValue(), Desc.PhysicsBody->MassKg,
													   Desc.PhysicsBody->CollisionPolicy, Desc.PhysicsBody->LinearVelocity,
												   Desc.PhysicsBody->AngularVelocityDegrees, 1))
		{
			SpawnedPhysicsProxy->Destroy();
			return {};
		}
		Proxy = SpawnedPhysicsProxy->GetWorldObjectProxyComponent();
	}

	const FWorldObjectEntityHandle Entity =
		CreateEntityInternal(*Desc.Definition, Desc.WorldTransform, Desc.MotionState, WorldEntityId, 1, Proxy,
							 Desc.InstanceInteractionBounds, Desc.InstanceShapeGeometry, Desc.InstanceShapeRevision,
							 Desc.PhysicsBody.IsSet() ? &Desc.PhysicsBody.GetValue() : nullptr,
							 Desc.MotionState != EWorldObjectMotionState::Attached, false, false);
	if (!Entity.IsSet() && IsValid(SpawnedPhysicsProxy))
	{
		SpawnedPhysicsProxy->Destroy();
	}
	if (!Entity.IsSet())
	{
		return {};
	}
	if (Desc.MotionState != EWorldObjectMotionState::Attached)
	{
		const FWorldResidentEntityRegistration Registration{
			WorldEntityId, EWorldEntityDomain::WorldObject,
			FWorldChunkCoord::FromWorldLocation(Desc.WorldTransform.GetLocation()), 1};
		if (WorldStorage->RegisterResidentEntity(Registration) != EWorldResidentUpsertResult::Inserted)
		{
			verify(DestroyEntityInternal(Entity, ERemovalSemantic::FailedRegistrationRollback, true, false, false));
			return {};
		}
		// MarkEntityDirty 会同步广播 Authority Mutation，网络层会在回调中立刻 Capture。
		// 因此持久化所有权和 HomeChunk 必须先对 Adapter 可见。
		Runtime->Persistence.OwnedEntities.Add(WorldEntityId);
		Runtime->Persistence.HomeChunks.Add(WorldEntityId, Registration.HomeChunk);
		if (!WorldStorage->MarkEntityDirty(WorldEntityId, 1))
		{
			Runtime->Persistence.OwnedEntities.Remove(WorldEntityId);
			Runtime->Persistence.HomeChunks.Remove(WorldEntityId);
			verify(DestroyEntityInternal(Entity, ERemovalSemantic::FailedRegistrationRollback, true, false, false));
			verify(WorldStorage->RollbackUnpublishedResidentRegistration(Registration));
			return {};
		}
	}
	FWorldObjectLifecycleRecord Lifecycle;
	if (BuildLifecycleRecord(Entity, Lifecycle))
	{
		EntitiesUpsertedEvent.Broadcast(MakeArrayView(&Lifecycle, 1));
	}
	FWorldObjectShapeInstanceSnapshot Shape;
	if (BuildShapeSnapshot(Entity, Shape))
	{
		verify(PublishShapeTransition({}, Shape, EWorldObjectQuerySnapshotChangeKind::Upsert,
										  EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
										  GetEffectiveTimeMilliseconds(GetWorldRef())));
	}
	if (SpawnedPhysicsProxy)
	{
		ScheduleAutomaticPhysicsRelease(Entity);
	}
	return Entity;
}

bool UWorldObjectWorldSubsystem::StageCreateEntities(
	const TConstArrayView<FWorldObjectCreateDesc> Descs,
	FWorldObjectStagedCreateBatch& OutBatch)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || Descs.IsEmpty()
		|| OutBatch.IsPrepared() || !OutBatch.Entities.IsEmpty())
	{
		return false;
	}
	UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	if (!WorldStorage || !WorldStorage->IsAuthorityStorage())
	{
		return false;
	}

	OutBatch.Reset();
	OutBatch.bPrepared = true;
	OutBatch.Entities.Reserve(Descs.Num());
	OutBatch.EntityIds.Reserve(Descs.Num());
	int32 InstanceBoundsCount = 0;
	int32 InstanceShapeCount = 0;
	for (const FWorldObjectCreateDesc& Desc : Descs)
	{
		InstanceBoundsCount += Desc.InstanceInteractionBounds.IsSet() ? 1 : 0;
		InstanceShapeCount += Desc.InstanceShapeGeometry.IsSet() ? 1 : 0;
	}
	// 跨域批次必须先扩容连续 Registry/目录。否则某个随机 Entity 恰逢 TArray/TMap
	// 扩容时会让墙钟预算无法在该单项中途抢占，形成可见长帧。
	FWorldObjectEntityRegistry& Registry = Runtime->Core.Registry;
	Registry.ReserveEntities(Descs.Num());
	Registry.ReserveFragments<FWorldObjectDefinitionFragment>(Descs.Num());
	Registry.ReserveFragments<FWorldObjectTransformFragment>(Descs.Num());
	Registry.ReserveFragments<FWorldObjectMotionFragment>(Descs.Num());
	Registry.ReserveFragments<FWorldObjectWorldIdentityFragment>(Descs.Num());
	Registry.ReserveFragments<FWorldObjectInstanceInteractionBoundsFragment>(InstanceBoundsCount);
	Registry.ReserveFragments<FWorldObjectInstanceShapeFragment>(InstanceShapeCount);
	// 产品 Definition 可以在 Dormant 创建时也装配 Physics 能力；保守预留比中途扩容稳定。
	Registry.ReserveFragments<FWorldObjectPhysicsBodyFragment>(Descs.Num());
	Runtime->Core.EntityByWorldEntityId.Reserve(
		Runtime->Core.EntityByWorldEntityId.Num() + Descs.Num());
	Runtime->Projection.ActorActiveIndexBySlot.Reserve(
		Runtime->Projection.ActorActiveIndexBySlot.Num() + Descs.Num());
	Runtime->Projection.ProxyBySlot.Reserve(
		Runtime->Projection.ProxyBySlot.Num() + Descs.Num());
	Runtime->Persistence.OwnedEntities.Reserve(
		Runtime->Persistence.OwnedEntities.Num() + Descs.Num());
	Runtime->Persistence.HomeChunks.Reserve(
		Runtime->Persistence.HomeChunks.Num() + Descs.Num());
	for (const FWorldObjectCreateDesc& Desc : Descs)
	{
		if (!IsValid(Desc.Definition) || Desc.MotionState == EWorldObjectMotionState::Attached
			|| (IsActorActiveState(Desc.MotionState) && !IsValid(Desc.Proxy) && !Desc.PhysicsBody.IsSet())
			|| (Desc.MotionState == EWorldObjectMotionState::Physics && Desc.PhysicsBody.IsSet()
				&& (!Desc.PhysicsBody->IsValid() || IsValid(Desc.Proxy)))
			|| (Desc.PhysicsBody.IsSet() && (!Desc.PhysicsBody->IsValid()
				|| Desc.MotionState != EWorldObjectMotionState::Physics))
			|| (Desc.InstanceShapeGeometry.IsSet()
				&& (!Desc.InstanceShapeGeometry->IsValid() || Desc.InstanceShapeRevision == 0))
			|| (!Desc.InstanceShapeGeometry.IsSet() && Desc.InstanceShapeRevision != 1)
			|| !RegisterDefinition(*Desc.Definition))
		{
			RollbackStagedCreateEntities(OutBatch);
			return false;
		}

		const FWorldEntityId WorldEntityId = Desc.ReservedWorldEntityId.IsSet()
			? Desc.ReservedWorldEntityId : WorldStorage->AllocateEntityId();
		if (!WorldEntityId.IsSet() || WorldStorage->IsResident(WorldEntityId)
			|| Runtime->Core.EntityByWorldEntityId.Contains(WorldEntityId))
		{
			RollbackStagedCreateEntities(OutBatch);
			return false;
		}

		AWorldObjectPhysicsProxyActor* SpawnedPhysicsProxy = nullptr;
		UWorldObjectProxyComponent* Proxy = Desc.Proxy;
		if (Desc.PhysicsBody.IsSet() && !Proxy)
		{
			if (!Desc.InstanceInteractionBounds.IsSet()
				|| Desc.InstanceInteractionBounds->IsValid == 0)
			{
				RollbackStagedCreateEntities(OutBatch);
				return false;
			}
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags |= RF_Transient;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnedPhysicsProxy =
				GetWorldRef().SpawnActor<AWorldObjectPhysicsProxyActor>(SpawnParameters);
			if (!IsValid(SpawnedPhysicsProxy))
			{
				RollbackStagedCreateEntities(OutBatch);
				return false;
			}
			SpawnedPhysicsProxy->SetActorTransform(
				AWorldObjectPhysicsProxyActor::MakeActorTransform(
					Desc.WorldTransform, Desc.InstanceInteractionBounds.GetValue()),
				false, nullptr, ETeleportType::TeleportPhysics);
			if (!SpawnedPhysicsProxy->ConfigurePhysics(
					Desc.InstanceInteractionBounds.GetValue(),
					Desc.PhysicsBody->MassKg,
					Desc.PhysicsBody->CollisionPolicy,
					Desc.PhysicsBody->LinearVelocity,
					Desc.PhysicsBody->AngularVelocityDegrees, 1))
			{
				SpawnedPhysicsProxy->Destroy();
				RollbackStagedCreateEntities(OutBatch);
				return false;
			}
			Proxy = SpawnedPhysicsProxy->GetWorldObjectProxyComponent();
		}

		const FWorldObjectEntityHandle Entity = CreateEntityInternal(
			*Desc.Definition,
			Desc.WorldTransform,
			Desc.MotionState,
			WorldEntityId,
			1,
			Proxy,
			Desc.InstanceInteractionBounds,
			Desc.InstanceShapeGeometry,
			Desc.InstanceShapeRevision,
			Desc.PhysicsBody.IsSet() ? &Desc.PhysicsBody.GetValue() : nullptr,
			false,
			false,
			false);
		if (!Entity.IsSet())
		{
			if (IsValid(SpawnedPhysicsProxy))
			{
				SpawnedPhysicsProxy->Destroy();
			}
			RollbackStagedCreateEntities(OutBatch);
			return false;
		}

		const FWorldResidentEntityRegistration Registration{
			WorldEntityId,
			EWorldEntityDomain::WorldObject,
			FWorldChunkCoord::FromWorldLocation(Desc.WorldTransform.GetLocation()),
			1};
		if (WorldStorage->RegisterResidentEntity(Registration)
			!= EWorldResidentUpsertResult::Inserted)
		{
			verify(DestroyEntityInternal(
				Entity, ERemovalSemantic::FailedRegistrationRollback, true, false, false));
			RollbackStagedCreateEntities(OutBatch);
			return false;
		}
		OutBatch.Entities.Add(Entity);
		OutBatch.EntityIds.Add(WorldEntityId);
	}
	return true;
}

bool UWorldObjectWorldSubsystem::CommitStagedCreateEntities(
	FWorldObjectStagedCreateBatch& Batch,
	TArray<FWorldObjectEntityHandle>& OutEntities)
{
	check(IsInGameThread());
	OutEntities.Reset();
	UWorldStorageSubsystem* WorldStorage = Runtime ? Runtime->Persistence.WorldStorage.Get() : nullptr;
	if (!Runtime || !WorldStorage || !Batch.IsPrepared()
		|| Batch.Entities.Num() != Batch.EntityIds.Num())
	{
		return false;
	}

	TArray<FWorldObjectLifecycleRecord> LifecycleRecords;
	TArray<FWorldObjectShapeInstanceSnapshot> Shapes;
	TArray<FWorldChunkCoord> HomeChunks;
	LifecycleRecords.Reserve(Batch.Entities.Num());
	Shapes.Reserve(Batch.Entities.Num());
	HomeChunks.Reserve(Batch.Entities.Num());
	for (int32 Index = 0; Index < Batch.Entities.Num(); ++Index)
	{
		const FWorldObjectEntityHandle Entity = Batch.Entities[Index];
		const FWorldEntityId EntityId = Batch.EntityIds[Index];
		const FWorldObjectTransformFragment* Transform =
			Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
		const FWorldObjectWorldIdentityFragment* Identity =
			Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
		FWorldObjectLifecycleRecord Lifecycle;
		FWorldObjectShapeInstanceSnapshot Shape;
		if (!Transform || !Identity || Identity->WorldEntityId != EntityId
			|| Identity->StateRevision != 1 || Runtime->Persistence.OwnedEntities.Contains(EntityId)
			|| !WorldStorage->IsResident(EntityId)
			|| !BuildLifecycleRecord(Entity, Lifecycle)
			|| !BuildShapeSnapshot(Entity, Shape))
		{
			return false;
		}
		LifecycleRecords.Add(MoveTemp(Lifecycle));
		Shapes.Add(MoveTemp(Shape));
		HomeChunks.Add(FWorldChunkCoord::FromWorldLocation(Transform->WorldTransform.GetLocation()));
	}

	// 所有可失败的 ECS/Shape 预检均已完成。此后只发布已经登记的 Revision=1 Resident。
	for (int32 Index = 0; Index < Batch.EntityIds.Num(); ++Index)
	{
		const FWorldEntityId EntityId = Batch.EntityIds[Index];
		Runtime->Persistence.OwnedEntities.Add(EntityId);
		Runtime->Persistence.HomeChunks.Add(EntityId, HomeChunks[Index]);
		// Authority Mutation 的同步 Capture 必须观察到完整的 WorldStorage 所有权。
		if (!WorldStorage->MarkEntityDirty(EntityId, 1))
		{
			checkNoEntry();
			return false;
		}
	}
	EntitiesUpsertedEvent.Broadcast(LifecycleRecords);

	check(!Runtime->Core.QuerySnapshots.IsInTransaction());
	check(Runtime->Core.QuerySnapshots.BeginTransaction());
	const int64 EffectiveTime = GetEffectiveTimeMilliseconds(GetWorldRef());
	for (const FWorldObjectShapeInstanceSnapshot& Shape : Shapes)
	{
		check(PublishShapeTransition(
			{}, Shape,
			EWorldObjectQuerySnapshotChangeKind::Upsert,
			EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
			EffectiveTime));
	}
	check(Runtime->Core.QuerySnapshots.CommitTransaction());
	for (const FWorldObjectEntityHandle Entity : Batch.Entities)
	{
		ScheduleAutomaticPhysicsRelease(Entity);
	}

	OutEntities = Batch.Entities;
	Batch.Reset();
	return true;
}

void UWorldObjectWorldSubsystem::RollbackStagedCreateEntities(
	FWorldObjectStagedCreateBatch& Batch)
{
	check(IsInGameThread());
	UWorldStorageSubsystem* WorldStorage = Runtime ? Runtime->Persistence.WorldStorage.Get() : nullptr;
	for (int32 Index = Batch.Entities.Num() - 1; Index >= 0; --Index)
	{
		const FWorldObjectEntityHandle Entity = Batch.Entities[Index];
		const FWorldEntityId EntityId = Batch.EntityIds.IsValidIndex(Index)
			? Batch.EntityIds[Index] : FWorldEntityId();
		FWorldChunkCoord HomeChunk;
		if (Runtime && Runtime->Core.Registry.IsAlive(Entity))
		{
			if (const FWorldObjectTransformFragment* Transform =
					Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity))
			{
				HomeChunk = FWorldChunkCoord::FromWorldLocation(
					Transform->WorldTransform.GetLocation());
			}
			verify(DestroyEntityInternal(
				Entity, ERemovalSemantic::FailedRegistrationRollback, true, false, false));
		}
		if (WorldStorage && EntityId.IsSet())
		{
			const FWorldResidentEntityRegistration Registration{
				EntityId, EWorldEntityDomain::WorldObject, HomeChunk, 1};
			verify(WorldStorage->RollbackUnpublishedResidentRegistration(Registration));
		}
	}
	Batch.Reset();
}

bool UWorldObjectWorldSubsystem::RegisterDefinition(UWorldObjectDefinition& Definition)
{
	check(IsInGameThread());
	if (!Runtime || !Definition.IsDefinitionValid())
	{
		return false;
	}
	if (const TWeakObjectPtr<UWorldObjectDefinition>* Existing =
			Runtime->Core.DefinitionById.Find(Definition.DefinitionId))
	{
		return Existing->Get() == &Definition;
	}

	Runtime->Core.DefinitionById.Add(Definition.DefinitionId, &Definition);
	return true;
}

bool UWorldObjectWorldSubsystem::RegisterPersistenceExtension(
	const TSharedRef<IWorldObjectPersistenceExtension> Extension)
{
	check(IsInGameThread());
	const FName SectionId = Extension->GetSectionId();
	UWorldStorageSubsystem* WorldStorage = Runtime ? Runtime->Persistence.WorldStorage.Get() : nullptr;
	if (!Runtime || !WorldStorage || SectionId.IsNone() || Extension->GetSectionVersion() == 0 ||
		Runtime->Persistence.Extensions.Contains(SectionId) || !Extension->RegisterFragmentPersistence(*WorldStorage))
	{
		return false;
	}
	Runtime->Persistence.Extensions.Add(SectionId, Extension);
	Runtime->Persistence.ExtensionOrder.Add(SectionId);
	Runtime->Persistence.ExtensionOrder.Sort(FNameLexicalLess());
	return true;
}

bool UWorldObjectWorldSubsystem::UnregisterPersistenceExtension(const FName SectionId,
																const IWorldObjectPersistenceExtension& Extension)
{
	check(IsInGameThread());
	const TSharedRef<IWorldObjectPersistenceExtension>* Existing =
		Runtime ? Runtime->Persistence.Extensions.Find(SectionId) : nullptr;
	if (!Existing || &Existing->Get() != &Extension || Runtime->Persistence.Extensions.Remove(SectionId) != 1)
	{
		return false;
	}
	Runtime->Persistence.ExtensionOrder.RemoveSingle(SectionId);
	return true;
}

UWorldObjectDefinition* UWorldObjectWorldSubsystem::FindDefinition(const FName DefinitionId) const
{
	if (!Runtime || DefinitionId.IsNone())
	{
		return nullptr;
	}
	const TWeakObjectPtr<UWorldObjectDefinition>* Definition = Runtime->Core.DefinitionById.Find(DefinitionId);
	return Definition ? Definition->Get() : nullptr;
}

FWorldObjectEntityHandle UWorldObjectWorldSubsystem::CreateEntityInternal(
	UWorldObjectDefinition& Definition, const FTransform& WorldTransform, const EWorldObjectMotionState MotionState,
	const FWorldEntityId WorldEntityId, const uint32 StateRevision, UWorldObjectProxyComponent* Proxy,
	const TOptional<FBox>& InstanceInteractionBounds,
	const TOptional<FWorldObjectShapeDefinition>& InstanceShapeGeometry, const uint64 InstanceShapeRevision,
	const FWorldObjectPhysicsBodyInit* PhysicsBody, const bool bWorldStorageOwned, const bool bPublishLifecycle,
	const bool bPublishQuerySnapshot)
{
	check(IsInGameThread());
	if (!Runtime || !Definition.IsDefinitionValid() || !WorldEntityId.IsSet() || StateRevision == 0 ||
		Runtime->Core.EntityByWorldEntityId.Contains(WorldEntityId) ||
		(Definition.SpatialClass == EWorldObjectSpatialClass::PermanentStatic &&
		 MotionState != EWorldObjectMotionState::Dormant) ||
		(GetWorldRef().GetNetMode() != NM_Client && IsActorActiveState(MotionState) && !IsValid(Proxy)) ||
		(InstanceShapeGeometry.IsSet() && (!InstanceShapeGeometry->IsValid() || InstanceShapeRevision == 0)) ||
		(!InstanceShapeGeometry.IsSet() && InstanceShapeRevision != 1) ||
		(PhysicsBody && (!PhysicsBody->IsValid() || (MotionState != EWorldObjectMotionState::Physics &&
													 MotionState != EWorldObjectMotionState::Dormant))) ||
		(IsValid(Proxy) && Proxy->GetWorldEntityId().IsSet() && Proxy->GetWorldEntityId() != WorldEntityId))
	{
		return {};
	}

	FBox WorldBounds(ForceInit);
	if (!TryCalculateWorldBounds(Definition,
								 InstanceInteractionBounds.IsSet() ? &InstanceInteractionBounds.GetValue() : nullptr,
								 WorldTransform, WorldBounds))
	{
		return {};
	}

	const FWorldObjectEntityHandle Entity = Runtime->Core.Registry.CreateEntity();
	FWorldObjectDefinitionFragment DefinitionFragment;
	DefinitionFragment.Definition.Reset(&Definition);
	FWorldObjectTransformFragment TransformFragment;
	TransformFragment.WorldTransform = WorldTransform;
	FWorldObjectMotionFragment MotionFragment;
	MotionFragment.State = MotionState;
	FWorldObjectWorldIdentityFragment IdentityFragment;
	IdentityFragment.WorldEntityId = WorldEntityId;
	IdentityFragment.StateRevision = StateRevision;

	bool bFragmentsAdded = Runtime->Core.Registry.AddFragment(Entity, DefinitionFragment) &&
						   Runtime->Core.Registry.AddFragment(Entity, TransformFragment) &&
						   Runtime->Core.Registry.AddFragment(Entity, MotionFragment) &&
						   Runtime->Core.Registry.AddFragment(Entity, IdentityFragment);
	if (bFragmentsAdded && InstanceInteractionBounds.IsSet())
	{
		FWorldObjectInstanceInteractionBoundsFragment BoundsFragment;
		BoundsFragment.InteractionLocalBounds = InstanceInteractionBounds.GetValue();
		bFragmentsAdded = Runtime->Core.Registry.AddFragment(Entity, BoundsFragment);
	}
	if (bFragmentsAdded && InstanceShapeGeometry.IsSet())
	{
		FWorldObjectInstanceShapeFragment ShapeFragment;
		ShapeFragment.ShapeGeometry = InstanceShapeGeometry.GetValue();
		ShapeFragment.Revision = InstanceShapeRevision;
		bFragmentsAdded = Runtime->Core.Registry.AddFragment(Entity, ShapeFragment);
	}
	if (bFragmentsAdded)
	{
		bFragmentsAdded = Definition.ConfigureEntity(Runtime->Core.Registry, Entity);
	}
	FWorldObjectPhysicsBodyFragment* PhysicsFragment =
		bFragmentsAdded ? Runtime->Core.Registry.FindMutableFragment<FWorldObjectPhysicsBodyFragment>(Entity) : nullptr;
	if (bFragmentsAdded && PhysicsBody && !PhysicsFragment)
	{
		bFragmentsAdded = false;
	}
	if (bFragmentsAdded && PhysicsFragment)
	{
		const FBox& CollisionBounds = InstanceInteractionBounds.IsSet() ? InstanceInteractionBounds.GetValue()
																		: Definition.InteractionLocalBounds;
		PhysicsFragment->LocalCollisionCenter = CollisionBounds.GetCenter();
		PhysicsFragment->LocalCollisionExtent = CollisionBounds.GetExtent();
		if (PhysicsBody)
		{
			PhysicsFragment->MassKg = PhysicsBody->MassKg;
			PhysicsFragment->CollisionPolicy = PhysicsBody->CollisionPolicy;
			PhysicsFragment->InitialLinearVelocity = PhysicsBody->LinearVelocity;
			PhysicsFragment->InitialAngularVelocityDegrees = PhysicsBody->AngularVelocityDegrees;
		}
		bFragmentsAdded = PhysicsFragment->MassKg > UE_SMALL_NUMBER &&
						  PhysicsFragment->CollisionPolicy >= EWorldObjectPhysicsCollisionPolicy::Standard &&
						  PhysicsFragment->CollisionPolicy <= EWorldObjectPhysicsCollisionPolicy::LooseDebris &&
						  !PhysicsFragment->LocalCollisionCenter.ContainsNaN() &&
						  !PhysicsFragment->LocalCollisionExtent.ContainsNaN() &&
						  PhysicsFragment->LocalCollisionExtent.GetMin() > UE_SMALL_NUMBER;
	}
	if (!bFragmentsAdded ||
		!Runtime->Core.SpatialIndex.Insert(Entity, WorldBounds, Definition.SpatialClass, WorldTransform.GetLocation()))
	{
		if (Runtime->Core.SpatialIndex.Contains(Entity))
		{
			verify(Runtime->Core.SpatialIndex.Remove(Entity));
		}
		verify(Runtime->Core.Registry.DestroyEntity(Entity));
		return {};
	}

	Runtime->EnsureSlotCapacity(Entity.GetSlot());
	Runtime->Projection.ActorActiveIndexBySlot[Entity.GetSlot()] = INDEX_NONE;
	Runtime->Projection.ProxyBySlot[Entity.GetSlot()] = nullptr;
	Runtime->Core.EntityByWorldEntityId.Add(WorldEntityId, Entity);

	if (IsValid(Proxy))
	{
		if (GetWorldRef().GetNetMode() != NM_Client && !Proxy->AssignAuthorityWorldEntityId(WorldEntityId))
		{
			Runtime->Core.EntityByWorldEntityId.Remove(WorldEntityId);
			verify(Runtime->Core.SpatialIndex.Remove(Entity));
			verify(Runtime->Core.Registry.DestroyEntity(Entity));
			return {};
		}
		if (!BindProxyToEntity(Entity, *Proxy))
		{
			Proxy->WorldEntityId = {};
			Runtime->Core.EntityByWorldEntityId.Remove(WorldEntityId);
			verify(Runtime->Core.SpatialIndex.Remove(Entity));
			verify(Runtime->Core.Registry.DestroyEntity(Entity));
			return {};
		}
	}
	else if (TWeakObjectPtr<UWorldObjectProxyComponent>* Pending =
				 Runtime->Projection.PendingProxies.Find(WorldEntityId))
	{
		if (UWorldObjectProxyComponent* PendingProxy = Pending->Get())
		{
			BindProxyToEntity(Entity, *PendingProxy);
		}
		Runtime->Projection.PendingProxies.Remove(WorldEntityId);
	}
	// 持久化所有权必须最后提交。Proxy 身份分配或绑定仍可能失败；提前写入会让
	// 创建回滚遗留一个没有对应 ECS Entity 的幽灵 WorldEntityId。
	if (bWorldStorageOwned)
	{
		Runtime->Persistence.OwnedEntities.Add(WorldEntityId);
	}
	if (IsActorActiveState(MotionState) && IsValid(Proxy))
	{
		AddActorActive(Entity);
	}

	if (bPublishLifecycle)
	{
		FWorldObjectLifecycleRecord Record;
		if (BuildLifecycleRecord(Entity, Record))
		{
			EntitiesUpsertedEvent.Broadcast(MakeArrayView(&Record, 1));
		}
	}
	if (bPublishQuerySnapshot)
	{
		FWorldObjectShapeInstanceSnapshot Shape;
		if (BuildShapeSnapshot(Entity, Shape))
		{
			verify(PublishShapeTransition({}, Shape, EWorldObjectQuerySnapshotChangeKind::Upsert,
										  EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
										  GetEffectiveTimeMilliseconds(GetWorldRef())));
		}
	}
	return Entity;
}

bool UWorldObjectWorldSubsystem::DestroyEntity(const FWorldObjectEntityHandle Entity)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_DestroyEntity);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, DestroyEntity);
	return Runtime && GetWorldRef().GetNetMode() != NM_Client &&
		   DestroyEntityInternal(Entity, ERemovalSemantic::GameplayDestroy, true, true);
}

bool UWorldObjectWorldSubsystem::BeginGameplayDestructionBatch()
{
	check(IsInGameThread());
	return Runtime && GetWorldRef().GetNetMode() != NM_Client
		&& !Runtime->Core.QuerySnapshots.IsInTransaction()
		&& Runtime->Core.QuerySnapshots.BeginTransaction();
}

bool UWorldObjectWorldSubsystem::EndGameplayDestructionBatch(const bool bCommit)
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

bool UWorldObjectWorldSubsystem::DestroyEntityInternal(const FWorldObjectEntityHandle Entity,
													   const ERemovalSemantic Semantic, const bool bDestroyProxyActor,
													   const bool bPublishLifecycle, const bool bPublishQuerySnapshot)
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}

	const bool bGameplayMutation = Semantic == ERemovalSemantic::GameplayDestroy;
	bool bCanDestroy = true;
	EntityPreDestroyEvent.Broadcast(Entity, bCanDestroy);
	if ((bGameplayMutation && !bCanDestroy) || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}

	const FWorldObjectWorldIdentityFragment* IdentityFragment =
		Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
	if (!IdentityFragment)
	{
		return false;
	}
	FWorldObjectLifecycleRecord LifecycleRecord;
	if (!BuildLifecycleRecord(Entity, LifecycleRecord))
	{
		return false;
	}
	TOptional<FWorldObjectShapeInstanceSnapshot> PreviousShape;
	if (bPublishQuerySnapshot)
	{
		FWorldObjectShapeInstanceSnapshot Shape;
		if (!BuildShapeSnapshot(Entity, Shape))
		{
			return false;
		}
		PreviousShape = MoveTemp(Shape);
	}
	const FWorldEntityId WorldEntityId = IdentityFragment->WorldEntityId;
	const bool bWorldStorageOwned = Runtime->Persistence.OwnedEntities.Contains(WorldEntityId);
	if (bGameplayMutation && bWorldStorageOwned && GetWorldRef().GetNetMode() != NM_Client)
	{
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		const uint32 RemovedRevision = NextRevision(IdentityFragment->StateRevision);
		if (!WorldStorage || !WorldStorage->GameplayDestroy(WorldEntityId, RemovedRevision))
		{
			return false;
		}
	}
	AActor* ProxyActor = nullptr;
	UWorldObjectProxyComponent* Proxy = GetProxy(Entity);
	if (Proxy)
	{
		Proxy->SuppressEntityDestroyOnEndPlay();
		ProxyActor = Proxy->GetOwner();
		if (Semantic == ERemovalSemantic::GameplayDestroy)
		{
			if (auto* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(ProxyActor))
				PhysicsProxy->RetireClientPhysicsProjection();
		}
		UnbindProxy(*Proxy);
	}
	const bool bClientReplicatedProxy = GetWorldRef().GetNetMode() == NM_Client
		&& IsValid(ProxyActor) && ProxyActor->GetIsReplicated() && !ProxyActor->HasAuthority();
	RemoveActorActive(Entity);
	if (!Runtime->Core.SpatialIndex.Remove(Entity))
	{
		return false;
	}
	Runtime->Core.EntityByWorldEntityId.Remove(WorldEntityId);
	Runtime->Projection.PendingProxies.Remove(WorldEntityId);
	Runtime->Persistence.OwnedEntities.Remove(WorldEntityId);
	Runtime->Persistence.HomeChunks.Remove(WorldEntityId);
	if (Runtime->Projection.ProxyBySlot.IsValidIndex(Entity.GetSlot()))
	{
		Runtime->Projection.ProxyBySlot[Entity.GetSlot()] = nullptr;
	}
	if (!Runtime->Core.Registry.DestroyEntity(Entity))
	{
		return false;
	}
	if (bClientReplicatedProxy && Semantic != ERemovalSemantic::GameplayDestroy)
	{
		// Chunk 迁移/卸载不终止 Actor Channel；保留弱绑定等待下一份 Record，结束仍由复制销毁负责。
		Runtime->Projection.PendingProxies.Add(WorldEntityId, Proxy);
	}
	if (bPublishLifecycle)
	{
		if (Semantic == ERemovalSemantic::GameplayDestroy)
		{
			EntitiesGameplayDestroyedEvent.Broadcast(MakeArrayView(&LifecycleRecord, 1));
		}
		else if (Semantic == ERemovalSemantic::RuntimeEvict)
		{
			EntitiesRuntimeEvictedEvent.Broadcast(MakeArrayView(&LifecycleRecord, 1));
		}
	}
	if (PreviousShape.IsSet())
	{
		EWorldObjectQuerySnapshotChangeKind RemovalKind = EWorldObjectQuerySnapshotChangeKind::RuntimeEvict;
		switch (Semantic)
		{
		case ERemovalSemantic::GameplayDestroy:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::GameplayDestroy;
			break;
		case ERemovalSemantic::RuntimeEvict:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::RuntimeEvict;
			break;
		case ERemovalSemantic::LeaveInterest:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::LeaveInterest;
			break;
		case ERemovalSemantic::FailedRegistrationRollback:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::FailedRegistrationRollback;
			break;
		default:
			checkNoEntry();
			return false;
		}
		verify(PublishShapeTransition(PreviousShape, {}, EWorldObjectQuerySnapshotChangeKind::Metadata, RemovalKind,
									  GetEffectiveTimeMilliseconds(GetWorldRef())));
	}
	if (bDestroyProxyActor && !bClientReplicatedProxy && IsValid(ProxyActor) && !ProxyActor->IsActorBeingDestroyed())
	{
		ProxyActor->Destroy();
	}
	return true;
}

bool UWorldObjectWorldSubsystem::SetMotionState(const FWorldObjectEntityHandle Entity,
												const EWorldObjectMotionState NewState)
{
	check(IsInGameThread());
	return Runtime && GetWorldRef().GetNetMode() != NM_Client && SetMotionStateInternal(Entity, NewState, true);
}

bool UWorldObjectWorldSubsystem::ActivatePhysics(const FWorldObjectEntityHandle Entity,
												 const FVector& LinearVelocity,
												 const FVector& AngularVelocityDegrees)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || LinearVelocity.ContainsNaN() ||
		AngularVelocityDegrees.ContainsNaN() || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	const FWorldObjectMotionFragment* Motion =
		Runtime->Core.Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
	const FWorldObjectTransformFragment* Transform =
		Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
	const FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
	if (!Motion || Motion->State != EWorldObjectMotionState::Dormant || !Transform || !Identity)
	{
		return false;
	}

	if (UWorldObjectProxyComponent* ExistingProxy = GetProxy(Entity))
	{
		// 自定义 Actor（当前为木棍）同时承担可见表现，Sleep 时只关闭物理投影。
		if (Cast<AWorldObjectPhysicsProxyActor>(ExistingProxy->GetOwner()) ||
			!ExistingProxy->SetAuthorityPhysicsProjectionActive(true))
		{
			return false;
		}
		UPrimitiveComponent* Primitive = ExistingProxy->GetPhysicsPrimitive();
		if (!Primitive)
		{
			ExistingProxy->SetAuthorityPhysicsProjectionActive(false);
			return false;
		}
		// SetMotionStateInternal 会同步发布 Live Delta；先让 Capture 看到本次唤醒速度。
		Primitive->SetPhysicsLinearVelocity(LinearVelocity);
		Primitive->SetPhysicsAngularVelocityInDegrees(AngularVelocityDegrees);
		Primitive->WakeAllRigidBodies();
		if (!SetMotionStateInternal(Entity, EWorldObjectMotionState::Physics, true))
		{
			ExistingProxy->SetAuthorityPhysicsProjectionActive(false);
			return false;
		}
		return true;
	}

	FWorldObjectPhysicsBodyFragment* Physics =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectPhysicsBodyFragment>(Entity);
	if (!Physics)
	{
		return false;
	}
	const FBox LocalBounds(Physics->LocalCollisionCenter - Physics->LocalCollisionExtent,
						   Physics->LocalCollisionCenter + Physics->LocalCollisionExtent);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWorldObjectPhysicsProxyActor* SpawnedProxy =
		GetWorldRef().SpawnActor<AWorldObjectPhysicsProxyActor>(SpawnParameters);
	if (!IsValid(SpawnedProxy))
	{
		return false;
	}
	SpawnedProxy->SetActorTransform(
		AWorldObjectPhysicsProxyActor::MakeActorTransform(Transform->WorldTransform, LocalBounds), false, nullptr,
		ETeleportType::TeleportPhysics);
	UWorldObjectProxyComponent* Proxy = SpawnedProxy->GetWorldObjectProxyComponent();
	const bool bConfigured = SpawnedProxy->ConfigurePhysics(
		LocalBounds, Physics->MassKg, Physics->CollisionPolicy, LinearVelocity, AngularVelocityDegrees,
		NextRevision(Identity->StateRevision)) &&
		Proxy && Proxy->AssignAuthorityWorldEntityId(Identity->WorldEntityId);
	if (!bConfigured)
	{
		if (Proxy)
		{
			Proxy->SuppressEntityDestroyOnEndPlay();
			UnbindProxy(*Proxy);
		}
		SpawnedProxy->Destroy();
		return false;
	}
	const FVector PreviousLinearVelocity = Physics->InitialLinearVelocity;
	const FVector PreviousAngularVelocityDegrees = Physics->InitialAngularVelocityDegrees;
	Physics->InitialLinearVelocity = LinearVelocity;
	Physics->InitialAngularVelocityDegrees = AngularVelocityDegrees;
	if (!SetMotionStateInternal(Entity, EWorldObjectMotionState::Physics, true))
	{
		Physics->InitialLinearVelocity = PreviousLinearVelocity;
		Physics->InitialAngularVelocityDegrees = PreviousAngularVelocityDegrees;
		Proxy->SuppressEntityDestroyOnEndPlay();
		UnbindProxy(*Proxy);
		SpawnedProxy->Destroy();
		return false;
	}
	// Dormant HISM 已经完成可见表现交接；接触/投掷唤醒无需等待新生成 Actor 的复制预留窗。
	SpawnedProxy->ReleasePhysicsImmediately();
	return true;
}

void UWorldObjectWorldSubsystem::ScheduleAutomaticPhysicsRelease(
	const FWorldObjectEntityHandle Entity)
{
	if (UWorldObjectProxyComponent* Proxy = GetProxy(Entity))
	{
		if (AWorldObjectPhysicsProxyActor* PhysicsProxy =
				Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()))
		{
			PhysicsProxy->SchedulePhysicsRelease();
		}
	}
}

bool UWorldObjectWorldSubsystem::SetMotionStateInternal(const FWorldObjectEntityHandle Entity,
														const EWorldObjectMotionState NewState,
														const bool bGameplayMutation, const bool bPublishQuerySnapshot)
{
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	FWorldObjectMotionFragment* Motion = Runtime->Core.Registry.FindMutableFragment<FWorldObjectMotionFragment>(Entity);
	const FWorldObjectDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
	FWorldObjectWorldIdentityFragment* WorldEntityIdentity =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectWorldIdentityFragment>(Entity);
	const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	if (!Motion || !Definition || !WorldEntityIdentity ||
		(Definition->SpatialClass == EWorldObjectSpatialClass::PermanentStatic &&
		 NewState != EWorldObjectMotionState::Dormant) ||
		(bGameplayMutation && IsActorActiveState(NewState) && !IsValid(GetProxy(Entity))))
	{
		return false;
	}
	if (Motion->State == NewState)
	{
		return true;
	}

	const EWorldObjectMotionState PreviousState = Motion->State;
	const bool bWasWorldOwned = Runtime->Persistence.OwnedEntities.Contains(WorldEntityIdentity->WorldEntityId);
	TOptional<FWorldResidentEntityRegistration> NewWorldRegistration;
	if (bGameplayMutation && bWasWorldOwned && NewState == EWorldObjectMotionState::Attached)
	{
		// Attached/Equipped 状态必须先由角色或背包存档接管；当前接口不允许把
		// 世界 Chunk 记录悄悄变成双重所有权。
		return false;
	}
	if (bGameplayMutation && !bWasWorldOwned && NewState != EWorldObjectMotionState::Attached)
	{
		const FWorldObjectTransformFragment* Transform =
			Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		const uint32 NewRevision = NextRevision(WorldEntityIdentity->StateRevision);
		if (!Transform || !WorldStorage)
		{
			return false;
		}
		const FWorldChunkCoord HomeChunk = FWorldChunkCoord::FromWorldLocation(Transform->WorldTransform.GetLocation());
		const FWorldResidentEntityRegistration Registration{WorldEntityIdentity->WorldEntityId,
															EWorldEntityDomain::WorldObject, HomeChunk, NewRevision};
		if (WorldStorage->RegisterResidentEntity(Registration) != EWorldResidentUpsertResult::Inserted)
		{
			return false;
		}
		NewWorldRegistration = Registration;
		Runtime->Persistence.OwnedEntities.Add(WorldEntityIdentity->WorldEntityId);
		Runtime->Persistence.HomeChunks.Add(WorldEntityIdentity->WorldEntityId, HomeChunk);
	}
	AActor* ProxyActor = nullptr;
	AWorldObjectPhysicsProxyActor* AutomaticPhysicsProxyActor = nullptr;
	if (const UWorldObjectProxyComponent* Proxy = GetProxy(Entity))
	{
		ProxyActor = Proxy->GetOwner();
		AutomaticPhysicsProxyActor = Cast<AWorldObjectPhysicsProxyActor>(ProxyActor);
	}
	if (bGameplayMutation && IsActorActiveState(NewState) && IsValid(ProxyActor))
	{
		// Wake 必须先打破 Actor Channel 休眠，再恢复状态与移动复制。
		ProxyActor->FlushNetDormancy();
		ProxyActor->SetNetDormancy(DORM_Awake);
		ProxyActor->SetReplicateMovement(true);
	}
	if (bGameplayMutation && NewState == EWorldObjectMotionState::Dormant && IsActorActiveState(PreviousState))
	{
		if (const UWorldObjectProxyComponent* Proxy = GetProxy(Entity))
		{
			if (const AActor* OwnerActor = Proxy->GetOwner())
			{
				const AWorldObjectPhysicsProxyActor* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(OwnerActor);
				if (!CommitTransformInternal(Entity,
											 PhysicsProxy ? PhysicsProxy->GetWorldObjectTransform()
														  : OwnerActor->GetActorTransform(),
											 false, bPublishQuerySnapshot))
				{
					return false;
				}
			}
		}
	}
	TOptional<FWorldObjectShapeInstanceSnapshot> PreviousShape;
	if (bPublishQuerySnapshot)
	{
		FWorldObjectShapeInstanceSnapshot Shape;
		if (!BuildShapeSnapshot(Entity, Shape))
		{
			return false;
		}
		PreviousShape = MoveTemp(Shape);
	}
	const uint32 NewStateRevision =
		bGameplayMutation ? NextRevision(WorldEntityIdentity->StateRevision) : WorldEntityIdentity->StateRevision;
	const uint32 PreviousStateRevision = WorldEntityIdentity->StateRevision;
	if (bGameplayMutation)
	{
		// MarkEntityDirty 的监听方同步 Capture；先提交 Motion 与 Revision，确保
		// Live Delta 的 Payload、外层 Revision 和客户端可见状态来自同一版本。
		Motion->State = NewState;
		WorldEntityIdentity->StateRevision = NewStateRevision;
	}
	if (bGameplayMutation && Runtime->Persistence.OwnedEntities.Contains(WorldEntityIdentity->WorldEntityId))
	{
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		if (!WorldStorage || !WorldStorage->MarkEntityDirty(WorldEntityIdentity->WorldEntityId, NewStateRevision))
		{
			Motion->State = PreviousState;
			WorldEntityIdentity->StateRevision = PreviousStateRevision;
			if (NewWorldRegistration.IsSet())
			{
				Runtime->Persistence.OwnedEntities.Remove(WorldEntityIdentity->WorldEntityId);
				Runtime->Persistence.HomeChunks.Remove(WorldEntityIdentity->WorldEntityId);
				if (WorldStorage)
				{
					verify(WorldStorage->RollbackUnpublishedResidentRegistration(NewWorldRegistration.GetValue()));
				}
			}
			return false;
		}
	}
	if (IsActorActiveState(PreviousState))
	{
		RemoveActorActive(Entity);
	}
	if (!bGameplayMutation)
	{
		Motion->State = NewState;
	}
	if (IsActorActiveState(NewState))
	{
		AddActorActive(Entity);
	}
	if (bGameplayMutation)
	{
		if (IsValid(ProxyActor))
		{
			ProxyActor->ForceNetUpdate();
			if (NewState == EWorldObjectMotionState::Dormant)
			{
				ProxyActor->SetNetDormancy(DORM_DormantAll);
				ProxyActor->SetReplicateMovement(false);
			}
		}
	}
	if (PreviousShape.IsSet())
	{
		FWorldObjectShapeInstanceSnapshot CurrentShape;
		if (!BuildShapeSnapshot(Entity, CurrentShape) ||
			!PublishShapeTransition(PreviousShape, CurrentShape, EWorldObjectQuerySnapshotChangeKind::Metadata,
									EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
									GetEffectiveTimeMilliseconds(GetWorldRef())))
		{
			return false;
		}
	}
	if (bGameplayMutation && NewState == EWorldObjectMotionState::Dormant && IsActorActiveState(PreviousState))
	{
		if (UWorldObjectProxyComponent* Proxy = GetProxy(Entity))
		{
			if (AutomaticPhysicsProxyActor)
			{
				// 自动代理没有可见内容。Sleep 已经把最终 Transform 封口，随后完整回收 Chaos Body。
				Proxy->SuppressEntityDestroyOnEndPlay();
				UnbindProxy(*Proxy);
				AutomaticPhysicsProxyActor->Destroy();
			}
			else
			{
				// 自定义 Actor 仍是表现载体，只撤掉服务器和客户端的临时物理投影。
				Proxy->SetAuthorityPhysicsProjectionActive(false);
			}
		}
	}
	return true;
}
