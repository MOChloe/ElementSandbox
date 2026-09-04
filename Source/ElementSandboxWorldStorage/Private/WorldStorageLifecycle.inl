// UWorldStorageSubsystem：Archive/Adapter 装配、Resident Directory 与 Entity COW 变更。
// 稳定身份、GameplayDestroy 和位置迁移都在 GameThread 原子更新。

namespace
{
	bool HasCommittedDirtyEntities(const FWorldStorageRuntime& Runtime)
	{
		for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : Runtime.Entities.DirtyEntities)
		{
			if (Pair.Value.DelayedMutationBatchId == 0)
			{
				return true;
			}
		}
		return false;
	}

	void TagDirtyWithActiveBatch(
		FWorldStorageRuntime& Runtime,
		const FWorldEntityId EntityId,
		FDirtyEntityState& Dirty)
	{
		const uint64 BatchId = Runtime.Entities.ActiveDelayedMutationBatchId;
		if (BatchId == 0)
		{
			return;
		}
		FDelayedMutationBatchState* Batch = Runtime.Entities.DelayedMutationBatches.Find(BatchId);
		check(Batch && (Dirty.DelayedMutationBatchId == 0 || Dirty.DelayedMutationBatchId == BatchId));
		Dirty.DelayedMutationBatchId = BatchId;
		Batch->EntityIds.Add(EntityId);
	}
}

UWorldStorageSubsystem::UWorldStorageSubsystem() = default;
UWorldStorageSubsystem::~UWorldStorageSubsystem() = default;

void UWorldStorageSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	check(!Runtime);
	Runtime = MakePimpl<FWorldStorageRuntime>();
	const bool bStandaloneExternalServerClient =
		GetWorld() && GetWorld()->GetNetMode() == NM_Standalone &&
		FWorldStorageProcessRole::ShouldUseExternalLocalServerForCurrentWorld(GetWorld());
	Runtime->Core.bAuthority = GetWorld() && GetWorld()->GetNetMode() != NM_Client && !bStandaloneExternalServerClient;
	if (Runtime->Core.bAuthority)
	{
		Runtime->Core.Archive = MakeShared<FWorldStorageArchive, ESPMode::ThreadSafe>();
		FWorldStorageOpenOptions Options;
		Options.WritableRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldSaves/DefaultWorld"));
		Options.SeedRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldSeeds/MillionSettlement"));
		const bool bExplicitSaveRoot = FParse::Value(FCommandLine::Get(), TEXT("WorldSaveRoot="), Options.WritableRoot);
		const bool bExplicitSeedRoot = FParse::Value(FCommandLine::Get(), TEXT("WorldSeedRoot="), Options.SeedRoot);
		if (GIsAutomationTesting && !bExplicitSaveRoot && !bExplicitSeedRoot)
		{
			Runtime->Core.EphemeralAutomationArchiveRoot = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("WorldStorageAutomation"),
								FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			Options.WritableRoot = FPaths::Combine(Runtime->Core.EphemeralAutomationArchiveRoot, TEXT("Writable"));
			Options.SeedRoot = FPaths::Combine(Runtime->Core.EphemeralAutomationArchiveRoot, TEXT("NoSeed"));
		}
		Options.WritableRoot = FPaths::ConvertRelativePathToFull(Options.WritableRoot);
		Options.SeedRoot = FPaths::ConvertRelativePathToFull(Options.SeedRoot);
		FString Error;
		const double OpenStartSeconds = FPlatformTime::Seconds();
		if (!Runtime->Core.Archive->Open(Options, Error))
		{
			UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("WorldStorage 打开失败：%s"), *Error);
			Runtime->Core.Archive.Reset();
			return;
		}
		Runtime->Core.ManifestInfo = Runtime->Core.Archive->GetManifestInfo();
		const FWorldStorageArchiveStats ArchiveStats = Runtime->Core.Archive->GetArchiveStats();
		UE_LOG(LogElementSandboxWorldStorage, Display,
			   TEXT("存档清单就绪：%.2f ms；%d Packs；%d 个非空 Chunk；未加载 Entity Blob。"),
			   (FPlatformTime::Seconds() - OpenStartSeconds) * 1000.0, ArchiveStats.PackCount,
			   ArchiveStats.OccupiedChunkCount);
		const FTimespan OfflineElapsed = FDateTime::UtcNow() - Runtime->Core.ManifestInfo.LastCheckpointUtc;
		if (OfflineElapsed.GetTotalMilliseconds() > 0.0)
		{
			Runtime->Core.ManifestInfo.WorldSimulationTimeMilliseconds +=
				FMath::FloorToInt64(OfflineElapsed.GetTotalMilliseconds());
		}
	}
}

