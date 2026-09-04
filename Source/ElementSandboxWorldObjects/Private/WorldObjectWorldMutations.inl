// UWorldObjectWorldSubsystem：Transform、实例几何与持久状态事务。
// 只修改 WorldObject ECS 真值并产生投影失效信号。

bool UWorldObjectWorldSubsystem::CommitPortableTransform(const FWorldObjectEntityHandle Entity,
														 const FTransform& WorldTransform)
{
	check(IsInGameThread());
	return Runtime && GetWorldRef().GetNetMode() != NM_Client && CommitTransformInternal(Entity, WorldTransform, true);
}

bool UWorldObjectWorldSubsystem::CommitInstanceGeometryChange(const FWorldObjectEntityHandle Entity,
															  const FBox& InteractionLocalBounds,
															  const FWorldObjectShapeDefinition& ShapeGeometry)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || !Runtime->Core.Registry.IsAlive(Entity) ||
		InteractionLocalBounds.IsValid == 0 || InteractionLocalBounds.ContainsNaN() ||
		InteractionLocalBounds.GetExtent().GetMin() <= UE_DOUBLE_SMALL_NUMBER || !ShapeGeometry.IsValid())
	{
		return false;
	}

	const FWorldObjectDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
	const FWorldObjectTransformFragment* Transform =
		Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
	FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectWorldIdentityFragment>(Entity);
	FWorldObjectInstanceInteractionBoundsFragment* ExistingBounds =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectInstanceInteractionBoundsFragment>(Entity);
	FWorldObjectInstanceShapeFragment* ExistingShape =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectInstanceShapeFragment>(Entity);
	const FWorldObjectPhysicsBodyFragment* Physics =
		Runtime->Core.Registry.FindFragment<FWorldObjectPhysicsBodyFragment>(Entity);
	const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	if (!Definition || !Transform || !Identity)
	{
		return false;
	}

	const FBox& PreviousInteractionBounds =
		ExistingBounds ? ExistingBounds->InteractionLocalBounds : Definition->InteractionLocalBounds;
	const FWorldObjectShapeDefinition& PreviousGeometry =
		ExistingShape ? ExistingShape->ShapeGeometry : Definition->ShapeGeometry;
	const bool bBoundsChanged = !PreviousInteractionBounds.Equals(InteractionLocalBounds, 0.01);
	const bool bShapeChanged = !AreWorldObjectShapesEqual(PreviousGeometry, ShapeGeometry);
	if (!bBoundsChanged && !bShapeChanged)
	{
		return true;
	}
	// 当前 Physics Proxy 的 Box 几何在创建事务中固定。这里不能只移动宿主 Bounds，
	// 否则 Interaction/Spatial 与真实 Chaos Body 会悄悄分叉。
	if (bBoundsChanged && Physics)
	{
		return false;
	}

	FWorldObjectShapeInstanceSnapshot PreviousShape;
	FBox PreviousWorldBounds(ForceInit);
	FBox NewWorldBounds(ForceInit);
	if (!BuildShapeSnapshot(Entity, PreviousShape) ||
		!Runtime->Core.SpatialIndex.TryGetBounds(Entity, PreviousWorldBounds) ||
		!TryCalculateWorldBounds(*Definition, &InteractionLocalBounds, Transform->WorldTransform, NewWorldBounds))
	{
		return false;
	}

	bool bSpatialUpdated = !bBoundsChanged;
	if (bBoundsChanged && Definition->SpatialClass == EWorldObjectSpatialClass::Portable)
	{
		bSpatialUpdated = Runtime->Core.SpatialIndex.UpdatePortable(Entity, NewWorldBounds);
	}
	else if (bBoundsChanged && Runtime->Core.SpatialIndex.Remove(Entity))
	{
		bSpatialUpdated = Runtime->Core.SpatialIndex.Insert(
			Entity, NewWorldBounds, EWorldObjectSpatialClass::PermanentStatic, Transform->WorldTransform.GetLocation());
		if (!bSpatialUpdated)
		{
			verify(Runtime->Core.SpatialIndex.Insert(Entity, PreviousWorldBounds,
													 EWorldObjectSpatialClass::PermanentStatic,
													 Transform->WorldTransform.GetLocation()));
		}
	}
	if (!bSpatialUpdated)
	{
		return false;
	}

	const bool bAddedBounds = bBoundsChanged && ExistingBounds == nullptr;
	const bool bAddedShape = bShapeChanged && ExistingShape == nullptr;
	FWorldObjectInstanceInteractionBoundsFragment PreviousBoundsFragment;
	FWorldObjectInstanceShapeFragment PreviousShapeFragment;
	if (bBoundsChanged && ExistingBounds)
	{
		PreviousBoundsFragment = *ExistingBounds;
		ExistingBounds->InteractionLocalBounds = InteractionLocalBounds;
		ExistingBounds->Revision = NextRevision64(ExistingBounds->Revision);
	}
	else if (bBoundsChanged)
	{
		FWorldObjectInstanceInteractionBoundsFragment NewBoundsFragment;
		NewBoundsFragment.InteractionLocalBounds = InteractionLocalBounds;
		NewBoundsFragment.Revision = 2;
		if (!Runtime->Core.Registry.AddFragment(Entity, NewBoundsFragment))
		{
			if (Definition->SpatialClass == EWorldObjectSpatialClass::Portable)
			{
				verify(Runtime->Core.SpatialIndex.UpdatePortable(Entity, PreviousWorldBounds));
			}
			else
			{
				verify(Runtime->Core.SpatialIndex.Remove(Entity));
				verify(Runtime->Core.SpatialIndex.Insert(Entity, PreviousWorldBounds,
														 EWorldObjectSpatialClass::PermanentStatic,
														 Transform->WorldTransform.GetLocation()));
			}
			return false;
		}
	}
	if (bShapeChanged && ExistingShape)
	{
		PreviousShapeFragment = *ExistingShape;
		ExistingShape->ShapeGeometry = ShapeGeometry;
		ExistingShape->Revision = NextRevision64(ExistingShape->Revision);
	}
	else if (bShapeChanged)
	{
		FWorldObjectInstanceShapeFragment NewShapeFragment;
		NewShapeFragment.ShapeGeometry = ShapeGeometry;
		NewShapeFragment.Revision = NextRevision64(PreviousShape.ShapeRevision);
		if (!Runtime->Core.Registry.AddFragment(Entity, NewShapeFragment))
		{
			if (bAddedBounds)
			{
				verify(Runtime->Core.Registry.RemoveFragment<FWorldObjectInstanceInteractionBoundsFragment>(Entity));
			}
			else if (bBoundsChanged)
			{
				*ExistingBounds = PreviousBoundsFragment;
			}
			if (bBoundsChanged && Definition->SpatialClass == EWorldObjectSpatialClass::Portable)
			{
				verify(Runtime->Core.SpatialIndex.UpdatePortable(Entity, PreviousWorldBounds));
			}
			else if (bBoundsChanged)
			{
				verify(Runtime->Core.SpatialIndex.Remove(Entity));
				verify(Runtime->Core.SpatialIndex.Insert(Entity, PreviousWorldBounds,
														 EWorldObjectSpatialClass::PermanentStatic,
														 Transform->WorldTransform.GetLocation()));
			}
			return false;
		}
	}

	const uint32 PreviousStateRevision = Identity->StateRevision;
	const uint32 NewStateRevision = NextRevision(PreviousStateRevision);
	// Authority Mutation 监听方会在 MarkEntityDirty 内同步 Capture；实例几何与
	// Revision 都必须在通知前成为同一份可见状态。
	Identity->StateRevision = NewStateRevision;
	if (Runtime->Persistence.OwnedEntities.Contains(Identity->WorldEntityId))
	{
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		if (!WorldStorage || !WorldStorage->MarkEntityDirty(Identity->WorldEntityId, NewStateRevision))
		{
			Identity->StateRevision = PreviousStateRevision;
			if (bAddedShape)
			{
				verify(Runtime->Core.Registry.RemoveFragment<FWorldObjectInstanceShapeFragment>(Entity));
			}
			else if (bShapeChanged)
			{
				*ExistingShape = PreviousShapeFragment;
			}
			if (bAddedBounds)
			{
				verify(Runtime->Core.Registry.RemoveFragment<FWorldObjectInstanceInteractionBoundsFragment>(Entity));
			}
			else if (bBoundsChanged)
			{
				*ExistingBounds = PreviousBoundsFragment;
			}
			if (bBoundsChanged && Definition->SpatialClass == EWorldObjectSpatialClass::Portable)
			{
				verify(Runtime->Core.SpatialIndex.UpdatePortable(Entity, PreviousWorldBounds));
			}
			else if (bBoundsChanged)
			{
				verify(Runtime->Core.SpatialIndex.Remove(Entity));
				verify(Runtime->Core.SpatialIndex.Insert(Entity, PreviousWorldBounds,
														 EWorldObjectSpatialClass::PermanentStatic,
														 Transform->WorldTransform.GetLocation()));
			}
			return false;
		}
	}

	FWorldObjectShapeInstanceSnapshot CurrentShape;
	return BuildShapeSnapshot(Entity, CurrentShape) &&
		   PublishShapeTransition(PreviousShape, CurrentShape, EWorldObjectQuerySnapshotChangeKind::Metadata,
								  EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
								  GetEffectiveTimeMilliseconds(GetWorldRef()));
}

