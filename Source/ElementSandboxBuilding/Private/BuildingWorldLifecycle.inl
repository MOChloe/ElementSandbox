// UBuildingWorldSubsystem：依赖装配、注册表/Journal 查询、Processor 与持久化门面。
// 运行时状态由 BuildingWorldRuntime.h 中的组合对象持有。

UBuildingWorldSubsystem::UBuildingWorldSubsystem() = default;

UBuildingWorldSubsystem::~UBuildingWorldSubsystem() = default;

void UBuildingWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldStorageSubsystem>();
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Collection.InitializeDependency<UPresentationWorldSubsystem>();
	check(!Runtime);
	Runtime = MakePimpl<FBuildingWorldRuntime>();
	Runtime->Persistence.WorldStorage = GetWorldRef().GetSubsystem<UWorldStorageSubsystem>();
	if (UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get())
	{
		const TSharedRef<FBuildingWorldStorageAdapter> Adapter = MakeShared<FBuildingWorldStorageAdapter>(*this);
		const bool bRegistered =
			WorldStorage->RegisterDomainAdapter(Adapter) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::Building,
													  *FBuildTransformFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::Building,
													  *FBuildDefinitionFragment::StaticStruct(),
													  EWorldFragmentPersistence::Derived) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::Building,
													  *FBuildWorldIdentityFragment::StaticStruct(),
													  EWorldFragmentPersistence::Persistent) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::Building,
													  *FBuildPartTransformFragment::StaticStruct(),
													  EWorldFragmentPersistence::Derived) &&
			WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::Building,
													  *FBuildRenderCustomDataFragment::StaticStruct(),
													  EWorldFragmentPersistence::Derived) &&
				WorldStorage->RegisterFragmentPersistence(EWorldEntityDomain::Building,
												  *FBuildDamageFragment::StaticStruct(),
												  EWorldFragmentPersistence::RuntimeOnly);
		if (bRegistered)
		{
			Runtime->Persistence.Adapter = Adapter;
		}
		else
		{
			UE_LOG(LogElementSandboxBuilding, Error, TEXT("Building WorldStorage Adapter 或 Fragment 分类注册失败。"));
		}
	}
	CollisionHost = nullptr;
	Runtime->Loop.PostActorTickHandle =
		FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UBuildingWorldSubsystem::HandleWorldPostActorTick);
}

void UBuildingWorldSubsystem::PostInitialize()
{
	Super::PostInitialize();
	CreateCollisionHost();
	RegisterPresentationProjector();
}

void UBuildingWorldSubsystem::Deinitialize()
{
	if (Runtime && Runtime->Loop.PostActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(Runtime->Loop.PostActorTickHandle);
		Runtime->Loop.PostActorTickHandle.Reset();
	}
	ReleasePresentationProjector();
	ReleaseCollisionHost();
	if (Runtime)
	{
		if (UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
			WorldStorage && Runtime->Persistence.Adapter.IsValid())
		{
			WorldStorage->UnregisterDomainAdapter(EWorldEntityDomain::Building, *Runtime->Persistence.Adapter);
		}
		Runtime->Persistence.Adapter.Reset();
		Runtime->Persistence.Extensions.Reset();
	}
	Runtime.Reset();
	Super::Deinitialize();
}

void UBuildingWorldSubsystem::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Runtime && Runtime->Core.SpatialIndex.HasPendingAsyncSnapshotWork())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Snapshot_Scheduler);
		CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, SnapshotScheduler);
		Runtime->Core.SpatialIndex.ProcessAsyncSnapshotWork();
	}
}

bool UBuildingWorldSubsystem::IsTickable() const
{
	return Runtime && Runtime->Core.SpatialIndex.HasPendingAsyncSnapshotWork();
}

TStatId UBuildingWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBuildingWorldSubsystem, STATGROUP_Tickables);
}

FBuildEntityRegistry& UBuildingWorldSubsystem::GetRegistry()
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.Registry;
}

const FBuildEntityRegistry& UBuildingWorldSubsystem::GetRegistry() const
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.Registry;
}

FBuildSpatialIndex& UBuildingWorldSubsystem::GetSpatialIndex()
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.SpatialIndex;
}

const FBuildSpatialIndex& UBuildingWorldSubsystem::GetSpatialIndex() const
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.SpatialIndex;
}