void UWorldStorageSubsystem::Deinitialize()
{
	FString EphemeralAutomationArchiveRoot;
	if (Runtime)
	{
		if (Runtime->Checkpoint.Future.IsValid())
		{
			Runtime->Checkpoint.Future.Wait();
			DrainCheckpointCompletion();
		}
				if (Runtime->Core.bAuthority && HasCommittedDirtyEntities(*Runtime))
		{
			StartCheckpoint(true);
			DrainCheckpointCompletion();
		}
		Runtime->Core.AsyncState->bAcceptResults.Store(false);
		AuthorityMutationEvent.Clear();
		AuthorityStepEvent.Clear();
		PrepareRuntimeEvictBatchEvent.Clear();
		EphemeralAutomationArchiveRoot = Runtime->Core.EphemeralAutomationArchiveRoot;
		Runtime.Reset();
	}
	if (!EphemeralAutomationArchiveRoot.IsEmpty())
	{
		const FString IntermediateRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir());
		const FString ExpectedPrefix = IntermediateRoot.EndsWith(TEXT("/")) || IntermediateRoot.EndsWith(TEXT("\\"))
										   ? IntermediateRoot
										   : IntermediateRoot + TEXT("/");
		if (EphemeralAutomationArchiveRoot.StartsWith(ExpectedPrefix))
		{
			IFileManager::Get().DeleteDirectory(*EphemeralAutomationArchiveRoot, false, true);
		}
	}
	Super::Deinitialize();
}

void UWorldStorageSubsystem::Tick(const float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick);
	if (!Runtime)
	{
		return;
	}
	const double TickStartSeconds = FPlatformTime::Seconds();
	Runtime->Residency.PollAccumulator += DeltaTime;
	Runtime->Metrics.AuthorityTickAccumulator += DeltaTime;
	Runtime->Checkpoint.AutosaveAccumulator += DeltaTime;
	bool bRunAuthorityStep = false;
	bool bRunResidencyMaintenance = false;
	if (Runtime->Core.bAuthority && Runtime->Metrics.AuthorityTickAccumulator >= AuthorityTickIntervalSeconds)
	{
		const double Elapsed = Runtime->Metrics.AuthorityTickAccumulator;
		Runtime->Metrics.AuthorityTickAccumulator =
			FMath::Fmod(Runtime->Metrics.AuthorityTickAccumulator, AuthorityTickIntervalSeconds);
		Runtime->Core.ManifestInfo.WorldSimulationTimeMilliseconds +=
			FMath::Max<int64>(1, FMath::RoundToInt64(Elapsed * 1000.0));
		bRunAuthorityStep = true;
	}
	if (Runtime->Residency.PollAccumulator >= ResidencyPollIntervalSeconds)
	{
		Runtime->Residency.PollAccumulator =
			FMath::Fmod(Runtime->Residency.PollAccumulator, ResidencyPollIntervalSeconds);
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_RefreshResidencySources);
			RefreshResidencySources(false);
		}
		bRunResidencyMaintenance = true;
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_ScheduleRequiredChunkLoads);
		ScheduleRequiredChunkLoads();
	}
	if (!Runtime->Core.bAuthority || bRunAuthorityStep)
	{
		const double InjectionStartSeconds = FPlatformTime::Seconds();
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_DrainCompletedLoads);
			DrainCompletedLoads(Runtime->Core.bAuthority ? ServerInjectionBudgetMilliseconds
											 : ClientInjectionBudgetMilliseconds);
		}
		const double InjectionMilliseconds = (FPlatformTime::Seconds() - InjectionStartSeconds) * 1000.0;
		Runtime->Metrics.LastInjectionMilliseconds = InjectionMilliseconds;
		CSV_CUSTOM_STAT(ElementSandboxWorldStorage, InjectionMilliseconds, InjectionMilliseconds,
						ECsvCustomStatOp::Set);
	}
	if (Runtime->Core.bAuthority && bRunAuthorityStep)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_AuthorityStepBroadcast);
		AuthorityStepEvent.Broadcast(Runtime->Core.ManifestInfo.WorldSimulationTimeMilliseconds);
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_DrainSnapshotPreparations);
		DrainSnapshotPreparations();
	}
	if (bRunResidencyMaintenance || Runtime->Residency.bEvictionRequested)
	{
		Runtime->Residency.bEvictionRequested = false;
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_EvictUnretainedChunks);
		EvictUnretainedChunks();
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_Tick_DrainCheckpointCompletion);
		DrainCheckpointCompletion();
	}
	if (Runtime->Core.bAuthority && Runtime->Checkpoint.AutosaveAccumulator >= AutosaveIntervalSeconds)
	{
		Runtime->Checkpoint.AutosaveAccumulator =
			FMath::Fmod(Runtime->Checkpoint.AutosaveAccumulator, AutosaveIntervalSeconds);
		RequestCheckpoint();
	}
	const double TickMilliseconds = (FPlatformTime::Seconds() - TickStartSeconds) * 1000.0;
	CSV_CUSTOM_STAT(ElementSandboxWorldStorage, TickMilliseconds, TickMilliseconds, ECsvCustomStatOp::Set);
	if (Runtime->Core.bAuthority && bRunAuthorityStep)
	{
		Runtime->Metrics.LastAuthorityStepMilliseconds = TickMilliseconds;
		CSV_CUSTOM_STAT(ElementSandboxWorldStorage, AuthorityStepMilliseconds, TickMilliseconds, ECsvCustomStatOp::Set);
	}
}

