// UWorldObjectWorldSubsystem：Actor Proxy、Active Array 与 Post-Actor 状态同步。
// 这些状态可丢弃，不承担 WorldStorage 身份或 GameplayDestroy 语义。

void UWorldObjectWorldSubsystem::QueueProxyMotionState(const FWorldEntityId WorldEntityId,
													   const EWorldObjectMotionState NewState)
{
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || !WorldEntityId.IsSet())
	{
		return;
	}
	Runtime->Projection.PendingMotionStates.Add({WorldEntityId, NewState});
}

void UWorldObjectWorldSubsystem::NotifyProxyEndPlay(const FWorldEntityId WorldEntityId,
													UWorldObjectProxyComponent& Proxy,
													const bool bRequestAuthorityDestroy)
{
	if (!Runtime)
	{
		return;
	}
	if (auto* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(Proxy.GetOwner()))
		PhysicsProxy->RetireClientPhysicsProjection();
	UnbindProxy(Proxy);
	if (const auto* Pending = Runtime->Projection.PendingProxies.Find(WorldEntityId);
		Pending && Pending->Get() == &Proxy)
		Runtime->Projection.PendingProxies.Remove(WorldEntityId);
	if (bRequestAuthorityDestroy && GetWorldRef().GetNetMode() != NM_Client && WorldEntityId.IsSet())
	{
		Runtime->Projection.PendingAuthorityDestroys.AddUnique(WorldEntityId);
	}
}

void UWorldObjectWorldSubsystem::RegisterProxy(UWorldObjectProxyComponent& Proxy)
{
	if (!Runtime || !Proxy.GetWorldEntityId().IsSet())
	{
		return;
	}
	const FWorldObjectEntityHandle Entity = FindEntity(Proxy.GetWorldEntityId());
	if (Entity.IsSet())
	{
		BindProxyToEntity(Entity, Proxy);
	}
	else
	{
		if (const auto* Pending = Runtime->Projection.PendingProxies.Find(Proxy.GetWorldEntityId()))
		{
			const auto* OldActor = Pending->IsValid() ? Cast<AWorldObjectPhysicsProxyActor>(Pending->Get()->GetOwner()) : nullptr;
			const auto* NewActor = Cast<AWorldObjectPhysicsProxyActor>(Proxy.GetOwner());
			if (OldActor && NewActor && OldActor != NewActor
				&& OldActor->GetActivationRevision() >= NewActor->GetActivationRevision()) return;
		}
		Runtime->Projection.PendingProxies.Add(Proxy.GetWorldEntityId(), &Proxy);
	}
}

bool UWorldObjectWorldSubsystem::BindProxyToEntity(const FWorldObjectEntityHandle Entity,
												   UWorldObjectProxyComponent& Proxy)
{
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity))
	{
		return false;
	}
	Runtime->EnsureSlotCapacity(Entity.GetSlot());
	UWorldObjectProxyComponent* Existing = Runtime->Projection.ProxyBySlot[Entity.GetSlot()].Get();
	if (IsValid(Existing) && Existing != &Proxy)
	{
		auto* OldActor = Cast<AWorldObjectPhysicsProxyActor>(Existing->GetOwner());
		auto* NewActor = Cast<AWorldObjectPhysicsProxyActor>(Proxy.GetOwner());
		if (GetWorldRef().GetNetMode() != NM_Client || !OldActor || !NewActor
			|| NewActor->GetActivationRevision() <= OldActor->GetActivationRevision()) return false;
		// 不同 Actor Channel 可以交错；按同一 WorldEntity 的生命周期 Revision 接管。
		OldActor->RetireClientPhysicsProjection();
		UnbindProxy(*Existing);
		Existing = nullptr;
	}
	if (!IsValid(Existing))
	{
		++Runtime->Projection.BoundProxyCount;
	}
	Runtime->Projection.ProxyBySlot[Entity.GetSlot()] = &Proxy;
	Proxy.SetLocalEntity(Entity);
	if (auto* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(Proxy.GetOwner()))
		PhysicsProxy->RefreshClientPhysicsProjection();
	const FWorldObjectMotionFragment* Motion = Runtime->Core.Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
	if (Motion && IsActorActiveState(Motion->State))
	{
		AddActorActive(Entity);
	}
	return true;
}