bool UBuildingWorldSubsystem::RegisterDefinition(UBuildingDefinition& Definition)
{
	check(IsInGameThread());
	if (!Runtime || !Definition.HasValidDefinitionId())
	{
		return false;
	}
	if (const TWeakObjectPtr<UBuildingDefinition>* Existing =
			Runtime->Core.DefinitionById.Find(Definition.DefinitionId))
	{
		return Existing->Get() == &Definition;
	}
	if (!Runtime->Core.PlacementGeometry.CacheDefinition(Definition))
	{
		return false;
	}

	Runtime->Core.DefinitionById.Add(Definition.DefinitionId, &Definition);
	return true;
}

UBuildingDefinition* UBuildingWorldSubsystem::FindDefinition(const FName DefinitionId) const
{
	if (!Runtime || DefinitionId.IsNone())
	{
		return nullptr;
	}
	const TWeakObjectPtr<UBuildingDefinition>* Definition = Runtime->Core.DefinitionById.Find(DefinitionId);
	return Definition ? Definition->Get() : nullptr;
}

bool UBuildingWorldSubsystem::RegisterPersistenceExtension(const TSharedRef<IBuildingPersistenceExtension> Extension)
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

bool UBuildingWorldSubsystem::UnregisterPersistenceExtension(const FName SectionId,
															 const IBuildingPersistenceExtension& Extension)
{
	check(IsInGameThread());
	if (!Runtime || SectionId.IsNone())
	{
		return false;
	}
	const TSharedRef<IBuildingPersistenceExtension>* Existing = Runtime->Persistence.Extensions.Find(SectionId);
	if (!Existing || &Existing->Get() != &Extension || Runtime->Persistence.Extensions.Remove(SectionId) != 1)
	{
		return false;
	}
	Runtime->Persistence.ExtensionOrder.RemoveSingle(SectionId);
	return true;
}

bool UBuildingWorldSubsystem::IsEntityAlive(const FBuildEntityHandle Entity) const
{
	return Runtime && Runtime->Core.Registry.IsAlive(Entity);
}

FBuildEntityHandle UBuildingWorldSubsystem::FindEntity(const FWorldEntityId WorldEntityId) const
{
	if (!Runtime || !WorldEntityId.IsSet())
	{
		return {};
	}
	const FBuildEntityHandle* Entity = Runtime->Core.EntityByWorldEntityId.Find(WorldEntityId);
	return Entity && Runtime->Core.Registry.IsAlive(*Entity) ? *Entity : FBuildEntityHandle();
}

FWorldEntityId UBuildingWorldSubsystem::GetWorldEntityId(const FBuildEntityHandle Entity) const
{
	const FBuildWorldIdentityFragment* WorldEntityIdentity =
		Runtime ? Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity) : nullptr;
	return WorldEntityIdentity ? WorldEntityIdentity->WorldEntityId : FWorldEntityId();
}

bool UBuildingWorldSubsystem::CopyQuerySnapshotPage(
	const int32 Offset,
	const int32 MaximumShapes,
	FBuildQuerySnapshotPage& OutPage) const
{
	check(IsInGameThread());
	return Runtime && Runtime->Core.QuerySnapshots.CopyPage(Offset, MaximumShapes, OutPage);
}

bool UBuildingWorldSubsystem::CopyEntityShapeSnapshots(const FBuildEntityHandle Entity,
													   TArray<FBuildShapeInstanceSnapshot>& OutShapes) const
{
	check(IsInGameThread());
	OutShapes.Reset();
	if (!Runtime || !Runtime->Core.Registry.IsAlive(Entity))
		return false;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, OutShapes);
	OutShapes.Sort([](const FBuildShapeInstanceSnapshot& Left, const FBuildShapeInstanceSnapshot& Right)
				   { return BuildShapeRefLess(Left.ShapeRef, Right.ShapeRef); });
	return !OutShapes.IsEmpty();
}

void UBuildingWorldSubsystem::QueryShapeSnapshots(const FBox& Bounds,
												  TArray<FBuildShapeInstanceSnapshot>& OutShapes) const
{
	check(IsInGameThread());
	OutShapes.Reset();
	if (!Runtime || Bounds.IsValid == 0 || Bounds.ContainsNaN())
		return;
	FBuildSpatialQueryScratch Scratch;
	TArray<FBuildEntityHandle> Entities;
	Runtime->Core.SpatialIndex.QueryOverlaps(Bounds, Scratch, Entities);
	TArray<FBuildShapeInstanceSnapshot> EntityShapes;
	for (const FBuildEntityHandle Entity : Entities)
	{
		CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, EntityShapes);
		for (FBuildShapeInstanceSnapshot& Shape : EntityShapes)
		{
			if (Shape.WorldBounds.Intersect(Bounds))
				OutShapes.Add(MoveTemp(Shape));
		}
	}
	OutShapes.Sort([](const FBuildShapeInstanceSnapshot& Left, const FBuildShapeInstanceSnapshot& Right)
				   { return BuildShapeRefLess(Left.ShapeRef, Right.ShapeRef); });
}