bool UWorldStorageSubsystem::IsTickable() const { return Runtime.IsValid(); }

TStatId UWorldStorageSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWorldStorageSubsystem, STATGROUP_Tickables);
}

FWorldEntityId UWorldStorageSubsystem::AllocateEntityId()
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->Core.bAuthority || Runtime->Core.ManifestInfo.NextEntityId == 0)
	{
		return {};
	}
	const FWorldEntityId Result(Runtime->Core.ManifestInfo.NextEntityId++);
	if (Runtime->Core.ManifestInfo.NextEntityId == 0)
	{
		UE_LOG(LogElementSandboxWorldStorage, Fatal, TEXT("FWorldEntityId 已耗尽。"));
	}
	return Result;
}

bool UWorldStorageSubsystem::IsStorageReady() const
{
	return Runtime && (!Runtime->Core.bAuthority || Runtime->Core.Archive.IsValid()) &&
		   (!Runtime->Core.bAuthority || Runtime->Core.ManifestInfo.WorldId.IsValid());
}

bool UWorldStorageSubsystem::IsAuthorityStorage() const { return Runtime && Runtime->Core.bAuthority; }

bool UWorldStorageSubsystem::IsChunkResident(const FWorldChunkCoord Coord) const
{
	const FChunkRuntime* Chunk = Runtime ? Runtime->Residency.Chunks.Find(Coord) : nullptr;
	return Chunk && Chunk->State == EChunkRuntimeState::Resident;
}

bool UWorldStorageSubsystem::IsAuthorityChunkReadyForActivation(const FWorldChunkCoord Coord) const
{
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Core.Archive.IsValid())
	{
		return false;
	}
	if (IsChunkResident(Coord))
	{
		return true;
	}
	if (Runtime->Entities.DirtyChunks.Contains(Coord))
	{
		const FChunkRuntime* Chunk = Runtime->Residency.Chunks.Find(Coord);
		const FWorldCompressedChunk* Snapshot = Runtime->Snapshots.CurrentCache.Find(Coord);
		return Chunk && Snapshot && Snapshot->Revision == Chunk->Revision;
	}
	FWorldChunkOffer ArchiveOffer;
	return !Runtime->Core.Archive->TryGetChunkOffer(Coord, ArchiveOffer);
}

bool UWorldStorageSubsystem::IsChunkDirty(const FWorldChunkCoord Coord) const
{
	return Runtime && Runtime->Entities.DirtyChunks.Contains(Coord);
}

int32 UWorldStorageSubsystem::GetChunkResidentEntityCount(const FWorldChunkCoord Coord) const
{
	const TSet<FWorldEntityId>* EntityIds = Runtime ? Runtime->Entities.ResidentIdsByChunk.Find(Coord) : nullptr;
	return EntityIds ? EntityIds->Num() : 0;
}