void UWorldObjectWorldSubsystem::UnbindProxy(UWorldObjectProxyComponent& Proxy)
{
	if (!Runtime)
	{
		return;
	}
	const FWorldObjectEntityHandle Entity = Proxy.GetLocalEntity();
	if (Runtime->Core.Registry.IsAlive(Entity)
		&& Runtime->Projection.ProxyBySlot.IsValidIndex(Entity.GetSlot())
		&& Runtime->Projection.ProxyBySlot[Entity.GetSlot()].Get() == &Proxy)
	{
		RemoveActorActive(Entity);
		Runtime->Projection.ProxyBySlot[Entity.GetSlot()] = nullptr;
		Runtime->Projection.BoundProxyCount = FMath::Max(0, Runtime->Projection.BoundProxyCount - 1);
	}
	Proxy.SetLocalEntity({});
	// Residency 卸载只暂停投影；同一个复制 Actor 仍可能在新 Chunk Record 到达后重新绑定。
	if (auto* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(Proxy.GetOwner()))
		PhysicsProxy->RefreshClientPhysicsProjection();
	if (GetWorldRef().GetNetMode() == NM_Client)
	{
		if (auto* Collision = GetWorldRef().GetSubsystem<UWorldObjectCollisionWorldSubsystem>())
			Collision->NotifyPhysicsProjectionChanged(Entity);
	}
}

void UWorldObjectWorldSubsystem::AddActorActive(const FWorldObjectEntityHandle Entity)
{
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity) || !IsValid(GetProxy(Entity)))
	{
		return;
	}
	Runtime->EnsureSlotCapacity(Entity.GetSlot());
	if (Runtime->Projection.ActorActiveIndexBySlot[Entity.GetSlot()] != INDEX_NONE)
	{
		return;
	}
	Runtime->Projection.ActorActiveIndexBySlot[Entity.GetSlot()] = Runtime->Projection.ActorActiveEntities.Add(Entity);
}

void UWorldObjectWorldSubsystem::RemoveActorActive(const FWorldObjectEntityHandle Entity)
{
	if (!Runtime || !Runtime->Projection.ActorActiveIndexBySlot.IsValidIndex(Entity.GetSlot()))
	{
		return;
	}
	const int32 ActiveIndex = Runtime->Projection.ActorActiveIndexBySlot[Entity.GetSlot()];
	if (!Runtime->Projection.ActorActiveEntities.IsValidIndex(ActiveIndex) ||
		Runtime->Projection.ActorActiveEntities[ActiveIndex] != Entity)
	{
		Runtime->Projection.ActorActiveIndexBySlot[Entity.GetSlot()] = INDEX_NONE;
		return;
	}
	const int32 LastIndex = Runtime->Projection.ActorActiveEntities.Num() - 1;
	if (ActiveIndex != LastIndex)
	{
		Runtime->Projection.ActorActiveEntities[ActiveIndex] = Runtime->Projection.ActorActiveEntities[LastIndex];
		Runtime->Projection.ActorActiveIndexBySlot[Runtime->Projection.ActorActiveEntities[ActiveIndex].GetSlot()] =
			ActiveIndex;
	}
	Runtime->Projection.ActorActiveEntities.RemoveAt(LastIndex, EAllowShrinking::No);
	Runtime->Projection.ActorActiveIndexBySlot[Entity.GetSlot()] = INDEX_NONE;
}

void UWorldObjectWorldSubsystem::HandleWorldPostActorTick(UWorld* World, const ELevelTick TickType,
														  const float DeltaSeconds)
{
	if (!Runtime || World != GetWorld())
	{
		return;
	}
	Runtime->Projection.CurrentPostActorDeltaSeconds = DeltaSeconds;
	EnsurePostActorStateCurrent();
}