FBuildQuerySnapshotBatchCommittedEvent& UBuildingWorldSubsystem::OnQuerySnapshotBatchCommitted()
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.QuerySnapshots.OnBatchCommitted();
}

FBuildDefinitionEntityUpsertedEvent& UBuildingWorldSubsystem::OnDefinitionEntityUpserted(const FName DefinitionId)
{
	check(IsInGameThread());
	check(Runtime && !DefinitionId.IsNone());
	return Runtime->Core.DefinitionEntityUpsertedEvents.FindOrAdd(DefinitionId);
}

FBuildRenderPartShapeAudit UBuildingWorldSubsystem::AuditRenderPartShape(const FBuildEntityHandle Entity,
																		 const int32 MeshPartId) const
{
	check(IsInGameThread());
	FBuildRenderPartShapeAudit Audit;
	Audit.Entity = Entity;
	Audit.MeshPartId = MeshPartId;
	if (!Runtime)
	{
		Audit.Status = EBuildRenderPartShapeAuditStatus::RuntimeUnavailable;
		Audit.FailureReason = TEXT("Building Runtime 不可用。");
		return Audit;
	}
	if (!Runtime->Core.Registry.IsAlive(Entity))
	{
		Audit.Status = EBuildRenderPartShapeAuditStatus::InvalidEntity;
		Audit.FailureReason = TEXT("Entity Handle 已失效或不属于本 Building Registry。");
		return Audit;
	}

	const FBuildDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const FBuildWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	Audit.WorldEntityId = Identity ? Identity->WorldEntityId : FWorldEntityId();
	Audit.DefinitionId = Definition ? Definition->DefinitionId : NAME_None;
	if (!Definition)
	{
		Audit.Status = EBuildRenderPartShapeAuditStatus::MissingDefinition;
		Audit.FailureReason = TEXT("Entity 缺少有效 Building Definition。");
		return Audit;
	}
	if (!Definition->MeshParts.IsValidIndex(MeshPartId))
	{
		Audit.Status = EBuildRenderPartShapeAuditStatus::MissingRenderPart;
		Audit.FailureReason = FString::Printf(TEXT("Definition %s 不存在 MeshPartId=%d。"),
											  *Definition->DefinitionId.ToString(), MeshPartId);
		return Audit;
	}

	const FBuildMeshPartDefinition& MeshPart = Definition->MeshParts[MeshPartId];
	Audit.SurfaceProfileId = MeshPart.SurfaceProfileId;
	if (!MeshPart.Mesh)
	{
		Audit.Status = EBuildRenderPartShapeAuditStatus::MissingRenderMesh;
		Audit.FailureReason = FString::Printf(TEXT("Definition %s 的 MeshPartId=%d 没有 Render Mesh。"),
											  *Definition->DefinitionId.ToString(), MeshPartId);
		return Audit;
	}

	TArray<FBuildShapeInstanceSnapshot> Shapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, MakeArrayView(&MeshPartId, 1), Shapes);
	if (Shapes.Num() != 1)
	{
		Audit.Status = EBuildRenderPartShapeAuditStatus::InvalidShapeGeometry;
		Audit.FailureReason = FString::Printf(TEXT("Definition %s 的 MeshPartId=%d 无法编译为稳定宿主 Shape。"),
											  *Definition->DefinitionId.ToString(), MeshPartId);
		return Audit;
	}

	Audit.ShapeRef = Shapes[0].ShapeRef;
	Audit.Status = EBuildRenderPartShapeAuditStatus::Registered;
	return Audit;
}