bool UWorldStorageSubsystem::RegisterDomainAdapter(const TSharedRef<IWorldStorageDomainAdapter> Adapter)
{
	check(IsInGameThread());
	if (!Runtime || !IsPersistentDomain(Adapter->GetDomain()))
	{
		return false;
	}
	if (const TSharedRef<IWorldStorageDomainAdapter>* Existing = Runtime->Core.Adapters.Find(Adapter->GetDomain()))
	{
		return &Existing->Get() == &Adapter.Get();
	}
	Runtime->Core.Adapters.Add(Adapter->GetDomain(), Adapter);
	return true;
}

bool UWorldStorageSubsystem::UnregisterDomainAdapter(const EWorldEntityDomain Domain,
                                                                                                         const IWorldStorageDomainAdapter& Adapter)
{
        check(IsInGameThread());
        if (!Runtime)
        {
                return false;
        }
        const TSharedRef<IWorldStorageDomainAdapter>* Existing = Runtime->Core.Adapters.Find(Domain);
        if (!Existing || &Existing->Get() != &Adapter)
        {
                return false;
        }

		// Domain Runtime 销毁后 Adapter 已不能再 Capture。先把所有已发布变更同步封口，
		// 才允许任一 Domain 从 WorldStorage 注销；否则 WorldStorage 最后析构时会留下
		// “Entity 已脏但 Adapter 已消失”的不可恢复窗口。
		if (Runtime->Core.bAuthority)
		{
			if (Runtime->Checkpoint.Future.IsValid())
			{
				Runtime->Checkpoint.Future.Wait();
				DrainCheckpointCompletion();
			}
			if (HasCommittedDirtyEntities(*Runtime))
			{
				if (!StartCheckpoint(true))
				{
					return false;
				}
				DrainCheckpointCompletion();
				if (HasCommittedDirtyEntities(*Runtime))
				{
					return false;
				}
			}
		}
		return Runtime->Core.Adapters.Remove(Domain) == 1;
}

#if WITH_DEV_AUTOMATION_TESTS
bool UWorldStorageSubsystem::ReplaceDomainAdapterForAutomation(const TSharedRef<IWorldStorageDomainAdapter> Adapter)
{
	check(IsInGameThread());
	if (!Runtime || !GIsAutomationTesting || !IsPersistentDomain(Adapter->GetDomain()))
	{
		return false;
	}
	Runtime->Core.Adapters.Add(Adapter->GetDomain(), Adapter);
	return true;
}
#endif

bool UWorldStorageSubsystem::RegisterFragmentPersistence(const EWorldEntityDomain Domain,
														 const UScriptStruct& FragmentType,
														 const EWorldFragmentPersistence Persistence)
{
	check(IsInGameThread());
	if (!Runtime || !IsClassifiableDomain(Domain))
	{
		return false;
	}
	const FWorldFragmentPersistenceKey Key{Domain, FName(*FragmentType.GetPathName())};
	if (const EWorldFragmentPersistence* Existing = Runtime->Core.FragmentPolicies.Find(Key))
	{
		return *Existing == Persistence;
	}
	Runtime->Core.FragmentPolicies.Add(Key, Persistence);
	return true;
}

TOptional<EWorldFragmentPersistence>
UWorldStorageSubsystem::FindFragmentPersistence(const EWorldEntityDomain Domain,
												const UScriptStruct& FragmentType) const
{
	if (!Runtime)
	{
		return {};
	}
	const FWorldFragmentPersistenceKey Key{Domain, FName(*FragmentType.GetPathName())};
	const EWorldFragmentPersistence* Value = Runtime->Core.FragmentPolicies.Find(Key);
	return Value ? TOptional<EWorldFragmentPersistence>(*Value) : TOptional<EWorldFragmentPersistence>();
}

