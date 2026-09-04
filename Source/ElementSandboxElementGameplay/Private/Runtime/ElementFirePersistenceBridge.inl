	bool CapturePersistentState(
		const TConstArrayView<FWorldEntityId> EntityIds,
		TArray<FElementFirePersistentRecord>& OutRecords,
		FString& OutError) const
	{
		OutRecords.Reset();
		if (!Execution)
		{
			OutError = TEXT("Client Element 投影不允许捕获 Authority 存档状态。");
			return false;
		}
		OutRecords.Reserve(EntityIds.Num());
		for (const FWorldEntityId EntityId : EntityIds)
		{
			const FPersistentBinding* Binding = PersistentBindings.Find(EntityId);
			const FHostRecord* Host = Binding ? Hosts.Find(Binding->Target) : nullptr;
			if (!Binding || !Host || !Execution->ReadRegistry().IsAlive(Binding->Element))
			{
				OutError = TEXT("Element Capture 找不到有效持久绑定或宿主。");
				OutRecords.Reset();
				return false;
			}
			FElementFirePersistentRecord& Record = OutRecords.AddDefaulted_GetRef();
			Record.ElementId = Binding->ElementId;
			Record.HostId = Binding->HostId;
			Record.HostDomain = GetPersistentHostDomain(*Host);
			Record.WorldLocation = GetHostStorageLocation(*Host);
			Record.StateRevision = Binding->StateRevision;
			if (!Execution->CaptureTargetState(Binding->Target, Record.AuthorityState))
			{
				OutError = TEXT("Element Capture 无法读取稳定 Authority Target State。");
				OutRecords.Reset();
				return false;
			}
			if (Binding->bHasFireSource)
			{
				const FFireSourceFragment* Fragment = Execution->ReadRegistry().FindFragment<FFireSourceFragment>(
					Binding->Element);
				if (!Fragment || !Fragment->IsValid())
				{
					OutError = TEXT("Element Capture 的 Fire Source 绑定与 Fragment 不一致。");
					OutRecords.Reset();
					return false;
				}
				FElementFirePersistentSource Source;
				Source.Intensity = Fragment->Intensity;
				Source.RangeCentimeters = Fragment->RangeCentimeters;
				Source.Policy = static_cast<uint8>(Fragment->Policy);
				Source.ExpireTimeMilliseconds = Fragment->ExpireTimeMilliseconds;
				Record.Source = Source;
			}
			if (!Record.IsValid())
			{
				OutError = TEXT("Element Capture 生成了非法纯值记录。");
				OutRecords.Reset();
				return false;
			}
		}
		return true;
	}

	bool RestorePersistentState(
		const FWorldChunkCoord& HomeChunk,
		const TConstArrayView<FElementFirePersistentRecord> Records,
		FString& OutError)
	{
		TArray<FHostRecord*> RestoreHosts;
		TArray<FElementAuthorityTargetStateSnapshot> RestoreStates;
		TSet<FWorldEntityId> SeenIds;
		TSet<FElementTargetKey> SeenTargets;
		RestoreHosts.Reserve(Records.Num());
		RestoreStates.Reserve(Records.Num());
		for (const FElementFirePersistentRecord& Record : Records)
		{
			FHostRecord* Host = FindHostByWorldId(Record.HostId);
			if (!Host && Record.HostDomain == EElementFirePersistentHostDomain::Building)
			{
				MaterializeBuildingHostForDependency(
					Record.HostId,
					GetAuthorityTime(Storage.Get(), Owner.IsValid() ? Owner->GetWorld() : nullptr));
				Host = FindHostByWorldId(Record.HostId);
			}
			FElementAuthorityTargetStateSnapshot State = Record.AuthorityState;
			if (Host) State.Target = Host->Target;
			const bool bHostDomainMatches = Host &&
				((Record.HostDomain == EElementFirePersistentHostDomain::Building
					&& Host->Target.Domain == EElementTargetDomain::Building)
				|| (Record.HostDomain == EElementFirePersistentHostDomain::WorldObject
					&& Host->Target.Domain == EElementTargetDomain::WorldObject));
			const FPersistentBinding* ExistingById = PersistentBindings.Find(Record.ElementId);
			const FWorldEntityId* ExistingForTarget = Host ? PersistentIdByTarget.Find(Host->Target) : nullptr;
			const FElementEntityHandle RegistryEntity = Execution
				? Execution->ReadRegistry().FindByPersistentId(Record.ElementId) : FElementEntityHandle();
			const int32 Policy = Record.Source.IsSet() ? static_cast<int32>(Record.Source->Policy) : 0;
			if (!Record.ElementId.IsSet() || !Record.HostId.IsSet() || Record.StateRevision == 0
				|| Record.WorldLocation.ContainsNaN()
				|| FWorldChunkCoord::FromWorldLocation(Record.WorldLocation) != HomeChunk
				|| !bHostDomainMatches || SeenIds.Contains(Record.ElementId) || SeenTargets.Contains(Host->Target)
				|| (ExistingById && ExistingById->Target != Host->Target)
				|| (ExistingById && ExistingById->StateRevision > Record.StateRevision)
				|| (ExistingForTarget && *ExistingForTarget != Record.ElementId)
				|| (RegistryEntity.IsSet() && (!ExistingById || ExistingById->Element != RegistryEntity))
				|| (Record.Source.IsSet()
					&& (Policy < static_cast<int32>(EFirePropagationPolicy::All)
						|| Policy > static_cast<int32>(EFirePropagationPolicy::CharacterOnly)))
				|| !ValidateRestoredFireState(State, OutError))
			{
				if (OutError.IsEmpty()) OutError = TEXT("Element Restore 全批预检失败。");
				return false;
			}
			if (ExistingById && Execution)
			{
				const bool bHasFragment = Execution->ReadRegistry().FindFragment<FFireSourceFragment>(
					ExistingById->Element) != nullptr;
				if (bHasFragment != ExistingById->bHasFireSource)
				{
					OutError = TEXT("Element Restore 发现持久绑定与 Fragment Pool 不一致。");
					return false;
				}
			}
			SeenIds.Add(Record.ElementId);
			SeenTargets.Add(Host->Target);
			RestoreHosts.Add(Host);
			RestoreStates.Add(MoveTemp(State));
		}

		TMap<FWorldEntityId, FElementEntityHandle> PreparedElements;
		if (Execution)
		{
			for (int32 Index = 0; Index < Records.Num(); ++Index)
			{
				if (PersistentBindings.Contains(Records[Index].ElementId)) continue;
				const FElementEntityHandle Element = Execution->CreateElement(Records[Index].ElementId);
				if (!Element.IsSet())
				{
					for (const TPair<FWorldEntityId, FElementEntityHandle>& Pair : PreparedElements)
						Execution->DestroyElement(Pair.Value);
					OutError = TEXT("Element Restore 无法分配 Generation-safe Element Entity。");
					return false;
				}
				PreparedElements.Add(Records[Index].ElementId, Element);
			}
			TArray<FElementAuthorityTargetStateSnapshot> Backups;
			Backups.SetNum(Records.Num());
			for (int32 Index = 0; Index < Records.Num(); ++Index)
			{
				Execution->CaptureTargetState(RestoreHosts[Index]->Target, Backups[Index]);
				if (!Execution->RestoreTargetState(RestoreStates[Index], &OutError))
				{
					for (int32 RollbackIndex = 0; RollbackIndex < Index; ++RollbackIndex)
						Execution->RestoreTargetState(Backups[RollbackIndex]);
					for (const TPair<FWorldEntityId, FElementEntityHandle>& Pair : PreparedElements)
						Execution->DestroyElement(Pair.Value);
					return false;
				}
			}
		}

		for (int32 Index = 0; Index < Records.Num(); ++Index)
		{
			const FElementFirePersistentRecord& Record = Records[Index];
			FHostRecord& Host = *RestoreHosts[Index];
			FPersistentBinding* Binding = PersistentBindings.Find(Record.ElementId);
			if (!Binding)
			{
				FPersistentBinding NewBinding;
				NewBinding.ElementId = Record.ElementId;
				NewBinding.HostId = Record.HostId;
				NewBinding.Target = Host.Target;
				NewBinding.Element = PreparedElements.FindRef(Record.ElementId);
				NewBinding.HomeChunk = HomeChunk;
				NewBinding.StateRevision = Record.StateRevision;
				PersistentBindings.Add(Record.ElementId, MoveTemp(NewBinding));
				PersistentIdByTarget.Add(Host.Target, Record.ElementId);
				Binding = PersistentBindings.Find(Record.ElementId);
			}
			else
			{
				Binding->HostId = Record.HostId;
				Binding->Target = Host.Target;
				Binding->HomeChunk = HomeChunk;
				Binding->StateRevision = Record.StateRevision;
			}

			if (Execution)
			{
				if (Record.Source.IsSet())
				{
					FFireSourceFragment Fragment;
					Fragment.Shape = Host.MakeCompoundShape();
					Fragment.Intensity = Record.Source->Intensity;
					Fragment.RangeCentimeters = Record.Source->RangeCentimeters;
					Fragment.Policy = static_cast<EFirePropagationPolicy>(Record.Source->Policy);
					Fragment.ExpireTimeMilliseconds = Record.Source->ExpireTimeMilliseconds;
					Fragment.HostTarget = Host.Target;
					if (Binding->bHasFireSource)
					{
						Execution->EditFragment<FFireSourceFragment>(Binding->Element,
							[&Fragment](FFireSourceFragment& Current)
							{
								const uint64 Revision = Current.Revision;
								Current = Fragment;
								Current.Revision = Revision;
							});
					}
					else
					{
						Execution->AddFragment(Binding->Element, Fragment);
					}
					Binding->bHasFireSource = true;
				}
				else if (Binding->bHasFireSource)
				{
					Execution->RemoveFragment<FFireSourceFragment>(Binding->Element);
					Binding->bHasFireSource = false;
				}
			}
			else
			{
				Binding->bHasFireSource = Record.Source.IsSet();
			}
			const int64 ProjectionReferenceTime = Execution
				? GetAuthorityTime(Storage.Get(), Owner.IsValid() ? Owner->GetWorld() : nullptr)
				: RestoreStates[Index].LastSettlementMilliseconds;
			ApplyProjectionCommand(MakeProjectionFromState(
				RestoreStates[Index], ProjectionReferenceTime));
		}
		return true;
	}

	bool RemovePersistentState(
		const FWorldChunkCoord& HomeChunk,
		const TConstArrayView<FWorldEntityId> EntityIds,
		const EElementPersistentRemovalSemantic Semantic,
		FString& OutError)
	{
		TSet<FWorldEntityId> Seen;
		for (const FWorldEntityId EntityId : EntityIds)
		{
			const FPersistentBinding* Binding = PersistentBindings.Find(EntityId);
			if (!Binding || Binding->HomeChunk != HomeChunk || Seen.Contains(EntityId))
			{
				OutError = TEXT("Element 移除批次含未知、重复或错误 HomeChunk 的 Entity。");
				return false;
			}
			Seen.Add(EntityId);
		}
		const EElementVisualChangeKind VisualKind = Semantic == EElementPersistentRemovalSemantic::GameplayDestroy
			? EElementVisualChangeKind::GameplayDestroy : EElementVisualChangeKind::RuntimeEvict;
		for (const FWorldEntityId EntityId : EntityIds)
		{
			const FPersistentBinding* Binding = PersistentBindings.Find(EntityId);
			const FElementTargetKey Target = Binding->Target;
			if (FHostRecord* Host = Hosts.Find(Target)) RemoveProjection(*Host, VisualKind);
			RemovePersistentBindingRuntime(Target);
		}
		return true;
	}

	bool CanRuntimeEvictPersistentState(const FWorldEntityId EntityId) const
	{
		return PersistentBindings.Contains(EntityId);
	}