FBuildRenderPartCollisionAudit UBuildingWorldSubsystem::AuditRenderPartCollision(const FBuildEntityHandle Entity,
																				 const int32 MeshPartId) const
{
	check(IsInGameThread());
	FBuildRenderPartCollisionAudit Audit;
	Audit.Entity = Entity;
	Audit.MeshPartId = MeshPartId;
	auto AddTerminal = [&Audit](const EBuildCollisionPartAuditStatus Status, FString&& FailureReason)
	{
		FBuildCollisionPartAudit& Part = Audit.CollisionParts.AddDefaulted_GetRef();
		Part.Status = Status;
		Part.FailureReason = MoveTemp(FailureReason);
	};

	if (!Runtime)
	{
		AddTerminal(EBuildCollisionPartAuditStatus::RuntimeUnavailable, TEXT("Building Runtime 不可用。"));
		return Audit;
	}
	if (!Runtime->Core.Registry.IsAlive(Entity))
	{
		AddTerminal(EBuildCollisionPartAuditStatus::InvalidEntity,
					TEXT("Entity Handle 已失效或不属于本 Building Registry。"));
		return Audit;
	}

	const FBuildDefinitionFragment* DefinitionFragment =
		Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const FBuildWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	Audit.WorldEntityId = Identity ? Identity->WorldEntityId : FWorldEntityId();
	Audit.DefinitionId = Definition ? Definition->DefinitionId : NAME_None;
	if (!Definition)
	{
		AddTerminal(EBuildCollisionPartAuditStatus::MissingDefinition, TEXT("Entity 缺少有效 Building Definition。"));
		return Audit;
	}
	if (!Definition->MeshParts.IsValidIndex(MeshPartId))
	{
		AddTerminal(EBuildCollisionPartAuditStatus::MissingRenderPart,
					FString::Printf(TEXT("Definition %s 不存在 MeshPartId=%d。"), *Definition->DefinitionId.ToString(),
									MeshPartId));
		return Audit;
	}
	Audit.SurfaceProfileId = Definition->MeshParts[MeshPartId].SurfaceProfileId;
	if (Definition->CollisionParts.IsEmpty())
	{
		AddTerminal(EBuildCollisionPartAuditStatus::NoCollisionDefinition,
					FString::Printf(TEXT("Definition %s 明确没有 Collision Part；未将 Decorative 内容解释为 Solid。"),
									*Definition->DefinitionId.ToString()));
		return Audit;
	}

	for (int32 CollisionPartId = 0; CollisionPartId < Definition->CollisionParts.Num(); ++CollisionPartId)
	{
		const FBuildCollisionPartDefinition& DefinitionPart = Definition->CollisionParts[CollisionPartId];
		if (DefinitionPart.DrivenMeshPartId != MeshPartId)
		{
			continue;
		}

		FBuildCollisionPartAudit& Part = Audit.CollisionParts.AddDefaulted_GetRef();
		Part.CollisionPartId = CollisionPartId;
		Part.ClusterKey.Mesh = DefinitionPart.CollisionMesh;
		Part.ClusterKey.Mobility = DefinitionPart.Mobility;
		Part.ClusterKey.CollisionProfileName = DefinitionPart.GetEffectiveCollisionProfileName();
		FBuildCollisionPartProjectionState State;
		if (!Runtime->Presentation.CollisionProcessor.GetPartProjectionState(Entity, CollisionPartId, State))
		{
			Part.Status = EBuildCollisionPartAuditStatus::RuntimeUnavailable;
			Part.FailureReason = TEXT("Collision Processor 无法读取 Part 状态。");
			continue;
		}
		Part.bRequired = State.bRequired;
		Part.bRetained = State.bRetained;
		Part.bPendingBudget = State.bPendingPrefetch;
		if (State.ClusterKey.IsSet())
		{
			Part.ClusterKey = State.ClusterKey;
		}
		Part.Instance = State.Instance;
		Part.ProjectionFailure = State.LastFailure;

		const bool bHostFailure = State.LastFailure == EBuildCollisionProjectionFailure::HostRemoveFailed ||
								  State.LastFailure == EBuildCollisionProjectionFailure::HostAddFailed ||
								  State.LastFailure == EBuildCollisionProjectionFailure::HostUpdateFailed;
		if (State.LastFailure != EBuildCollisionProjectionFailure::None)
		{
			Part.Status = bHostFailure ? EBuildCollisionPartAuditStatus::HostApplyFailure
									   : EBuildCollisionPartAuditStatus::ProjectionFailure;
			Part.FailureReason = FString::Printf(TEXT("CollisionPartId=%d 最近一次投影失败，FailureCode=%d。"),
												 CollisionPartId, static_cast<int32>(State.LastFailure));
			continue;
		}
		if (State.Instance.IsSet())
		{
			if (!IsValid(CollisionHost))
			{
				Part.Status = EBuildCollisionPartAuditStatus::HostUnavailable;
				Part.FailureReason = TEXT("Collision Host 不可用。");
			}
			else if (!CollisionHost->IsValidInstance(State.Instance))
			{
				Part.Status = EBuildCollisionPartAuditStatus::InvalidInstance;
				Part.FailureReason = TEXT("Processor 保留了 Instance Handle，但 Host 已无法解析该 Body。");
			}
			else
			{
				Part.Status = State.bRequired ? EBuildCollisionPartAuditStatus::ActiveBody
											  : EBuildCollisionPartAuditStatus::CachedBody;
			}
			continue;
		}
		if (!State.bRequired)
		{
			Part.Status = State.bSelectionPending ? EBuildCollisionPartAuditStatus::AwaitingSelection
												  : EBuildCollisionPartAuditStatus::NotRequired;
			Part.FailureReason = State.bSelectionPending
									 ? TEXT("Collision Source 已变化，尚未完成下一次局部选择。")
									 : TEXT("当前没有 Collision Source 要求该 Part 进入近场工作集。");
		}
		else if (State.bPendingPrefetch)
		{
			Part.Status = EBuildCollisionPartAuditStatus::PendingBudget;
			Part.FailureReason = TEXT("Part 已 Required，正在等待 Prefetch Add 预算。");
		}
		else if (State.bSelectionPending)
		{
			Part.Status = EBuildCollisionPartAuditStatus::AwaitingSelection;
			Part.FailureReason = TEXT("Part 等待局部选择重建。");
		}
		else
		{
			Part.Status = EBuildCollisionPartAuditStatus::AwaitingProjection;
			Part.FailureReason = TEXT("Part 已 Required，但尚未获得 Host Instance。");
		}
	}

	if (Audit.CollisionParts.IsEmpty())
	{
		AddTerminal(EBuildCollisionPartAuditStatus::NoMatchingCollisionPart,
					FString::Printf(TEXT("Definition %s 有 Collision Part，但没有 Part 由 MeshPartId=%d 驱动。"),
									*Definition->DefinitionId.ToString(), MeshPartId));
	}
	return Audit;
}

