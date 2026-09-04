// UBuildingWorldSubsystem：Collision/Render 投影、Host 生命周期与 Post-Actor flush。
// 本组只消费 ECS 真值并维护可丢弃表现，不拥有 Gameplay 状态。

FBuildCollisionSourceHandle UBuildingWorldSubsystem::RegisterCollisionSource(const FBuildCollisionSource& SourceData)
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Presentation.CollisionProcessor.RegisterSource(SourceData);
}

bool UBuildingWorldSubsystem::UpdateCollisionSource(const FBuildCollisionSourceHandle Source,
													const FBuildCollisionSource& SourceData)
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Presentation.CollisionProcessor.UpdateSource(Source, SourceData);
}

bool UBuildingWorldSubsystem::UnregisterCollisionSource(const FBuildCollisionSourceHandle Source)
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Presentation.CollisionProcessor.UnregisterSource(Source);
}

int32 UBuildingWorldSubsystem::GetActiveCollisionBodyCount() const
{
	check(IsInGameThread());
	return IsValid(CollisionHost) ? CollisionHost->GetInstanceCount() : 0;
}

int32 UBuildingWorldSubsystem::GetCollisionSourceCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetSourceCount() : 0;
}

int32 UBuildingWorldSubsystem::GetActiveCollisionEntityCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetProjectedEntityCount() : 0;
}

int32 UBuildingWorldSubsystem::GetRequiredCollisionPartCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetRequiredPartCount() : 0;
}

int32 UBuildingWorldSubsystem::GetCachedOnlyCollisionPartCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetCachedOnlyPartCount() : 0;
}

int32 UBuildingWorldSubsystem::GetPendingCollisionPrefetchAddCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetPendingPrefetchAddCount() : 0;
}

int32 UBuildingWorldSubsystem::GetCollisionEvictionCandidateCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetEvictionCandidateCount() : 0;
}

int32 UBuildingWorldSubsystem::GetLastQueriedCollisionEntityCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetLastQueriedEntityCount() : 0;
}

int32 UBuildingWorldSubsystem::GetLastInspectedCollisionPartCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetLastInspectedPartCount() : 0;
}

int32 UBuildingWorldSubsystem::GetLastChangedCollisionPartCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetLastChangedPartCount() : 0;
}

FBuildCollisionActivationConfig UBuildingWorldSubsystem::GetCollisionActivationConfig() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.CollisionProcessor.GetActivationConfig() : FBuildCollisionActivationConfig();
}

bool UBuildingWorldSubsystem::HasPendingCollisionWork() const
{
	check(IsInGameThread());
	return Runtime && Runtime->Presentation.CollisionProcessor.HasPendingWork();
}

bool UBuildingWorldSubsystem::TryGetPartCollisionInstance(const FBuildEntityHandle Entity, const int32 CollisionPartId,
														  FBuildCollisionInstanceHandle& OutInstance) const
{
	check(IsInGameThread());
	return Runtime &&
		   Runtime->Presentation.CollisionProcessor.TryGetInstanceHandle(Entity, CollisionPartId, OutInstance);
}

bool UBuildingWorldSubsystem::SetPresentationMotionActive(const FBuildEntityHandle Entity, const bool bActive)
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
	if (!Runtime->Presentation.RenderProcessor.SetPresentationMotionActive(Entity, bActive) ||
		!Runtime->Presentation.RenderDirtySet.MarkAllPartsDirty(Entity) || !ResolveRenderChanges())
	{
		return false;
	}
	UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	return Presentation && Runtime->Presentation.Layer.IsSet() &&
		   Runtime->Presentation.RenderProcessor.Project(Runtime->Core.Registry, Runtime->Presentation.LastViews,
														 *Presentation, Runtime->Presentation.Layer);
}

bool UBuildingWorldSubsystem::IsPresentationMotionActive(const FBuildEntityHandle Entity) const
{
	check(IsInGameThread());
	return Runtime && !GetWorldRef().IsNetMode(NM_DedicatedServer) && Runtime->Core.Registry.IsAlive(Entity) &&
		   Runtime->Presentation.RenderProcessor.IsPresentationMotionActive(Entity);
}

bool UBuildingWorldSubsystem::TryGetPartRenderStorageClass(const FBuildEntityHandle Entity, const int32 PartId,
														   EBuildRenderStorageClass& OutStorageClass) const
{
	check(IsInGameThread());
	return Runtime && Runtime->Presentation.RenderProcessor.TryGetPartStorageClass(Entity, PartId, OutStorageClass);
}