bool UWorldObjectWorldSubsystem::CanCommitPersistentStateChange(const FWorldObjectEntityHandle Entity) const
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || !Runtime->Core.Registry.IsAlive(Entity) ||
		Runtime->Core.QuerySnapshots.IsInTransaction())
	{
		return false;
	}
	const FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
	if (!Identity)
		return false;
	if (!Runtime->Persistence.OwnedEntities.Contains(Identity->WorldEntityId))
		return true;
	const UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	return WorldStorage && WorldStorage->IsResident(Identity->WorldEntityId);
}

bool UWorldObjectWorldSubsystem::CommitPersistentStateChange(const FWorldObjectEntityHandle Entity)
{
	check(IsInGameThread());
	if (!CanCommitPersistentStateChange(Entity))
	{
		return false;
	}
	FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectWorldIdentityFragment>(Entity);
	if (!Identity)
	{
		return false;
	}
	FWorldObjectShapeInstanceSnapshot PreviousShape;
	if (!BuildShapeSnapshot(Entity, PreviousShape))
	{
		return false;
	}
	const uint32 NewStateRevision = NextRevision(Identity->StateRevision);
	const uint32 PreviousStateRevision = Identity->StateRevision;
	// WorldStorage 的 Mutation 监听方会在 MarkEntityDirty 内同步捕获 Record。
	// 先发布宿主 Revision，确保木棍燃烧等持久状态的 Live Delta 内外 Revision
	// 一致；否则客户端会拒绝整个 Batch，Actor 表现将停留在旧状态。
	Identity->StateRevision = NewStateRevision;
	if (Runtime->Persistence.OwnedEntities.Contains(Identity->WorldEntityId))
	{
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		if (!WorldStorage || !WorldStorage->MarkEntityDirty(Identity->WorldEntityId, NewStateRevision))
		{
			Identity->StateRevision = PreviousStateRevision;
			return false;
		}
	}
	// Attached/Equipped 的持久化由角色或背包所有者负责，但宿主 Revision/快照流
	// 仍必须推进，供其唯一所有者消费同一份中性状态变化。
	FWorldObjectShapeInstanceSnapshot CurrentShape;
	return BuildShapeSnapshot(Entity, CurrentShape) &&
		   PublishShapeTransition(PreviousShape, CurrentShape, EWorldObjectQuerySnapshotChangeKind::Metadata,
								  EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
								  GetEffectiveTimeMilliseconds(GetWorldRef()));
}

