	struct FBuildFireShapeRoute final
	{
		EBuildFixedFireEmitterKind EmitterKind = EBuildFixedFireEmitterKind::FirePile;
		int32 BurnCustomDataIndex = INDEX_NONE;
		bool bFixedEmitter = false;
		bool bCombustible = false;

		bool IsRelevant() const { return bFixedEmitter || bCombustible; }
	};

	FBuildFireShapeRoute ResolveBuildFireShapeRoute(const FName DefinitionId) const
	{
		FBuildFireShapeRoute Route;
		Route.bFixedEmitter = TryGetBuildFixedFireEmitterKind(DefinitionId, Route.EmitterKind);
		UBuildingDefinition* Definition = Buildings.IsValid() ? Buildings->FindDefinition(DefinitionId) : nullptr;
		Route.bCombustible = Definition &&
			TryGetBuildCombustionConfiguration(*Definition, Route.BurnCustomDataIndex);
		return Route;
	}

	const FBuildFireShapeRoute& GetBuildFireShapeRoute(const FName DefinitionId)
	{
		if (const FBuildFireShapeRoute* Cached = BuildingFireShapeRoutes.Find(DefinitionId))
		{
			return *Cached;
		}
		// Entity 只有在 Definition 注册后才会进入查询快照；相同 DefinitionId 不能换绑对象，
		// 因而相关性在本 World 生命周期内稳定，可跨批次缓存空路由与有效路由。
		return BuildingFireShapeRoutes.Add(DefinitionId, ResolveBuildFireShapeRoute(DefinitionId));
	}

	bool PublishTarget(
		FHostRecord& Host,
		const int64 EffectiveTime,
		const bool bContinuousMotion,
		const FTransform& PreviousTransform,
		const EElementQueryPriority Priority)
	{
		const FElementCompoundShape Compound = Host.MakeCompoundShape();
		if (!Compound.IsValid()) return false;
		if (!Execution)
		{
			Host.LastEffectiveTimeMilliseconds = EffectiveTime;
			Host.bPublished = true;
			return true;
		}
		Host.SnapshotRevision = AdvanceNonZero(Host.SnapshotRevision);
		FElementTargetSnapshot Snapshot;
		Snapshot.Target = Host.Target;
		Snapshot.Revision = Host.SnapshotRevision;
		Snapshot.EffectiveTimeMilliseconds = EffectiveTime;
		Snapshot.Shape = Compound;
		Snapshot.Metadata = MakeFireTargetMetadata(Host.Profile, Host.bFireInteractionActive);
		if (!Execution->ReplaceTargetSnapshot(Snapshot)) return false;

		if (bContinuousMotion && Host.bPublished && EffectiveTime > Host.LastEffectiveTimeMilliseconds)
		{
			FElementMotionSubmission Motion;
			Motion.Target = Host.Target;
			Motion.ExpectedTargetRevision = Host.SnapshotRevision;
			Motion.Priority = Priority;
			FElementMotionSegment& Segment = Motion.Segments.AddDefaulted_GetRef();
			Segment.PreviousTransform = PreviousTransform;
			Segment.CurrentTransform = Host.WorldTransform;
			Segment.StartTimeMilliseconds = Host.LastEffectiveTimeMilliseconds;
			Segment.EndTimeMilliseconds = EffectiveTime;
			if (!Execution->SubmitMotion(Motion)) return false;
		}
		Host.LastEffectiveTimeMilliseconds = EffectiveTime;
		Host.bPublished = true;
		RefreshGeneratedSourceShape(Host);
		return true;
	}

	bool UpsertBuildingShape(
		const FBuildShapeInstanceSnapshot& Shape,
		const FBuildFireShapeRoute& Route,
		const int64 EffectiveTime,
		const bool bMotion,
		const FTransform& PreviousTransform,
		const bool bPublishImmediately)
	{
		if (!Shape.IsValid()) return false;
		if (Route.bFixedEmitter && Shape.ShapeRef.PartId == 0 && Execution
			&& !UpsertFixedBuildingSource(Shape, Route.EmitterKind))
		{
			return false;
		}
		if (!Route.bCombustible) return true;

		const FElementTargetKey Target = MakeTargetKey(Shape.ShapeRef.WorldEntityId, Shape.ShapeRef.Entity);
		FHostRecord& Host = Hosts.FindOrAdd(Target);
		Host.Target = Target;
		Host.Profile = EElementFireTargetProfile::Structure;
		Host.WorldEntityId = Shape.ShapeRef.WorldEntityId;
		Host.Building = Shape.ShapeRef.Entity;
		Host.DefinitionId = Shape.DefinitionId;
		Host.WorldTransform = FTransform::Identity;
		Host.BurnCustomDataIndex = Route.BurnCustomDataIndex;
		TargetByHostId.Add(Host.WorldEntityId, Target);
		const FElementShape Converted = ConvertBuildGeometryToWorld(Shape);
		if (!Converted.IsValid()) return false;
		Host.Shapes.Add(MakeBuildChildKey(Shape.ShapeRef), Converted);
		BuildingTargets.Add(Shape.ShapeRef.Entity, Target);
		if (!bPublishImmediately) return true;
		return PublishTarget(Host, EffectiveTime, false, PreviousTransform, EElementQueryPriority::Normal);
	}

	void MarkBuildingTargetDirty(
		const FBuildEntityHandle Entity,
		const int64 EffectiveTime,
		TMap<FElementTargetKey, int64>& InOutDirtyTargets) const
	{
		const FElementTargetKey* Target = BuildingTargets.Find(Entity);
		if (!Target) return;
		int64& LatestEffectiveTime = InOutDirtyTargets.FindOrAdd(*Target);
		LatestEffectiveTime = FMath::Max(LatestEffectiveTime, EffectiveTime);
	}

	bool PublishBuildingTargets(const TMap<FElementTargetKey, int64>& DirtyTargets)
	{
		for (const TPair<FElementTargetKey, int64>& Pair : DirtyTargets)
		{
			FHostRecord* Host = Hosts.Find(Pair.Key);
			if (Host && !PublishTarget(
				*Host, Pair.Value, false, FTransform::Identity, EElementQueryPriority::Normal))
			{
				return false;
			}
		}
		return true;
	}

	bool UpsertWorldObjectShape(
		const FWorldObjectShapeInstanceSnapshot& Shape,
		const int64 EffectiveTime,
		const bool bMotion,
		const FTransform& PreviousTransform)
	{
		if (!Shape.IsValid()) return false;
		EWorldObjectCombustionProfileKind Kind = EWorldObjectCombustionProfileKind::Stick;
		if (!TryGetWorldObjectCombustionProfileKind(Shape.DefinitionId, Kind)) return true;
		const FElementShape Converted = ConvertWorldObjectGeometry(Shape.LocalGeometry);
		if (!Converted.IsValid()) return false;
		const FElementTargetKey Target = MakeTargetKey(Shape.ShapeRef.WorldEntityId, Shape.ShapeRef.Entity);
		FHostRecord& Host = Hosts.FindOrAdd(Target);
		Host.Target = Target;
		Host.Profile = Kind == EWorldObjectCombustionProfileKind::Structure
			? EElementFireTargetProfile::Structure : EElementFireTargetProfile::Stick;
		Host.WorldEntityId = Shape.ShapeRef.WorldEntityId;
		Host.WorldObject = Shape.ShapeRef.Entity;
		Host.DefinitionId = Shape.DefinitionId;
		Host.Shapes.Add(0, Converted);
		Host.WorldTransform = Shape.WorldTransform;
		TargetByHostId.Add(Host.WorldEntityId, Target);
		WorldObjectTargets.Add(Shape.ShapeRef.Entity, Target);
		const EElementQueryPriority Priority = Shape.MotionState == EWorldObjectMotionState::Physics
			? EElementQueryPriority::Normal : EElementQueryPriority::Background;
		return PublishTarget(Host, EffectiveTime, bMotion, PreviousTransform, Priority);
	}

	bool UpsertCharacterSnapshot(
		const FCharacterQuerySnapshot& Snapshot,
		const bool bMotion,
		const FTransform& PreviousTransform)
	{
		if (!Snapshot.IsValid()) return false;
		const FElementTargetKey Target = MakeTargetKey(Snapshot.Handle);
		FHostRecord& Host = Hosts.FindOrAdd(Target);
		Host.Target = Target;
		Host.Profile = EElementFireTargetProfile::Character;
		Host.Character = Snapshot.Handle;
		Host.Shapes.Reset();
		const FElementCompoundShape CharacterShape = MakeCharacterShape(Snapshot);
		if (!CharacterShape.IsValid()) return false;
		Host.Shapes.Add(0, CharacterShape.Shapes[0]);
		Host.WorldTransform = CharacterShape.WorldTransform;
		CharacterTargets.Add(Snapshot.Handle, Target);
		return PublishTarget(
			Host, Snapshot.EffectiveTimeMilliseconds, bMotion, PreviousTransform,
			EElementQueryPriority::Critical);
	}

	bool UpsertFixedBuildingSource(
		const FBuildShapeInstanceSnapshot& Shape,
		const EBuildFixedFireEmitterKind Kind)
	{
		FElementCompoundShape SourceShape;
		SourceShape.WorldTransform = Shape.WorldTransform;
		const bool bFirePile = Kind == EBuildFixedFireEmitterKind::FirePile;
		const FElementFireSourceRule& SourceRule = bFirePile ? Rules.FirePile : Rules.MountedTorch;
		if (bFirePile)
		{
			SourceShape.Shapes.Add(FElementShape::MakeCapsule(
				Rules.FirePileCapsuleCenter,
				FVector::UpVector,
				Rules.FirePileCapsuleRadius,
				Rules.FirePileCapsuleSegmentHalfLength));
		}
		else
		{
			SourceShape.Shapes.Add(FElementShape::MakeSphere(
				Rules.MountedTorchSphereCenter,
				Rules.MountedTorchSphereRadius));
		}
		if (!SourceShape.IsValid()) return false;

		FFireSourceFragment Fragment;
		Fragment.Shape = MoveTemp(SourceShape);
		Fragment.HostTarget = MakeTargetKey(Shape.ShapeRef.WorldEntityId, Shape.ShapeRef.Entity);
		Fragment.Intensity = SourceRule.Intensity;
		Fragment.RangeCentimeters = SourceRule.RangeCentimeters;
		Fragment.Policy = SourceRule.Policy;
		Fragment.Revision = 1;
		FFixedSource* Existing = FixedSources.Find(Shape.ShapeRef.WorldEntityId);
		if (Existing && Execution->ReadRegistry().IsAlive(Existing->Element))
		{
			return Execution->EditFragment<FFireSourceFragment>(Existing->Element,
				[&Fragment](FFireSourceFragment& Current)
				{
					Current.Shape = Fragment.Shape;
					Current.HostTarget = Fragment.HostTarget;
					Current.Intensity = Fragment.Intensity;
					Current.RangeCentimeters = Fragment.RangeCentimeters;
					Current.Policy = Fragment.Policy;
				});
		}
		FElementEntityHandle Element = Execution->CreateElement();
		if (!Element.IsSet() || !Execution->AddFragment(Element, Fragment))
		{
			if (Element.IsSet()) Execution->DestroyElement(Element);
			return false;
		}
		FixedSources.Add(Shape.ShapeRef.WorldEntityId, {Shape.ShapeRef.Entity, Element});
		return true;
	}

	void OnBuildingBatch(const FBuildQuerySnapshotBatchRef Batch)
	{
		// Client 的 Cold Building 不需要 Fire Host/BVH；有状态的 Dependent Element 会按 HostId
		// 精确物化并登记 BuildingTargets。Authority 仍按预算消费完整的宿主输入。
		if (!Batch->Changes.IsEmpty() && (Execution || BuildingTargets.Num() > 0))
		{
			FPendingBuildingSnapshotBatch& Pending = PendingBuildingSnapshotBatches.AddDefaulted_GetRef();
			Pending.Batch = Batch;
		}
	}

	void PumpBuildingSnapshotBatches()
	{
		if (!PendingBuildingSnapshotBatches.IsValidIndex(PendingBuildingSnapshotBatchCursor)) return;
		TRACE_CPUPROFILER_EVENT_SCOPE(ElementFire_BuildingSnapshotPump);
		constexpr double ApplyBudgetMilliseconds = 2.0;
		constexpr int32 MaximumEntityGroupsPerPump = 256;
		const double StartSeconds = FPlatformTime::Seconds();
		FName CachedDefinitionId = NAME_None;
		FBuildFireShapeRoute CachedRoute;
		bool bHasCachedRoute = false;
		if (Execution) Execution->BeginTargetSnapshotBatch();
		int32 ProcessedEntityGroups = 0;
		bool bBudgetExhausted = false;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ElementFire_BuildingSnapshotPump_ApplyChanges);
			while (!bBudgetExhausted &&
				PendingBuildingSnapshotBatches.IsValidIndex(PendingBuildingSnapshotBatchCursor))
			{
				FPendingBuildingSnapshotBatch& Pending =
					PendingBuildingSnapshotBatches[PendingBuildingSnapshotBatchCursor];
				if (!Pending.Batch.IsValid())
				{
					++PendingBuildingSnapshotBatchCursor;
					continue;
				}
				const TArray<FBuildQuerySnapshotChange>& Changes = Pending.Batch->Changes;
				while (Changes.IsValidIndex(Pending.ChangeCursor))
				{
					const FBuildQuerySnapshotChange& Change = Changes[Pending.ChangeCursor];
					if (!Pending.ActiveEntity.IsSet()) Pending.ActiveEntity = Change.Entity;
					check(Pending.ActiveEntity == Change.Entity);
					const bool bNeedsFireProjection = Execution || BuildingTargets.Contains(Change.Entity);
					if (Change.Kind != EBuildQuerySnapshotChangeKind::LeaveInterest && bNeedsFireProjection)
					{
						const FBuildShapeInstanceSnapshot& Shape = Change.Current.IsSet()
							? Change.Current.GetValue() : Change.Previous.GetValue();
						if (!bHasCachedRoute || CachedDefinitionId != Shape.DefinitionId)
						{
							CachedDefinitionId = Shape.DefinitionId;
							CachedRoute = GetBuildFireShapeRoute(CachedDefinitionId);
							bHasCachedRoute = true;
						}
						if (!CachedRoute.IsRelevant())
						{
							if (BuildingTargets.Contains(Change.Entity) || FixedSources.Contains(Change.WorldEntityId))
							{
								RemoveBuildingShape(Change, false);
								Pending.bActiveEntityDirty = true;
							}
						}
						else if (Change.Current.IsSet())
						{
							if ((!Buildings.IsValid() || Buildings->IsEntityAlive(Change.Entity)) &&
								UpsertBuildingShape(
									Change.Current.GetValue(), CachedRoute, Change.EffectiveTimeMilliseconds,
									Change.Kind == EBuildQuerySnapshotChangeKind::Motion,
									Change.Previous.IsSet() ? Change.Previous->WorldTransform
										: Change.Current->WorldTransform,
									false))
							{
								Pending.bActiveEntityDirty = true;
							}
						}
						else
						{
							RemoveBuildingShape(Change, false);
							Pending.bActiveEntityDirty = true;
						}
						Pending.ActiveEntityEffectiveTimeMilliseconds = FMath::Max(
							Pending.ActiveEntityEffectiveTimeMilliseconds, Change.EffectiveTimeMilliseconds);
					}

					++Pending.ChangeCursor;
					const bool bEntityComplete = !Changes.IsValidIndex(Pending.ChangeCursor) ||
						Changes[Pending.ChangeCursor].Entity != Pending.ActiveEntity;
					if (bEntityComplete)
					{
						if (Pending.bActiveEntityDirty)
						{
							const FElementTargetKey* Target = BuildingTargets.Find(Pending.ActiveEntity);
							FHostRecord* Host = Target ? Hosts.Find(*Target) : nullptr;
							if (Host)
							{
								PublishTarget(
									*Host, Pending.ActiveEntityEffectiveTimeMilliseconds, false,
									FTransform::Identity, EElementQueryPriority::Normal);
							}
						}
						Pending.ActiveEntity = {};
						Pending.ActiveEntityEffectiveTimeMilliseconds = 0;
						Pending.bActiveEntityDirty = false;
						++ProcessedEntityGroups;
					}
					bBudgetExhausted = ProcessedEntityGroups >= MaximumEntityGroupsPerPump ||
						(FPlatformTime::Seconds() - StartSeconds) * 1000.0 >= ApplyBudgetMilliseconds;
					if (bBudgetExhausted) break;
				}
				if (!Changes.IsValidIndex(Pending.ChangeCursor))
				{
					check(!Pending.ActiveEntity.IsSet());
					++PendingBuildingSnapshotBatchCursor;
				}
			}
		}
		if (Execution)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ElementFire_BuildingSnapshotPump_EndTargetBatch);
			Execution->EndTargetSnapshotBatch();
		}
		if (!PendingBuildingSnapshotBatches.IsValidIndex(PendingBuildingSnapshotBatchCursor))
		{
			// 最后一份 ThreadSafe SharedPtr 可能拥有百万级 Changes。把已消费批次整体
			// 转交 Worker 析构，避免 TArray/Optional 链在预算 Pump 结束后集中阻塞 GT。
			TArray<FPendingBuildingSnapshotBatch> RetiredBatches =
				MoveTemp(PendingBuildingSnapshotBatches);
			PendingBuildingSnapshotBatchCursor = 0;
			AsyncTask(
				ENamedThreads::AnyBackgroundThreadNormalTask,
				[RetiredBatches = MoveTemp(RetiredBatches)]() mutable
				{
					RetiredBatches.Reset();
				});
		}
	}

	void OnWorldObjectBatch(const FWorldObjectQuerySnapshotBatch& Batch)
	{
		if (Execution) Execution->BeginTargetSnapshotBatch();
		for (const FWorldObjectQuerySnapshotChange& Change : Batch.Changes)
		{
			if (Change.Kind == EWorldObjectQuerySnapshotChangeKind::LeaveInterest) continue;
			if (Change.Current.IsSet())
			{
				const FTransform Previous = Change.Previous.IsSet()
					? Change.Previous->WorldTransform : Change.Current->WorldTransform;
				UpsertWorldObjectShape(
					Change.Current.GetValue(), Change.EffectiveTimeMilliseconds,
					Change.Kind == EWorldObjectQuerySnapshotChangeKind::Motion, Previous);
				continue;
			}
			RemoveWorldObject(Change);
		}
		if (Execution) Execution->EndTargetSnapshotBatch();
	}

	void OnCharacterBatch(const FCharacterQuerySnapshotBatch& Batch)
	{
		if (Execution) Execution->BeginTargetSnapshotBatch();
		for (const FCharacterQuerySnapshotChange& Change : Batch.Changes)
		{
			if (Change.Current.IsSet())
			{
				const FTransform Previous = Change.Previous.IsSet()
					? Change.Previous->WorldTransform : Change.Current->WorldTransform;
				UpsertCharacterSnapshot(
					Change.Current.GetValue(), Change.Kind == ECharacterQuerySnapshotChangeKind::Motion, Previous);
			}
			else
			{
				const FElementTargetKey* Target = CharacterTargets.Find(Change.Handle);
				if (Target) RemoveTarget(*Target, EElementTargetRemovalReason::GameplayDestroy);
			}
		}
		if (Execution) Execution->EndTargetSnapshotBatch();
	}

	void RemoveBuildingShape(const FBuildQuerySnapshotChange& Change, const bool bPublishImmediately)
	{
		const bool bRemovesEmitter = Change.Kind != EBuildQuerySnapshotChangeKind::ShapeRemove
			|| (Change.Previous.IsSet() && Change.Previous->ShapeRef.PartId == 0);
		if (bRemovesEmitter)
		{
			if (FFixedSource* Fixed = FixedSources.Find(Change.WorldEntityId))
			{
				if (Execution) Execution->DestroyElement(Fixed->Element);
				FixedSources.Remove(Change.WorldEntityId);
			}
		}
		const FElementTargetKey* Target = BuildingTargets.Find(Change.Entity);
		FHostRecord* Host = Target ? Hosts.Find(*Target) : nullptr;
		if (!Host) return;
		if (Change.Kind == EBuildQuerySnapshotChangeKind::ShapeRemove && Change.Previous.IsSet())
		{
			Host->Shapes.Remove(MakeBuildChildKey(Change.Previous->ShapeRef));
			if (!Host->Shapes.IsEmpty())
			{
				if (bPublishImmediately)
				{
					PublishTarget(*Host, Change.EffectiveTimeMilliseconds, false, FTransform::Identity,
						EElementQueryPriority::Normal);
				}
				return;
			}
		}
		RemoveTarget(*Target,
			Change.Kind == EBuildQuerySnapshotChangeKind::GameplayDestroy
				? EElementTargetRemovalReason::GameplayDestroy : EElementTargetRemovalReason::RuntimeEvict);
	}

	void RemoveWorldObject(const FWorldObjectQuerySnapshotChange& Change)
	{
		const FElementTargetKey* Target = WorldObjectTargets.Find(Change.Entity);
		if (!Target) return;
		RemoveTarget(*Target,
			Change.Kind == EWorldObjectQuerySnapshotChangeKind::GameplayDestroy
				? EElementTargetRemovalReason::GameplayDestroy : EElementTargetRemovalReason::RuntimeEvict);
	}

	FPersistentBinding* FindPersistentBinding(const FElementTargetKey Target)
	{
		const FWorldEntityId* ElementId = PersistentIdByTarget.Find(Target);
		return ElementId ? PersistentBindings.Find(*ElementId) : nullptr;
	}

	const FPersistentBinding* FindPersistentBinding(const FElementTargetKey Target) const
	{
		const FWorldEntityId* ElementId = PersistentIdByTarget.Find(Target);
		return ElementId ? PersistentBindings.Find(*ElementId) : nullptr;
	}

	/**
	 * WorldStorage 在同一注入帧先恢复 Primary Building、再恢复 Dependent Element。
	 * Authority 的大批宿主快照仍走预算 Pump；Client 的 Cold Building 不建 Fire Host，
	 * 这里只按 HostId 精确编译依附记录实际需要的单个 Building。
	 */
	bool MaterializeBuildingHostForDependency(const FWorldEntityId HostId, const int64 EffectiveTimeMilliseconds)
	{
		if (FindHostByWorldId(HostId)) return true;
		if (!Buildings.IsValid()) return false;
		const FBuildEntityHandle Entity = Buildings->FindEntity(HostId);
		TArray<FBuildShapeInstanceSnapshot> Shapes;
		if (!Entity.IsSet() || !Buildings->CopyEntityShapeSnapshots(Entity, Shapes)) return false;

		TMap<FElementTargetKey, int64> DirtyTargets;
		if (Execution) Execution->BeginTargetSnapshotBatch();
		bool bSucceeded = true;
		for (const FBuildShapeInstanceSnapshot& Shape : Shapes)
		{
			const FBuildFireShapeRoute& Route = GetBuildFireShapeRoute(Shape.DefinitionId);
			if (!Route.IsRelevant()) continue;
			if (!UpsertBuildingShape(
				Shape, Route, EffectiveTimeMilliseconds, false, Shape.WorldTransform, false))
			{
				bSucceeded = false;
				break;
			}
			MarkBuildingTargetDirty(Entity, EffectiveTimeMilliseconds, DirtyTargets);
		}
		if (bSucceeded) bSucceeded = PublishBuildingTargets(DirtyTargets);
		if (Execution) Execution->EndTargetSnapshotBatch();
		return bSucceeded && FindHostByWorldId(HostId) != nullptr;
	}

	FHostRecord* FindHostByWorldId(const FWorldEntityId HostId)
	{
		const FElementTargetKey* Target = TargetByHostId.Find(HostId);
		return Target ? Hosts.Find(*Target) : nullptr;
	}

	static FVector GetHostStorageLocation(const FHostRecord& Host)
	{
		const FBox Bounds = Host.MakeCompoundShape().CalculateWorldBounds();
		return Bounds.IsValid != 0 ? Bounds.GetCenter() : Host.WorldTransform.GetLocation();
	}

	static EElementFirePersistentHostDomain GetPersistentHostDomain(const FHostRecord& Host)
	{
		return Host.Target.Domain == EElementTargetDomain::Building
			? EElementFirePersistentHostDomain::Building
			: EElementFirePersistentHostDomain::WorldObject;
	}

	static bool ShouldPersistAuthorityState(const FElementAuthorityTargetStateSnapshot& Snapshot)
	{
		for (const FElementStateValue& State : Snapshot.StateValues)
		{
			if (State.SchemaId != ElementFireRuntimeNames::ThermalState || State.Payload.Count != 8) continue;
			const EFireCombustionPhase Phase = static_cast<EFireCombustionPhase>(
				FMath::Clamp(FMath::RoundToInt(State.Payload.Values[0]), 0,
					static_cast<int32>(EFireCombustionPhase::BurnedOut)));
			return Phase != EFireCombustionPhase::Cold
				|| State.Payload.Values[1] > UE_DOUBLE_SMALL_NUMBER
				|| State.Payload.Values[7] > UE_DOUBLE_SMALL_NUMBER
				|| !Snapshot.Wakes.IsEmpty();
		}
		return false;
	}

	FPersistentBinding* EnsurePersistentBinding(FHostRecord& Host)
	{
		if (Host.Target.Domain == EElementTargetDomain::Character || !Host.WorldEntityId.IsSet()
			|| !Execution || !Storage.IsValid()) return nullptr;
		if (FPersistentBinding* Existing = FindPersistentBinding(Host.Target)) return Existing;

		const FWorldEntityId ElementId = Storage->AllocateEntityId();
		const FElementEntityHandle Element = Execution->CreateElement(ElementId);
		if (!ElementId.IsSet() || !Element.IsSet()) return nullptr;
		FPersistentBinding Binding;
		Binding.ElementId = ElementId;
		Binding.HostId = Host.WorldEntityId;
		Binding.Target = Host.Target;
		Binding.Element = Element;
		Binding.HomeChunk = FWorldChunkCoord::FromWorldLocation(GetHostStorageLocation(Host));
		Binding.StateRevision = 1;
		FWorldResidentEntityRegistration Registration;
		Registration.EntityId = ElementId;
		Registration.Domain = EWorldEntityDomain::Element;
		Registration.HomeChunk = Binding.HomeChunk;
		Registration.StateRevision = Binding.StateRevision;
		if (Storage->RegisterResidentEntity(Registration) != EWorldResidentUpsertResult::Inserted)
		{
			Execution->DestroyElement(Element);
			return nullptr;
		}
		PersistentIdByTarget.Add(Host.Target, ElementId);
		PersistentBindings.Add(ElementId, MoveTemp(Binding));
		return PersistentBindings.Find(ElementId);
	}

	bool MarkPersistentBindingDirty(FPersistentBinding& Binding, const FHostRecord& Host)
	{
		if (!Storage.IsValid()) return false;
		Binding.StateRevision = AdvancePersistentRevision(Binding.StateRevision);
		const FVector Location = GetHostStorageLocation(Host);
		const FWorldChunkCoord NewChunk = FWorldChunkCoord::FromWorldLocation(Location);
		const bool bSucceeded = NewChunk == Binding.HomeChunk
			? Storage->MarkEntityDirty(Binding.ElementId, Binding.StateRevision)
			: Storage->UpdateEntityLocation(Binding.ElementId, Location, Binding.StateRevision);
		if (bSucceeded) Binding.HomeChunk = NewChunk;
		return bSucceeded;
	}

	void RemovePersistentBindingRuntime(const FElementTargetKey Target)
	{
		const FWorldEntityId* FoundId = PersistentIdByTarget.Find(Target);
		if (!FoundId) return;
		const FWorldEntityId ElementId = *FoundId;
		FPersistentBinding Binding;
		if (PersistentBindings.RemoveAndCopyValue(ElementId, Binding) && Execution)
		{
			Execution->RemoveTargetState(Target);
			if (Execution->ReadRegistry().IsAlive(Binding.Element)) Execution->DestroyElement(Binding.Element);
		}
		PersistentIdByTarget.Remove(Target);
		PendingPersistenceDirty.Remove(Target);
	}

	bool DestroyPersistentBinding(const FElementTargetKey Target, const bool bCreateTombstone)
	{
		FPersistentBinding* Binding = FindPersistentBinding(Target);
		if (!Binding) return true;
		if (bCreateTombstone && Storage.IsValid())
		{
			const uint32 TombstoneRevision = AdvancePersistentRevision(Binding->StateRevision);
			if (!Storage->GameplayDestroy(Binding->ElementId, TombstoneRevision)) return false;
		}
		RemovePersistentBindingRuntime(Target);
		return true;
	}

	void RemoveTarget(const FElementTargetKey Target, const EElementTargetRemovalReason Reason)
	{
		FHostRecord* Host = Hosts.Find(Target);
		if (!Host) return;
		if (Reason == EElementTargetRemovalReason::GameplayDestroy)
		{
			if (Execution && !DestroyPersistentBinding(Target, true))
			{
				UE_LOG(LogTemp, Error, TEXT("宿主 GameplayDestroy 时 Element Tombstone 提交失败。"));
			}
			// Client 只消费网络 Tombstone，不能反向调用 Authority WorldStorage。
			// Host 先退场时暂留 Dependent Binding，随后 Element Tombstone 负责清理它。
		}
		else
		{
			// 正常路径由 Dependent Element Adapter 先 Capture/Evict；这里同时处理无持久状态的宿主。
			RemovePersistentBindingRuntime(Target);
		}
		if (Execution) Execution->RemoveTargetSnapshot(Target, Host->SnapshotRevision, Reason);
		RemoveProjection(*Host, Reason == EElementTargetRemovalReason::GameplayDestroy
			? EElementVisualChangeKind::GameplayDestroy : EElementVisualChangeKind::RuntimeEvict);
		if (Host->Building.IsSet()) BuildingTargets.Remove(Host->Building);
		if (Host->WorldObject.IsSet()) WorldObjectTargets.Remove(Host->WorldObject);
		if (Host->Character.IsSet()) CharacterTargets.Remove(Host->Character);
		if (Host->WorldEntityId.IsSet()) TargetByHostId.Remove(Host->WorldEntityId);
			PendingBurnoutConversions.Remove(Target);
			BurnPresentations.Remove(Target);
			Hosts.Remove(Target);
	}

	void RefreshGeneratedSourceShape(FHostRecord& Host)
	{
		FPersistentBinding* Binding = FindPersistentBinding(Host.Target);
		if (!Binding || !Binding->bHasFireSource || !Execution
			|| !Execution->ReadRegistry().IsAlive(Binding->Element)) return;
		const FFireSourceFragment* Existing = Execution->ReadRegistry().FindFragment<FFireSourceFragment>(
			Binding->Element);
		if (!Existing) return;
			const FElementCompoundShape Shape = Host.MakeCompoundShape();
		if (Execution->EditFragment<FFireSourceFragment>(Binding->Element,
			[&Shape](FFireSourceFragment& Fragment){ Fragment.Shape = Shape; }))
		{
			MarkPersistentBindingDirty(*Binding, Host);
		}
	}

	const FFireCombustionProfile* SelectProfile(const EElementFireTargetProfile Profile) const
	{
		switch (Profile)
		{
		case EElementFireTargetProfile::Structure: return &Rules.Structure;
		case EElementFireTargetProfile::Stick: return &Rules.Stick;
		case EElementFireTargetProfile::Character: return &Rules.Character;
		default: return nullptr;
		}
	}