SIZE_T UBuildingWorldSubsystem::GetEstimatedBuildingCPUAllocatedSize() const
{
	check(IsInGameThread());
	if (!Runtime)
	{
		return 0;
	}

	return Runtime->Core.Registry.GetEstimatedAllocatedSize() + Runtime->Core.SpatialIndex.GetEstimatedAllocatedSize() +
		   Runtime->Core.QuerySnapshots.GetAllocatedSize() +
		   Runtime->Presentation.CollisionProcessor.GetEstimatedAllocatedSize() +
			   Runtime->Presentation.RenderDirtySet.GetEstimatedAllocatedSize() +
			   Runtime->Presentation.RenderProcessor.GetEstimatedAllocatedSize() +
				   Runtime->Core.ProcessorScheduler.GetEstimatedAllocatedSize() +
		   Runtime->Core.EntityByWorldEntityId.GetAllocatedSize() + Runtime->Core.DefinitionById.GetAllocatedSize() +
		   Runtime->Persistence.Extensions.GetAllocatedSize() +
		   (IsValid(CollisionHost) ? CollisionHost->GetEstimatedCPUAllocatedSize() : 0);
}

int32 UBuildingWorldSubsystem::GetRenderedBuildingCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.RenderProcessor.GetRenderedEntityCount() : 0;
}

int32 UBuildingWorldSubsystem::GetRenderedInstanceCount() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.RenderProcessor.GetSelectionStats().ResidentMeshPartCount : 0;
}

int32 UBuildingWorldSubsystem::GetRenderClusterCount() const
{
	check(IsInGameThread());
	const UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	return Presentation ? Presentation->GetMeshPoolStats().ClusterCount : 0;
}

uint64 UBuildingWorldSubsystem::GetHierarchicalTreeBuildRequestCount() const
{
	check(IsInGameThread());
	const UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	return Presentation ? Presentation->GetMeshPoolStats().HierarchicalTreeBuildRequests : 0;
}

FBuildPresentationSelectionStats UBuildingWorldSubsystem::GetPresentationSelectionStats() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.RenderProcessor.GetSelectionStats() : FBuildPresentationSelectionStats();
}

double UBuildingWorldSubsystem::GetStaticRenderCellSize() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.RenderProcessor.GetStaticRenderCellSize() : 0.0;
}

double UBuildingWorldSubsystem::GetLastRenderFlushMilliseconds() const
{
	check(IsInGameThread());
	return Runtime ? Runtime->Presentation.LastRenderFlushMilliseconds : 0.0;
}

bool UBuildingWorldSubsystem::FlushCollisionChanges() { return FlushCollisionChanges(GetWorldRef().GetTimeSeconds()); }

bool UBuildingWorldSubsystem::FlushCollisionChanges(const double CurrentTimeSeconds)
{
	check(IsInGameThread());
	check(Runtime);
	if (!Runtime->Presentation.CollisionProcessor.HasPendingWork())
	{
		return true;
	}

	return IsValid(CollisionHost) &&
		   Runtime->Presentation.CollisionProcessor.Execute(Runtime->Core.Registry, Runtime->Core.SpatialIndex,
															*CollisionHost, CurrentTimeSeconds);
}

bool UBuildingWorldSubsystem::FlushRenderChanges()
{
	check(IsInGameThread());
	check(Runtime);
	const double FlushStartSeconds = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		if (Runtime)
		{
			Runtime->Presentation.LastRenderFlushMilliseconds = (FPlatformTime::Seconds() - FlushStartSeconds) * 1000.0;
		}
	};
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Render_Flush);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, RenderFlush);
	if (GetWorldRef().IsNetMode(NM_DedicatedServer))
	{
		Runtime->Presentation.RenderDirtySet.Clear();
		Runtime->Presentation.RenderCustomDataDirtyEntities.Reset();
		Runtime->Presentation.RenderCustomDataDirtySet.Reset();
		return true;
	}

	UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	if (!Presentation || !Runtime->Presentation.Layer.IsSet() || !Runtime->Presentation.Projector.IsSet())
	{
		return false;
	}
	Runtime->Presentation.bLastProjectionSucceeded = true;
	Runtime->Presentation.RenderProcessor.RequestSynchronousLocalSelectionForNextProjection();
	return Presentation->RunCycleNow() && Runtime->Presentation.bLastProjectionSucceeded;
}

bool UBuildingWorldSubsystem::IsEntityPresentationResident(const FBuildEntityHandle Entity) const
{
	check(IsInGameThread());
	return Runtime && Entity.IsSet()
		&& (Runtime->Presentation.RenderProcessor.GetRenderedPartCount(Entity) > 0
			|| Runtime->Presentation.RenderProcessor.HasRetiringInstances(Entity));
}