EWorldResidentUpsertResult
UWorldStorageSubsystem::RegisterResidentEntity(const FWorldResidentEntityRegistration& Registration)
{
	check(IsInGameThread());
	if (!Runtime || !Registration.EntityId.IsSet() || !IsPersistentDomain(Registration.Domain) ||
		Registration.StateRevision == 0)
	{
		return EWorldResidentUpsertResult::Invalid;
	}
	if (Runtime->Entities.TombstoneRevisions.Contains(Registration.EntityId))
	{
		return EWorldResidentUpsertResult::RejectedTombstone;
	}
	if (FResidentEntry* Existing = Runtime->Entities.Residents.Find(Registration.EntityId))
	{
		if (Existing->Domain != Registration.Domain)
		{
			return EWorldResidentUpsertResult::RejectedTypeCollision;
		}
		if (Existing->StateRevision == Registration.StateRevision)
		{
			return EWorldResidentUpsertResult::SameRevision;
		}
		if (Existing->StateRevision > Registration.StateRevision)
		{
			return EWorldResidentUpsertResult::RejectedOlderRevision;
		}
		if (Existing->HomeChunk != Registration.HomeChunk)
		{
			if (TSet<FWorldEntityId>* PreviousChunkIds = Runtime->Entities.ResidentIdsByChunk.Find(Existing->HomeChunk))
			{
				PreviousChunkIds->Remove(Registration.EntityId);
				if (PreviousChunkIds->IsEmpty())
				{
					Runtime->Entities.ResidentIdsByChunk.Remove(Existing->HomeChunk);
				}
			}
			Runtime->Entities.ResidentIdsByChunk.FindOrAdd(Registration.HomeChunk).Add(Registration.EntityId);
			Runtime->Residency.Chunks.FindOrAdd(Registration.HomeChunk).State = EChunkRuntimeState::Resident;
			Runtime->ReferenceNewResidentChunk(Registration.HomeChunk);
		}
		Existing->HomeChunk = Registration.HomeChunk;
		Existing->StateRevision = Registration.StateRevision;
		return EWorldResidentUpsertResult::Updated;
	}
	FResidentEntry Entry;
	Entry.Domain = Registration.Domain;
	Entry.HomeChunk = Registration.HomeChunk;
	Entry.StateRevision = Registration.StateRevision;
	Runtime->Entities.Residents.Add(Registration.EntityId, Entry);
	Runtime->Entities.ResidentIdsByChunk.FindOrAdd(Registration.HomeChunk).Add(Registration.EntityId);
	Runtime->Residency.Chunks.FindOrAdd(Registration.HomeChunk).State = EChunkRuntimeState::Resident;
	Runtime->ReferenceNewResidentChunk(Registration.HomeChunk);
	return EWorldResidentUpsertResult::Inserted;
}

bool UWorldStorageSubsystem::RollbackUnpublishedResidentRegistration(
	const FWorldResidentEntityRegistration& Registration)
{
	check(IsInGameThread());
	const FResidentEntry* Resident = Runtime && Runtime->Core.bAuthority
		? Runtime->Entities.Residents.Find(Registration.EntityId)
		: nullptr;
	if (!Resident || Resident->Domain != Registration.Domain
		|| Resident->HomeChunk != Registration.HomeChunk
		|| Resident->StateRevision != Registration.StateRevision
		|| Runtime->Entities.DirtyEntities.Contains(Registration.EntityId)
		|| Runtime->Entities.TombstoneRevisions.Contains(Registration.EntityId))
	{
		return false;
	}
	RemoveResident(*Runtime, Registration.EntityId);
	return true;
}

bool UWorldStorageSubsystem::IsResident(const FWorldEntityId EntityId) const
{
	return Runtime && Runtime->Entities.Residents.Contains(EntityId);
}

bool UWorldStorageSubsystem::MarkEntityDirty(const FWorldEntityId EntityId, const uint32 StateRevision)
{
	check(IsInGameThread());
	FResidentEntry* Resident =
		Runtime && Runtime->Core.bAuthority ? Runtime->Entities.Residents.Find(EntityId) : nullptr;
	if (!Resident || StateRevision == 0 || StateRevision < Resident->StateRevision)
	{
		return false;
	}
	Resident->StateRevision = StateRevision;
	FDirtyEntityState& Dirty = Runtime->Entities.DirtyEntities.FindOrAdd(EntityId);
	Dirty.MutationSequence = Runtime->Entities.NextMutationSequence++;
	Dirty.Domain = Resident->Domain;
	Dirty.CurrentChunk = Resident->HomeChunk;
	Dirty.StateRevision = StateRevision;
        Dirty.bGameplayDestroyed = false;
        Dirty.StoredRecord.Reset();
		TagDirtyWithActiveBatch(*Runtime, EntityId, Dirty);
	Runtime->IncrementChunkRevision(Resident->HomeChunk);
	FWorldStorageEntityMutation Mutation;
	Mutation.Kind = EWorldStorageMutationKind::Upsert;
	Mutation.EntityId = EntityId;
	Mutation.PreviousChunk = Resident->HomeChunk;
	Mutation.CurrentChunk = Resident->HomeChunk;
	Mutation.StateRevision = StateRevision;
	AuthorityMutationEvent.Broadcast(Mutation);
	return true;
}