bool UWorldObjectWorldSubsystem::CommitPersistentStateOnlyChange(const FWorldObjectEntityHandle Entity)
{
	check(IsInGameThread());
	if (!CanCommitPersistentStateChange(Entity))
	{
		return false;
	}
	FWorldObjectWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectWorldIdentityFragment>(Entity);
	if (!Identity)
	{
		return false;
	}

	const uint32 PreviousStateRevision = Identity->StateRevision;
	const uint32 NewStateRevision = NextRevision(PreviousStateRevision);
	// Fire 等纯状态结算不改变交互包络、Transform 或 Shape。Attached/Equipped
	// 实体由其唯一所有者持久化，但本地宿主 Revision 仍需单调推进。
	Identity->StateRevision = NewStateRevision;
	if (Runtime->Persistence.OwnedEntities.Contains(Identity->WorldEntityId))
	{
		UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
		if (!WorldStorage || !WorldStorage->MarkEntityDirty(Identity->WorldEntityId, NewStateRevision))
		{
			Identity->StateRevision = PreviousStateRevision;
			return false;
		}
	}
	return true;
}

bool UWorldObjectWorldSubsystem::CommitTransformInternal(const FWorldObjectEntityHandle Entity,
														 const FTransform& WorldTransform,
														 const bool bPublishStableTransform,
														 const bool bPublishQuerySnapshot)
{
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity) || WorldTransform.ContainsNaN())
	{
		return false;
	}
	FWorldObjectTransformFragment* Transform =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectTransformFragment>(Entity);
	const FWorldObjectDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
	FWorldObjectWorldIdentityFragment* WorldEntityIdentity =
		Runtime->Core.Registry.FindMutableFragment<FWorldObjectWorldIdentityFragment>(Entity);
	const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	const FWorldObjectInstanceInteractionBoundsFragment* InstanceBounds =
		Runtime->Core.Registry.FindFragment<FWorldObjectInstanceInteractionBoundsFragment>(Entity);
	if (!Transform || !Definition || !WorldEntityIdentity ||
		Definition->SpatialClass != EWorldObjectSpatialClass::Portable)
	{
		return false;
	}
	if (Transform->WorldTransform.Equals(WorldTransform, 0.01))
	{
		return true;
	}
	const FTransform PreviousTransform = Transform->WorldTransform;
	const uint64 PreviousTransformRevision = Transform->Revision;
	const uint32 PreviousStateRevision = WorldEntityIdentity->StateRevision;
	FBox PreviousWorldBounds(ForceInit);
	if (!Runtime->Core.SpatialIndex.TryGetBounds(Entity, PreviousWorldBounds))
	{
		return false;
	}
	TOptional<FWorldObjectShapeInstanceSnapshot> PreviousShape;
	if (bPublishQuerySnapshot)
	{
		FWorldObjectShapeInstanceSnapshot Snapshot;
		if (!BuildShapeSnapshot(Entity, Snapshot))
		{
			return false;
		}
		PreviousShape = MoveTemp(Snapshot);
	}
	FBox WorldBounds(ForceInit);
	if (!TryCalculateWorldBounds(*Definition, InstanceBounds ? &InstanceBounds->InteractionLocalBounds : nullptr,
								 WorldTransform, WorldBounds))
	{
		return false;
	}
	if (!Runtime->Core.SpatialIndex.UpdatePortable(Entity, WorldBounds))
	{
		return false;
	}
	Transform->WorldTransform = WorldTransform;
	Transform->Revision = NextRevision64(Transform->Revision);
	if (bPublishStableTransform)
	{
		WorldEntityIdentity->StateRevision = NextRevision(WorldEntityIdentity->StateRevision);
	}
	// Client 的 Actor/预测位姿只更新本地空间投影；Chunk 所属与 StateRevision 由服务器 Record 驱动。
	// 在客户端调用 UpdateEntityLocation 会被权威入口拒绝，并使跨 Chunk 的 Restore 永久失败。
	if (GetWorldRef().GetNetMode() != NM_Client
		&& Runtime->Persistence.OwnedEntities.Contains(WorldEntityIdentity->WorldEntityId))
	{
		const FWorldChunkCoord NewChunk = FWorldChunkCoord::FromWorldLocation(WorldTransform.GetLocation());
		const FWorldChunkCoord* PreviousChunk =
			Runtime->Persistence.HomeChunks.Find(WorldEntityIdentity->WorldEntityId);
		const bool bCrossedChunk = !PreviousChunk || *PreviousChunk != NewChunk;
		if (bCrossedChunk && !bPublishStableTransform)
		{
			WorldEntityIdentity->StateRevision = NextRevision(WorldEntityIdentity->StateRevision);
		}
		if (bPublishStableTransform || bCrossedChunk)
		{
			UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
			if (!WorldStorage ||
				!WorldStorage->UpdateEntityLocation(WorldEntityIdentity->WorldEntityId, WorldTransform.GetLocation(),
													WorldEntityIdentity->StateRevision))
			{
				verify(Runtime->Core.SpatialIndex.UpdatePortable(Entity, PreviousWorldBounds));
				Transform->WorldTransform = PreviousTransform;
				Transform->Revision = PreviousTransformRevision;
				WorldEntityIdentity->StateRevision = PreviousStateRevision;
				return false;
			}
			Runtime->Persistence.HomeChunks.Add(WorldEntityIdentity->WorldEntityId, NewChunk);
		}
	}
	if (bPublishQuerySnapshot)
	{
		TOptional<FWorldObjectShapeInstanceSnapshot> CurrentShape;
		FWorldObjectShapeInstanceSnapshot Snapshot;
		if (!BuildShapeSnapshot(Entity, Snapshot))
		{
			return false;
		}
		CurrentShape = MoveTemp(Snapshot);
		if (!PublishShapeTransition(PreviousShape, CurrentShape, EWorldObjectQuerySnapshotChangeKind::Motion,
									EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
									GetEffectiveTimeMilliseconds(GetWorldRef())))
		{
			return false;
		}
	}
	return true;
}