bool UBuildingWorldSubsystem::RequestPresentationProjection()
{
	check(IsInGameThread());
	if (GetWorldRef().IsNetMode(NM_DedicatedServer))
	{
		return true;
	}
	UPresentationWorldSubsystem* Presentation =
		GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	return Presentation && Presentation->RequestProjectionCycle();
}

bool UBuildingWorldSubsystem::ResolveRenderChanges()
{
	check(IsInGameThread());
	check(Runtime);
	if (GetWorldRef().IsNetMode(NM_DedicatedServer))
	{
		Runtime->Presentation.RenderDirtySet.Clear();
		Runtime->Presentation.RenderCustomDataDirtyEntities.Reset();
		Runtime->Presentation.RenderCustomDataDirtySet.Reset();
		return true;
	}
	UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	if (!Presentation || !Runtime->Presentation.Layer.IsSet())
	{
		return false;
	}
	if (!Runtime->Presentation.RenderDirtySet.IsEmpty() &&
		!Runtime->Presentation.RenderProcessor.Execute(Runtime->Core.Registry, Runtime->Presentation.RenderDirtySet,
													   *Presentation, Runtime->Presentation.Layer))
	{
		return false;
	}
	if (!Runtime->Presentation.RenderCustomDataDirtyEntities.IsEmpty() &&
		!Runtime->Presentation.RenderProcessor.ApplyCustomDataChanges(
			Runtime->Core.Registry, *Presentation, Runtime->Presentation.RenderCustomDataDirtyEntities))
	{
		Runtime->Presentation.RenderDirtySet.RequestClearAll();
		TArray<FBuildEntityHandle> RenderEntities;
		Runtime->Core.Registry.GetEntitiesWithFragment<FBuildDefinitionFragment>(RenderEntities);
		for (const FBuildEntityHandle RenderEntity : RenderEntities)
		{
			Runtime->Presentation.RenderDirtySet.MarkRebuild(RenderEntity);
		}
		RequestPresentationProjection();
		return false;
	}
	Runtime->Presentation.RenderCustomDataDirtyEntities.Reset();
	Runtime->Presentation.RenderCustomDataDirtySet.Reset();
	return true;
}

void UBuildingWorldSubsystem::HandlePresentationProjection(const FPresentationViewSnapshot& Views)
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().IsNetMode(NM_DedicatedServer))
	{
		return;
	}
	Runtime->Presentation.LastViews = Views;
	UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>();
	const double ResolveStartSeconds = FPlatformTime::Seconds();
	const bool bResolved = Presentation && ResolveRenderChanges();
	CSV_CUSTOM_STAT(ElementSandboxBuilding, PresentationResolveMilliseconds,
		(FPlatformTime::Seconds() - ResolveStartSeconds) * 1000.0, ECsvCustomStatOp::Set);
	const bool bSucceeded = bResolved &&
								Runtime->Presentation.RenderProcessor.Project(Runtime->Core.Registry, Views, *Presentation,
																  Runtime->Presentation.Layer);
	Runtime->Presentation.bLastProjectionSucceeded &= bSucceeded;
	if (bSucceeded && Runtime->Presentation.RenderProcessor.HasPendingProjectionWork())
	{
		// Local/Far 选择与大批结果发布都可能跨多个客户端投影周期。观察源静止时不会
		// 再产生 Source Dirty，因此领域 Projector 必须显式续约，直到自己的工作队列排空。
		Presentation->RequestProjectionCycle();
	}
}

bool UBuildingWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UBuildingWorldSubsystem::RegisterPresentationProjector()
{
	check(IsInGameThread());
	check(Runtime);
	UWorld& World = GetWorldRef();
	if (World.IsNetMode(NM_DedicatedServer))
	{
		return true;
	}

	if (Runtime->Presentation.Layer.IsSet() && Runtime->Presentation.Projector.IsSet())
	{
		return true;
	}
	UPresentationWorldSubsystem* Presentation = World.GetSubsystem<UPresentationWorldSubsystem>();
	if (!Presentation)
	{
		UE_LOG(LogElementSandboxBuilding, Error,
			   TEXT("Building World Subsystem could not resolve Presentation in World %s."), *World.GetName());
		return false;
	}
	Runtime->Presentation.Layer = Presentation->RegisterMeshLayer(TEXT("Building"));
	if (!Runtime->Presentation.Layer.IsSet())
	{
		return false;
	}
	FPresentationProjectorDelegate Delegate;
	Delegate.BindUObject(this, &UBuildingWorldSubsystem::HandlePresentationProjection);
	Runtime->Presentation.Projector = Presentation->RegisterProjector(TEXT("Building"), MoveTemp(Delegate));
	if (!Runtime->Presentation.Projector.IsSet())
	{
		Presentation->UnregisterMeshLayer(Runtime->Presentation.Layer);
		Runtime->Presentation.Layer = {};
		return false;
	}
	Runtime->Presentation.MeshPoolInstanceRetiredHandle =
		Presentation->OnInstanceRetired().AddUObject(
			this, &UBuildingWorldSubsystem::HandleMeshPoolInstanceRetired);
	return true;
}