bool UBuildingWorldSubsystem::EvaluatePlacement(const UBuildingDefinition& Definition,
												const FTransform& CandidateTransform, const FVector& BuilderLocation,
												const double MaximumDistance, FBuildPlacementEvaluation& OutEvaluation,
												const double PenetrationTolerance) const
{
	check(IsInGameThread());
	if (!Runtime
		|| FindDefinition(Definition.DefinitionId) != &Definition)
	{
		OutEvaluation = {};
		OutEvaluation.ResolvedTransform = CandidateTransform;
		OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
		return false;
	}
	return FBuildPlacementEvaluator::Evaluate(
		GetWorldRef(), Definition, CandidateTransform, BuilderLocation, MaximumDistance, PenetrationTolerance,
		Runtime->Core.Registry, Runtime->Core.SpatialIndex, Runtime->Core.PlacementGeometry, CollisionHost, OutEvaluation);
}

bool UBuildingWorldSubsystem::QueryPlacementSurface(
	const FVector& Start,
	const FVector& End,
	const FCollisionQueryParams& WorldQueryParams,
	FBuildPlacementSurfaceHit& OutHit) const
{
	check(IsInGameThread());
	OutHit = {};
	if (!Runtime || Start.ContainsNaN() || End.ContainsNaN()
		|| FVector::DistSquared(Start, End) <= UE_SMALL_NUMBER)
	{
		return false;
	}

	FBuildPlacementSurfaceQuery::QueryBuilding(
		Start,
		End,
		Runtime->Core.Registry,
		Runtime->Core.SpatialIndex,
		Runtime->Core.PlacementGeometry,
		OutHit);

	FCollisionQueryParams QueryParams = WorldQueryParams;
	if (IsValid(CollisionHost))
	{
		QueryParams.AddIgnoredActor(CollisionHost);
	}
	FCollisionObjectQueryParams ObjectQuery;
	ObjectQuery.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectQuery.AddObjectTypesToQuery(ECC_WorldDynamic);
	FHitResult WorldHit;
	if (GetWorldRef().LineTraceSingleByObjectType(
			WorldHit,
			Start,
			End,
			ObjectQuery,
			QueryParams))
	{
		const double Distance = FVector::Distance(Start, WorldHit.ImpactPoint);
		if (Distance < OutHit.Distance)
		{
			OutHit.Location = WorldHit.ImpactPoint;
			OutHit.Normal = WorldHit.ImpactNormal.GetSafeNormal();
			OutHit.Distance = Distance;
			OutHit.bBuildingSurface = false;
		}
	}
	return OutHit.IsValid();
}