bool UWorldStorageSubsystem::UpdateEntityLocation(const FWorldEntityId EntityId, const FVector& NewLocation,
												  const uint32 StateRevision)
{
	check(IsInGameThread());
	FResidentEntry* Resident =
		Runtime && Runtime->Core.bAuthority ? Runtime->Entities.Residents.Find(EntityId) : nullptr;
	if (!Resident || NewLocation.ContainsNaN() || StateRevision == 0 || StateRevision < Resident->StateRevision)
	{
		return false;
	}
	const FWorldChunkCoord OldChunk = Resident->HomeChunk;
	const FWorldChunkCoord NewChunk = FWorldChunkCoord::FromWorldLocation(NewLocation);
	FDirtyEntityState& Dirty = Runtime->Entities.DirtyEntities.FindOrAdd(EntityId);
	Dirty.MutationSequence = Runtime->Entities.NextMutationSequence++;
	Dirty.Domain = Resident->Domain;
	Dirty.CurrentChunk = NewChunk;
	Dirty.StateRevision = StateRevision;
        Dirty.bGameplayDestroyed = false;
        Dirty.StoredRecord.Reset();
		TagDirtyWithActiveBatch(*Runtime, EntityId, Dirty);
	if (OldChunk != NewChunk)
	{
		Dirty.RemovedFromChunks.Add(OldChunk);
		if (TSet<FWorldEntityId>* OldChunkIds = Runtime->Entities.ResidentIdsByChunk.Find(OldChunk))
		{
			OldChunkIds->Remove(EntityId);
			if (OldChunkIds->IsEmpty())
			{
				Runtime->Entities.ResidentIdsByChunk.Remove(OldChunk);
			}
		}
		Runtime->Entities.ResidentIdsByChunk.FindOrAdd(NewChunk).Add(EntityId);
		Resident->HomeChunk = NewChunk;
		Runtime->ReferenceNewResidentChunk(NewChunk);
		Runtime->Residency.Chunks.FindOrAdd(NewChunk).State = EChunkRuntimeState::Resident;
		Runtime->IncrementChunkRevision(OldChunk);
	}
	Resident->StateRevision = StateRevision;
	Runtime->IncrementChunkRevision(NewChunk);
	FWorldStorageEntityMutation Mutation;
	Mutation.Kind = OldChunk == NewChunk ? EWorldStorageMutationKind::Upsert : EWorldStorageMutationKind::Move;
	Mutation.EntityId = EntityId;
	Mutation.PreviousChunk = OldChunk;
	Mutation.CurrentChunk = NewChunk;
	Mutation.StateRevision = StateRevision;
	AuthorityMutationEvent.Broadcast(Mutation);
	return true;
}

bool UWorldStorageSubsystem::GameplayDestroy(const FWorldEntityId EntityId, const uint32 StateRevision)
{
	check(IsInGameThread());
	const FResidentEntry* Resident =
		Runtime && Runtime->Core.bAuthority ? Runtime->Entities.Residents.Find(EntityId) : nullptr;
	if (!Resident || StateRevision == 0 || StateRevision < Resident->StateRevision)
	{
		return false;
	}
	FDirtyEntityState& Dirty = Runtime->Entities.DirtyEntities.FindOrAdd(EntityId);
	Dirty.MutationSequence = Runtime->Entities.NextMutationSequence++;
	Dirty.Domain = Resident->Domain;
	Dirty.CurrentChunk = Resident->HomeChunk;
	Dirty.StateRevision = StateRevision;
        Dirty.bGameplayDestroyed = true;
        Dirty.StoredRecord.Reset();
		TagDirtyWithActiveBatch(*Runtime, EntityId, Dirty);
	Runtime->IncrementChunkRevision(Resident->HomeChunk);
	Runtime->Entities.TombstoneRevisions.Add(EntityId, StateRevision);
	FWorldStorageEntityMutation Mutation;
	Mutation.Kind = EWorldStorageMutationKind::GameplayTombstone;
	Mutation.EntityId = EntityId;
	Mutation.PreviousChunk = Resident->HomeChunk;
	Mutation.CurrentChunk = Resident->HomeChunk;
	Mutation.StateRevision = StateRevision;
	RemoveResident(*Runtime, EntityId);
	AuthorityMutationEvent.Broadcast(Mutation);
	return true;
}

