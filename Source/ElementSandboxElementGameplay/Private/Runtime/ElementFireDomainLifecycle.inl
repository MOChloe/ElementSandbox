	bool Initialize()
	{
		check(IsInGameThread());
		if (bInitialized || !Owner.IsValid()) return false;
		UWorld* World = Owner->GetWorld();
		if (!World) return false;
		Simulation = World->GetSubsystem<UElementSimulationSubsystem>();
		Buildings = World->GetSubsystem<UBuildingWorldSubsystem>();
		WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
		Characters = World->GetSubsystem<UCharacterQuerySnapshotSubsystem>();
		Trees = World->GetSubsystem<USettlementTreeWorldSubsystem>();
		Storage = World->GetSubsystem<UWorldStorageSubsystem>();
		if (!Simulation.IsValid() || !Buildings.IsValid() || !WorldObjects.IsValid()
			|| !Characters.IsValid() || !Trees.IsValid() || !Storage.IsValid()) return false;

		RuleAsset.Reset(LoadObject<UElementFireRuleSet>(nullptr, UElementFireRuleSet::GetDefaultAssetPath()));
		FString Error;
		if (!RuleAsset.IsValid() || !RuleAsset->Freeze(Rules, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("Element Fire RuleSet 加载失败：%s"), *Error);
			return false;
		}
		if (!UElementCharacterBurningEffect::ConfigureRuntimeRules(
			static_cast<float>(Rules.BaseDamagePerPeriod),
			static_cast<float>(Rules.DamagePeriodSeconds)))
		{
			return false;
		}

		BuildingChangesHandle = Buildings->OnQuerySnapshotBatchCommitted().AddRaw(
			this, &FElementFireDomainData::OnBuildingBatch);
		WorldObjectChangesHandle = WorldObjects->OnQuerySnapshotBatchCommitted().AddRaw(
			this, &FElementFireDomainData::OnWorldObjectBatch);
		CharacterChangesHandle = Characters->OnSnapshotsCommitted().AddRaw(
			this, &FElementFireDomainData::OnCharacterBatch);
		StorageAdapter = MakeElementWorldStorageAdapter(*DomainOwner);
		if (!StorageAdapter.IsValid() || !Storage->RegisterDomainAdapter(StorageAdapter.ToSharedRef()))
		{
			Shutdown();
			return false;
		}
		PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(
			this, &FElementFireDomainData::OnWorldPostActorTick);

		// Client 只消费服务器投影；不创建 BVH、查询任务或 Gameplay Processor。
		if (World->GetNetMode() == NM_Client)
		{
			if (!LoadInitialSnapshots(GetAuthorityTime(Storage.Get(), World)))
			{
				Shutdown();
				return false;
			}
			bInitialized = true;
			return true;
		}

		if (!Simulation->ActivateAuthorityExecution()) return false;
		Execution = Simulation->GetAuthorityExecution();
		if (!Execution
			|| !Execution->RegisterNumericProcessor(MakeUnique<FElementFireNumericProcessor>())
			|| !Execution->RegisterStateProcessor(MakeUnique<FElementThermalStateProcessor>(Rules))
			|| !Execution->ValidateProcessorRegistry(&Error))
		{
			UE_LOG(LogTemp, Error, TEXT("Element Processor 注册失败：%s"), *Error);
			Shutdown();
			return false;
		}

		const int64 Now = GetAuthorityTime(Storage.Get(), World);
		if (!LoadInitialSnapshots(Now))
		{
			Shutdown();
			return false;
		}
		AuthorityStepHandle = Storage->OnAuthorityStep().AddRaw(
			this, &FElementFireDomainData::OnAuthorityStep);
		bInitialized = true;
		return true;
	}

	void Shutdown()
	{
		check(IsInGameThread());
		if (Storage.IsValid() && AuthorityStepHandle.IsValid())
		{
			Storage->OnAuthorityStep().Remove(AuthorityStepHandle);
		}
		if (PostActorTickHandle.IsValid())
		{
			FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
		}
		if (Buildings.IsValid() && Buildings->HasRuntimeState() && BuildingChangesHandle.IsValid())
		{
			Buildings->OnQuerySnapshotBatchCommitted().Remove(BuildingChangesHandle);
		}
		if (WorldObjects.IsValid() && WorldObjects->HasRuntimeState() && WorldObjectChangesHandle.IsValid())
		{
			WorldObjects->OnQuerySnapshotBatchCommitted().Remove(WorldObjectChangesHandle);
		}
		if (Characters.IsValid() && Characters->HasRuntimeState() && CharacterChangesHandle.IsValid())
		{
			Characters->OnSnapshotsCommitted().Remove(CharacterChangesHandle);
			for (const TPair<FElementTargetKey, FActiveGameplayEffectHandle>& Pair : CharacterEffects)
			{
				if (const FHostRecord* Host = Hosts.Find(Pair.Key))
				{
					if (UAbilitySystemComponent* ASC = Characters->ResolveAbilitySystem(Host->Character))
					{
						RemoveAllCharacterBurningEffects(*ASC);
					}
				}
			}
		}
		if (Storage.IsValid() && StorageAdapter.IsValid())
		{
			Storage->UnregisterDomainAdapter(EWorldEntityDomain::Element, *StorageAdapter);
		}
		if (Simulation.IsValid() && Execution) Simulation->DeactivateAuthorityExecution();
		Execution = nullptr;
		Hosts.Reset();
		BuildingTargets.Reset();
		WorldObjectTargets.Reset();
		CharacterTargets.Reset();
		FixedSources.Reset();
		BuildingFireShapeRoutes.Reset();
		PersistentBindings.Reset();
		PersistentIdByTarget.Reset();
		TargetByHostId.Reset();
		PendingPersistenceDirty.Reset();
		PendingBurnoutConversions.Reset();
		RuntimeSources.Reset();
		RuntimeSourceExpiries.Reset();
		CharacterEffects.Reset();
		VisualShards.Reset();
		BurnPresentations.Reset();
		PendingBuildingSnapshotBatches.Reset();
		PendingBuildingSnapshotBatchCursor = 0;
		RuleAsset.Reset();
		StorageAdapter.Reset();
		bInitialized = false;
	}

	bool LoadInitialSnapshots(const int64 Now)
	{
		constexpr int32 PageSize = 4096;
		// Client 不为全部 Cold Building 建 Fire Host；有持久 Element 状态时由
		// Dependent Restore 按 HostId 精确物化。Authority 才需要完整 Target 输入。
		for (int32 Offset = 0; Execution;)
		{
			FBuildQuerySnapshotPage Page;
			if (!Buildings->CopyQuerySnapshotPage(Offset, PageSize, Page)) return false;
			TMap<FElementTargetKey, int64> DirtyTargets;
			FName CachedDefinitionId = NAME_None;
			FBuildFireShapeRoute CachedRoute;
			bool bHasCachedRoute = false;
			if (Execution) Execution->BeginTargetSnapshotBatch();
			bool bPageSucceeded = true;
			for (const FBuildShapeInstanceSnapshot& Shape : Page.Shapes)
			{
				if (!bHasCachedRoute || CachedDefinitionId != Shape.DefinitionId)
				{
					CachedDefinitionId = Shape.DefinitionId;
					CachedRoute = GetBuildFireShapeRoute(CachedDefinitionId);
					bHasCachedRoute = true;
				}
				if (!CachedRoute.IsRelevant()) continue;
				if (!UpsertBuildingShape(Shape, CachedRoute, Now, false, FTransform::Identity, false))
				{
					bPageSucceeded = false;
					break;
				}
				MarkBuildingTargetDirty(Shape.ShapeRef.Entity, Now, DirtyTargets);
			}
			if (bPageSucceeded) bPageSucceeded = PublishBuildingTargets(DirtyTargets);
			if (Execution) Execution->EndTargetSnapshotBatch();
			if (!bPageSucceeded) return false;
			if (!Page.bHasMore) break;
			Offset = Page.NextOffset;
		}
		for (int32 Offset = 0;;)
		{
			FWorldObjectQuerySnapshotPage Page;
			if (!WorldObjects->CopyQuerySnapshotPage(Offset, PageSize, Page)) return false;
			for (const FWorldObjectShapeInstanceSnapshot& Shape : Page.Shapes)
			{
				if (!UpsertWorldObjectShape(Shape, Now, false, Shape.WorldTransform)) return false;
			}
			if (!Page.bHasMore) break;
			Offset = Page.NextOffset;
		}
		TArray<FCharacterQuerySnapshot> CharacterSnapshots;
		Characters->CopyAllSnapshots(CharacterSnapshots);
		for (const FCharacterQuerySnapshot& Snapshot : CharacterSnapshots)
		{
			if (!UpsertCharacterSnapshot(Snapshot, false, Snapshot.WorldTransform)) return false;
		}
		return true;
	}