void UBuildingWorldSubsystem::HandleMeshPoolInstanceRetired(
	const FMeshPoolInstanceHandle Instance)
{
	check(IsInGameThread());
	if (Runtime)
	{
		Runtime->Presentation.RenderProcessor.NotifyInstanceRetired(Instance);
	}
}

bool UBuildingWorldSubsystem::CreateCollisionHost()
{
	check(IsInGameThread());
	check(Runtime);
	if (IsValid(CollisionHost))
	{
		return true;
	}

	UWorld& World = GetWorldRef();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CollisionHost = World.SpawnActor<ABuildCollisionHost>(SpawnParameters);
	if (!CollisionHost)
	{
		UE_LOG(LogElementSandboxBuilding, Error,
			   TEXT("Building World Subsystem failed to create its Collision Host in World %s."), *World.GetName());
		return false;
	}
	return true;
}

void UBuildingWorldSubsystem::ReleaseCollisionHost()
{
	check(IsInGameThread());
	if (!IsValid(CollisionHost))
	{
		CollisionHost = nullptr;
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !World->IsBeingCleanedUp())
	{
		CollisionHost->Destroy();
	}
	CollisionHost = nullptr;
}

void UBuildingWorldSubsystem::ReleasePresentationProjector()
{
	check(IsInGameThread());
	if (!Runtime)
	{
		return;
	}
	if (UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>())
	{
		if (Runtime->Presentation.Projector.IsSet())
		{
			Presentation->UnregisterProjector(Runtime->Presentation.Projector);
		}
		if (Runtime->Presentation.Layer.IsSet())
		{
			Presentation->UnregisterMeshLayer(Runtime->Presentation.Layer);
		}
		if (Runtime->Presentation.MeshPoolInstanceRetiredHandle.IsValid())
		{
			Presentation->OnInstanceRetired().Remove(
				Runtime->Presentation.MeshPoolInstanceRetiredHandle);
		}
	}
	Runtime->Presentation.Projector = {};
	Runtime->Presentation.Layer = {};
	Runtime->Presentation.MeshPoolInstanceRetiredHandle.Reset();
}

void UBuildingWorldSubsystem::HandleWorldPostActorTick(UWorld* World, const ELevelTick TickType,
													   const float DeltaSeconds)
{
	if (!Runtime || World != GetWorld())
	{
		return;
	}

	const bool bAuthority = World->GetNetMode() != NM_Client;
	bool bRunAuthorityStep = false;
	float AuthorityDeltaSeconds = 0.0f;
	if (bAuthority && TickType != LEVELTICK_TimeOnly)
	{
		Runtime->Loop.AuthorityTickAccumulator += FMath::Max(0.0f, DeltaSeconds);
		if (Runtime->Loop.AuthorityTickAccumulator >= UWorldStorageSubsystem::AuthorityTickIntervalSeconds)
		{
			AuthorityDeltaSeconds = static_cast<float>(Runtime->Loop.AuthorityTickAccumulator);
			Runtime->Loop.AuthorityTickAccumulator = FMath::Fmod(Runtime->Loop.AuthorityTickAccumulator,
																 UWorldStorageSubsystem::AuthorityTickIntervalSeconds);
			bRunAuthorityStep = true;
		}
	}

	if (bRunAuthorityStep && Runtime->Core.ProcessorScheduler.HasReadyProcessors())
	{
		const UWorldStorageSubsystem* Storage = Runtime->Persistence.WorldStorage.Get();
		const double WorldTimeSeconds =
			Storage ? static_cast<double>(Storage->GetWorldSimulationTimeMilliseconds()) / 1000.0
					: World->GetTimeSeconds();
		Runtime->Core.ProcessorScheduler.ExecuteReady(*this, WorldTimeSeconds, AuthorityDeltaSeconds);
	}

	if (Runtime->Presentation.CollisionProcessor.HasPendingWork())
	{
		if (FlushCollisionChanges())
		{
			Runtime->Presentation.bCollisionFlushFailureLogged = false;
		}
		else if (!Runtime->Presentation.bCollisionFlushFailureLogged)
		{
			UE_LOG(LogElementSandboxBuilding, Error,
				   TEXT("Building World Subsystem could not flush its collision change batch in World %s."),
				   *World->GetName());
			Runtime->Presentation.bCollisionFlushFailureLogged = true;
		}
	}
}