FWorldResidencySourceHandle UWorldStorageSubsystem::RegisterResidencySource(const FVector& Location,
																			const FVector& Forward)
{
	check(IsInGameThread());
	if (!Runtime || Location.ContainsNaN() || Forward.ContainsNaN())
	{
		return {};
	}
	int32 SlotIndex = INDEX_NONE;
	if (!Runtime->Residency.FreeSourceSlots.IsEmpty())
	{
		SlotIndex = Runtime->Residency.FreeSourceSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		SlotIndex = Runtime->Residency.Sources.AddDefaulted();
	}
	FResidencySourceSlot& Slot = Runtime->Residency.Sources[SlotIndex];
	Slot.bAlive = true;
	Slot.bForceRefresh = true;
	Slot.Location = Location;
	Slot.Forward = Forward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
	Slot.Center = FWorldChunkCoord::FromWorldLocation(Location);
	FWorldResidencySourceHandle Handle;
	Handle.Slot = SlotIndex;
	Handle.Generation = Slot.Generation;
	RefreshResidencySources(true);
	ResidencySourcesChangedEvent.Broadcast();
	return Handle;
}

bool UWorldStorageSubsystem::UpdateResidencySource(const FWorldResidencySourceHandle Source, const FVector& Location,
												   const FVector& Forward, const bool bForceRefresh)
{
	check(IsInGameThread());
	FResidencySourceSlot* Slot = Runtime ? Runtime->FindSource(Source) : nullptr;
	if (!Slot || Location.ContainsNaN() || Forward.ContainsNaN())
	{
		return false;
	}
	const FWorldChunkCoord NewCenter = FWorldChunkCoord::FromWorldLocation(Location);
	Slot->Location = Location;
	Slot->Forward = Forward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
	Slot->bForceRefresh |= bForceRefresh || NewCenter != Slot->Center;
	if (Slot->bForceRefresh)
	{
		RefreshResidencySources(true);
		ResidencySourcesChangedEvent.Broadcast();
	}
	return true;
}

bool UWorldStorageSubsystem::UnregisterResidencySource(const FWorldResidencySourceHandle Source)
{
	check(IsInGameThread());
	FResidencySourceSlot* Slot = Runtime ? Runtime->FindSource(Source) : nullptr;
	if (!Slot)
	{
		return false;
	}
	for (const FWorldChunkCoord& Coord : Slot->LoadChunks)
	{
		FChunkRuntime& Chunk = Runtime->Residency.Chunks.FindOrAdd(Coord);
		Chunk.LoadRefCount = FMath::Max(0, Chunk.LoadRefCount - 1);
	}
	for (const FWorldChunkCoord& Coord : Slot->RetentionChunks)
	{
		FChunkRuntime& Chunk = Runtime->Residency.Chunks.FindOrAdd(Coord);
		Chunk.RetentionRefCount = FMath::Max(0, Chunk.RetentionRefCount - 1);
	}
	Slot->LoadChunks.Reset();
	Slot->RetentionChunks.Reset();
	Slot->bAlive = false;
	Slot->Generation = NextRevision(Slot->Generation);
	Runtime->Residency.FreeSourceSlots.Add(Source.Slot);
	Runtime->Residency.bEvictionRequested = true;
	ResidencySourcesChangedEvent.Broadcast();
	return true;
}

void UWorldStorageSubsystem::CopyResidencyRetentionBoxes(TArray<FWorldChunkBox>& OutBoxes) const
{
	check(IsInGameThread());
	OutBoxes.Reset();
	if (!Runtime) return;
	for (const auto& Source : Runtime->Residency.Sources)
		if (Source.bAlive) OutBoxes.Add(FWorldChunkBox::Centered(Source.Center, RetentionEdgeChunks));
}