FBuildProcessorRegistrationHandle UBuildingWorldSubsystem::RegisterProcessor(TUniquePtr<FBuildProcessor> Processor)
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.ProcessorScheduler.RegisterProcessor(MoveTemp(Processor));
}

bool UBuildingWorldSubsystem::UnregisterProcessor(const FBuildProcessorRegistrationHandle Registration)
{
	check(IsInGameThread());
	check(Runtime);
	return Runtime->Core.ProcessorScheduler.UnregisterProcessor(Registration);
}

bool UBuildingWorldSubsystem::TryGetProcessorStats(const FBuildProcessorRegistrationHandle Registration,
												   FBuildProcessorStats& OutStats) const
{
	check(IsInGameThread());
	return Runtime && Runtime->Core.ProcessorScheduler.TryGetProcessorStats(Registration, OutStats);
}

bool UBuildingWorldSubsystem::CanCommitPersistentStateChange(const FBuildEntityHandle Entity) const
{
	check(IsInGameThread());
	if (!Runtime || GetWorldRef().GetNetMode() == NM_Client || !Runtime->Core.Registry.IsAlive(Entity) ||
		Runtime->Core.QuerySnapshots.IsInTransaction())
	{
		return false;
	}
	const FBuildWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
	const UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	return Identity && WorldStorage && WorldStorage->IsResident(Identity->WorldEntityId);
}

bool UBuildingWorldSubsystem::CommitPersistentStateChange(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!CanCommitPersistentStateChange(Entity))
	{
		return false;
	}
	FScopedBuildQuerySnapshotTransaction JournalTransaction(Runtime->Core.QuerySnapshots);
	if (!JournalTransaction.IsValid())
	{
		return false;
	}
	FBuildWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindMutableFragment<FBuildWorldIdentityFragment>(Entity);
	UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	if (!Identity || !WorldStorage)
	{
		return false;
	}
	const uint32 PreviousStateRevision = Identity->StateRevision;
	const uint32 NewStateRevision = NextBuildRevision(PreviousStateRevision);
	// MarkEntityDirty 会同步广播 Authority Mutation；网络层在该回调中立即捕获
	// Resident Record。Revision 必须先进入宿主，否则 Delta 外层是 NewRevision，
	// Record 仍是 PreviousRevision，客户端会把整批 Live Delta 判为非法。
	Identity->StateRevision = NewStateRevision;
	if (!WorldStorage->MarkEntityDirty(Identity->WorldEntityId, NewStateRevision))
	{
		Identity->StateRevision = PreviousStateRevision;
		return false;
	}
	TArray<FBuildShapeInstanceSnapshot> Shapes;
	CompileBuildEntityShapes(Runtime->Core.Registry, Entity, true, {}, Shapes);
	const bool bPublished =
		PublishBuildShapeTransition(Runtime->Core.QuerySnapshots, Shapes, Shapes, EBuildQuerySnapshotChangeKind::Metadata,
									  EBuildQuerySnapshotChangeKind::ShapeRemove, NewStateRevision,
									  GetBuildHostEffectiveTimeMilliseconds(*Runtime, GetWorldRef()));
	if (!bPublished)
	{
		Identity->StateRevision = PreviousStateRevision;
	}
	return JournalTransaction.Finish(bPublished);
}

bool UBuildingWorldSubsystem::CommitPersistentStateOnlyChange(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!CanCommitPersistentStateChange(Entity))
	{
		return false;
	}
	FBuildWorldIdentityFragment* Identity =
		Runtime->Core.Registry.FindMutableFragment<FBuildWorldIdentityFragment>(Entity);
	UWorldStorageSubsystem* WorldStorage = Runtime->Persistence.WorldStorage.Get();
	if (!Identity || !WorldStorage)
	{
		return false;
	}

	const uint32 PreviousStateRevision = Identity->StateRevision;
	const uint32 NewStateRevision = NextBuildRevision(PreviousStateRevision);
	// Fire 等纯状态结算不改变中性 Shape Metadata。WorldStorage Mutation 监听方会在
	// MarkEntityDirty 内同步捕获 Fragment，因此仍须先发布宿主 Revision。
	Identity->StateRevision = NewStateRevision;
	if (!WorldStorage->MarkEntityDirty(Identity->WorldEntityId, NewStateRevision))
	{
		Identity->StateRevision = PreviousStateRevision;
		return false;
	}
	return true;
}

ABuildCollisionHost* UBuildingWorldSubsystem::GetCollisionHost() const
{
	check(IsInGameThread());
	return CollisionHost;
}