void UWorldObjectWorldSubsystem::EnsurePostActorStateCurrent()
{
	check(IsInGameThread());
	if (!Runtime)
	{
		return;
	}

	if (Runtime->Projection.LastPostActorSyncFrame != GFrameCounter)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_PostActorTick);
		CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, PostActorTick);
		ProcessPendingProxyEvents();
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_ActiveSync);
			CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, ActiveSync);
			SyncActiveTransforms();
		}
		if (Runtime->Core.SpatialIndex.IsStaticDirty())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_StaticBuild);
			CSV_SCOPED_TIMING_STAT(ElementSandboxWorldObjects, StaticBuild);
			Runtime->Core.SpatialIndex.RebuildStaticIfDirty();
		}
		Runtime->Projection.LastPostActorSyncFrame = GFrameCounter;
	}
}

void UWorldObjectWorldSubsystem::ProcessPendingProxyEvents()
{
	TArray<FWorldEntityId> PendingDestroys = MoveTemp(Runtime->Projection.PendingAuthorityDestroys);
	Runtime->Projection.PendingAuthorityDestroys.Reset();
	for (const FWorldEntityId WorldEntityId : PendingDestroys)
	{
		const FWorldObjectEntityHandle Entity = FindEntity(WorldEntityId);
		if (Entity.IsSet())
		{
			DestroyEntityInternal(Entity, ERemovalSemantic::GameplayDestroy, false, true);
		}
	}

	TArray<FWorldObjectWorldRuntime::FPendingMotionState> PendingStates =
		MoveTemp(Runtime->Projection.PendingMotionStates);
	Runtime->Projection.PendingMotionStates.Reset();
	for (const FWorldObjectWorldRuntime::FPendingMotionState& Pending : PendingStates)
	{
		const FWorldObjectEntityHandle Entity = FindEntity(Pending.WorldEntityId);
		if (Entity.IsSet())
		{
			SetMotionStateInternal(Entity, Pending.State, true);
		}
	}
}

void UWorldObjectWorldSubsystem::SyncActiveTransforms()
{
	Runtime->Projection.LastSampledActiveCount = 0;
	Runtime->Projection.LastChangedTransformCount = 0;
	TArray<FWorldObjectEntityHandle, TInlineAllocator<16>> SleepingPhysicsEntities;
	for (const FWorldObjectEntityHandle Entity : Runtime->Projection.ActorActiveEntities)
	{
		UWorldObjectProxyComponent* Proxy = GetProxy(Entity);
		AActor* Actor = Proxy ? Proxy->GetOwner() : nullptr;
		const FWorldObjectTransformFragment* Transform =
			Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
		const FWorldObjectMotionFragment* Motion =
			Runtime->Core.Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
		if (!IsValid(Actor) || !Transform || !Motion)
		{
			continue;
		}
		++Runtime->Projection.LastSampledActiveCount;
		const AWorldObjectPhysicsProxyActor* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(Actor);
		if (PhysicsProxy && GetWorldRef().GetNetMode() == NM_Client
			&& !PhysicsProxy->HasClientPhysicsProjection()) continue;
		const FTransform ActorTransform =
			PhysicsProxy ? PhysicsProxy->GetWorldObjectTransform() : Actor->GetActorTransform();
		if (!Transform->WorldTransform.Equals(ActorTransform, 0.01) &&
			CommitTransformInternal(Entity, ActorTransform, false))
		{
			++Runtime->Projection.LastChangedTransformCount;
		}
		UPrimitiveComponent* PhysicsPrimitive = Proxy->GetPhysicsPrimitive();
		if (GetWorldRef().GetNetMode() != NM_Client && Motion->State == EWorldObjectMotionState::Physics && IsValid(PhysicsPrimitive) &&
			PhysicsPrimitive->IsSimulatingPhysics() && !PhysicsPrimitive->IsAnyRigidBodyAwake())
		{
			SleepingPhysicsEntities.Add(Entity);
		}
	}
	// Chaos Wake/Sleep delegate 是主路径；这里仅在活动 Actor 集合内核对真实睡眠状态，
	// 覆盖测试 World 或平台后端漏投递通知的情况。必须在 Active Array 遍历后提交。
	for (const FWorldObjectEntityHandle Entity : SleepingPhysicsEntities)
	{
		SetMotionStateInternal(Entity, EWorldObjectMotionState::Dormant, true);
	}
}

bool UWorldObjectWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