FWorldStorageMutationBatchHandle UWorldStorageSubsystem::BeginDelayedMutationBatch()
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Entities.DelayedMutationBatches.IsEmpty()
		|| Runtime->Entities.ActiveDelayedMutationBatchId != 0)
	{
		return {};
	}
        uint64 BatchId = Runtime->Entities.NextDelayedMutationBatchId++;
	if (BatchId == 0)
	{
		BatchId = Runtime->Entities.NextDelayedMutationBatchId++;
	}
		FDelayedMutationBatchState& State = Runtime->Entities.DelayedMutationBatches.Add(BatchId);
		State.bReady = !Runtime->Checkpoint.bInFlight && !HasCommittedDirtyEntities(*Runtime);
		if (!State.bReady && !Runtime->Checkpoint.bInFlight && !StartCheckpoint(false))
		{
				Runtime->Entities.DelayedMutationBatches.Remove(BatchId);
				return {};
		}
        return {BatchId};
}

bool UWorldStorageSubsystem::IsDelayedMutationBatchReady(
		const FWorldStorageMutationBatchHandle Batch)
{
		check(IsInGameThread());
		FDelayedMutationBatchState* State = Runtime && Batch.IsSet()
				? Runtime->Entities.DelayedMutationBatches.Find(Batch.Value) : nullptr;
		if (!State) return false;
		if (State->bReady) return true;
		if (Runtime->Checkpoint.bInFlight) return false;
		if (HasCommittedDirtyEntities(*Runtime))
		{
				StartCheckpoint(false);
				return false;
		}
		State->bReady = true;
		return true;
}

bool UWorldStorageSubsystem::ExecuteInDelayedMutationBatch(
	const FWorldStorageMutationBatchHandle Batch,
	const TFunctionRef<bool()> Mutation)
{
	check(IsInGameThread());
		FDelayedMutationBatchState* State = Runtime && Batch.IsSet()
				? Runtime->Entities.DelayedMutationBatches.Find(Batch.Value) : nullptr;
		if (!State || !State->bReady || Runtime->Entities.ActiveDelayedMutationBatchId != 0)
	{
		return false;
	}
	Runtime->Entities.ActiveDelayedMutationBatchId = Batch.Value;
	const bool bResult = Mutation();
	check(Runtime->Entities.ActiveDelayedMutationBatchId == Batch.Value);
	Runtime->Entities.ActiveDelayedMutationBatchId = 0;
	return bResult;
}

bool UWorldStorageSubsystem::CommitDelayedMutationBatch(const FWorldStorageMutationBatchHandle Batch)
{
	check(IsInGameThread());
	FDelayedMutationBatchState* State = Runtime && Batch.IsSet()
		? Runtime->Entities.DelayedMutationBatches.Find(Batch.Value) : nullptr;
		if (!State || !State->bReady || Runtime->Entities.ActiveDelayedMutationBatchId != 0)
	{
		return false;
	}
	for (const FWorldEntityId EntityId : State->EntityIds)
	{
		FDirtyEntityState* Dirty = Runtime->Entities.DirtyEntities.Find(EntityId);
		if (!Dirty || Dirty->DelayedMutationBatchId != Batch.Value)
		{
			return false;
		}
	}
	for (const FWorldEntityId EntityId : State->EntityIds)
	{
		FDirtyEntityState& Dirty = Runtime->Entities.DirtyEntities.FindChecked(EntityId);
		Dirty.DelayedMutationBatchId = 0;
		Dirty.MutationSequence = Runtime->Entities.NextMutationSequence++;
	}
	Runtime->Entities.DelayedMutationBatches.Remove(Batch.Value);
	return true;
}

bool UWorldStorageSubsystem::CancelEmptyDelayedMutationBatch(const FWorldStorageMutationBatchHandle Batch)
{
	check(IsInGameThread());
	const FDelayedMutationBatchState* State = Runtime && Batch.IsSet()
		? Runtime->Entities.DelayedMutationBatches.Find(Batch.Value) : nullptr;
	return State && State->EntityIds.IsEmpty()
		&& Runtime->Entities.ActiveDelayedMutationBatchId == 0
		&& Runtime->Entities.DelayedMutationBatches.Remove(Batch.Value) == 1;
}
