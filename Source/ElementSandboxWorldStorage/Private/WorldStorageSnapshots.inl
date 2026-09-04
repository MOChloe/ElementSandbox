// UWorldStorageSubsystem：当前 Revision Snapshot、网络应用、Checkpoint 与诊断统计。
// Snapshot/Checkpoint 都消费冻结的 COW 视图，不反向依赖任何 Gameplay 领域。

void UWorldStorageSubsystem::GetRelevantChunkOffers(const FWorldResidencySourceHandle Source,
												 TConstArrayView<FWorldChunkCoord> RequiredProjectionChunks,
												 const TSet<FWorldChunkCoord>& AcknowledgedChunks,
												 TArray<FWorldChunkOffer>& OutOffers,
												 TSet<FWorldChunkCoord>& OutRelevantChunks)
{
	OutOffers.Reset();
	OutRelevantChunks.Reset();
	FResidencySourceSlot* Slot = Runtime ? Runtime->FindSource(Source) : nullptr;
	if (!Slot || !Runtime->Core.bAuthority)
	{
		return;
	}
	OutRelevantChunks = Slot->LoadChunks;
	for (const FWorldChunkCoord& Coord : RequiredProjectionChunks)
	{
		OutRelevantChunks.Add(Coord);
	}
	TArray<FWorldChunkCoord> Coords = OutRelevantChunks.Array();
	// 已建立基线的连接消费 Live Delta。先排除已 ACK 坐标，避免为无人请求的 Dirty Chunk
	// 反复同步 Capture/异步压缩；OutRelevantChunks 必须保留它们，不能误触发 LeaveInterest。
	Coords.RemoveAll([&AcknowledgedChunks](const FWorldChunkCoord& Coord)
	{
		return AcknowledgedChunks.Contains(Coord);
	});
	Coords.Sort(
		[Location = Slot->Location, Forward = Slot->Forward](const FWorldChunkCoord& Left,
															 const FWorldChunkCoord& Right)
		{
			const double LeftScore = ChunkLoadPriorityScore(Left, Location, Forward);
			const double RightScore = ChunkLoadPriorityScore(Right, Location, Forward);
			return LeftScore != RightScore ? LeftScore < RightScore : Left < Right;
		});
	for (const FWorldChunkCoord& Coord : Coords)
	{
		FWorldChunkOffer Offer;
		if (!Runtime->Entities.DirtyChunks.Contains(Coord) && Runtime->Core.Archive.IsValid() &&
			Runtime->Core.Archive->TryGetChunkOffer(Coord, Offer))
		{
			Offer.WorldId = Runtime->Core.ManifestInfo.WorldId;
			OutOffers.Add(Offer);
			continue;
		}
		const FChunkRuntime* ChunkRuntime = Runtime->Residency.Chunks.Find(Coord);
		const FWorldCompressedChunk* Compressed = Runtime->Snapshots.CurrentCache.Find(Coord);
		if (ChunkRuntime && Compressed && Compressed->Revision == ChunkRuntime->Revision)
		{
			Offer.WorldId = Runtime->Core.ManifestInfo.WorldId;
			Offer.Coord = Coord;
			Offer.Revision = Compressed->Revision;
			Offer.ContentHash = Compressed->ContentHash;
			Offer.CompressedSize = Compressed->Bytes.Num();
			Offer.UncompressedSize = Compressed->UncompressedSize;
			OutOffers.Add(Offer);
			continue;
		}
		FString Error;
		if (!StartCurrentChunkSnapshotPreparation(Coord, Error) && !Error.IsEmpty())
		{
			UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("Chunk (%d,%d,%d) 无法开始异步 Snapshot：%s"), Coord.X,
				   Coord.Y, Coord.Z, *Error);
		}
	}
}

bool UWorldStorageSubsystem::StartCurrentChunkSnapshotPreparation(const FWorldChunkCoord Coord, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Core.Archive.IsValid())
	{
		OutError = TEXT("只有已打开存档的 Authority 可以准备 Chunk Snapshot。");
		return false;
	}
	const uint32 Revision = Runtime->Residency.Chunks.FindOrAdd(Coord).Revision;
	if (const FWorldCompressedChunk* Cached = Runtime->Snapshots.CurrentCache.Find(Coord);
		Cached && Cached->Revision == Revision)
	{
		return true;
	}
	if (Runtime->Snapshots.PreparationsInFlight.Contains(Coord))
	{
		return true;
	}

	TMap<FWorldEntityId, FDirtyEntityState> DirtyEntities;
	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : Runtime->Entities.DirtyEntities)
	{
		if (Pair.Value.CurrentChunk == Coord || Pair.Value.RemovedFromChunks.Contains(Coord))
		{
			DirtyEntities.Add(Pair.Key, Pair.Value);
		}
	}
	TArray<FWorldPersistentEntityRecord> ResidentRecords;
	if (const TSet<FWorldEntityId>* ResidentIds = Runtime->Entities.ResidentIdsByChunk.Find(Coord))
	{
		const TArray<FWorldEntityId> Ids = ResidentIds->Array();
		if (!Ids.IsEmpty() && !CaptureRecords(*Runtime, Ids, ResidentRecords, OutError))
		{
			return false;
		}
	}

	Runtime->Snapshots.PreparationsInFlight.Add(Coord);
	const TSharedRef<FWorldStorageArchive, ESPMode::ThreadSafe> Archive = Runtime->Core.Archive.ToSharedRef();
	const TSharedRef<FWorldStorageAsyncState, ESPMode::ThreadSafe> AsyncState = Runtime->Core.AsyncState;
	Async(EAsyncExecution::ThreadPool,
		  [Archive, AsyncState, Coord, Revision, DirtyEntities = MoveTemp(DirtyEntities),
		   ResidentRecords = MoveTemp(ResidentRecords)]() mutable
		  {
			  TUniquePtr<FCompletedChunkSnapshot> Result = MakeUnique<FCompletedChunkSnapshot>();
			  Result->Coord = Coord;
			  Result->Revision = Revision;
			  BuildCapturedChunkSnapshot(Archive, Coord, Revision, DirtyEntities, MoveTemp(ResidentRecords),
										 Result->Compressed, Result->Error);
			  if (AsyncState->bAcceptResults.Load())
			  {
				  AsyncState->CompletedSnapshots.Enqueue(MoveTemp(Result));
			  }
		  });
	return true;
}

void UWorldStorageSubsystem::DrainSnapshotPreparations()
{
	if (!Runtime)
	{
		return;
	}
	TUniquePtr<FCompletedChunkSnapshot> Completed;
	while (Runtime->Core.AsyncState->CompletedSnapshots.Dequeue(Completed))
	{
		Runtime->Snapshots.PreparationsInFlight.Remove(Completed->Coord);
		const FChunkRuntime* Current = Runtime->Residency.Chunks.Find(Completed->Coord);
		if (Completed->Error.IsEmpty() && Completed->Compressed.IsValid() && Current &&
			Current->Revision == Completed->Revision)
		{
			Runtime->Snapshots.CurrentCache.Add(Completed->Coord, Completed->Compressed);
		}
		else if (Completed->Error.IsEmpty())
		{
			Completed->Error = TEXT("Chunk 在异步 Snapshot 封口期间产生了更新；已丢弃过期结果。");
		}
		if (!Completed->Error.IsEmpty())
		{
			UE_LOG(LogElementSandboxWorldStorage, Verbose, TEXT("Chunk (%d,%d,%d) Snapshot 未发布：%s"),
				   Completed->Coord.X, Completed->Coord.Y, Completed->Coord.Z, *Completed->Error);
		}

		TArray<FWorldCompressedChunkReady> Waiters;
		if (TArray<FWorldCompressedChunkReady>* Existing = Runtime->Snapshots.Waiters.Find(Completed->Coord))
		{
			Waiters = MoveTemp(*Existing);
			Runtime->Snapshots.Waiters.Remove(Completed->Coord);
		}
		for (FWorldCompressedChunkReady& Waiter : Waiters)
		{
			FWorldCompressedChunk Result = Completed->Error.IsEmpty() ? Completed->Compressed : FWorldCompressedChunk();
			FString Error = Completed->Error;
			Waiter(MoveTemp(Result), MoveTemp(Error));
		}
	}
}

bool UWorldStorageSubsystem::RequestCurrentCompressedChunk(const FWorldChunkCoord Coord,
														   FWorldCompressedChunkReady&& Completion)
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Core.Archive.IsValid() || !Completion)
	{
		return false;
	}
	if (const FWorldCompressedChunk* Cached = Runtime->Snapshots.CurrentCache.Find(Coord))
	{
		FWorldCompressedChunk Result = *Cached;
		Completion(MoveTemp(Result), FString());
		return true;
	}
	if (Runtime->Entities.DirtyChunks.Contains(Coord))
	{
		FString Error;
		if (!StartCurrentChunkSnapshotPreparation(Coord, Error))
		{
			return false;
		}
		Runtime->Snapshots.Waiters.FindOrAdd(Coord).Add(MoveTemp(Completion));
		return true;
	}

	const TSharedRef<FWorldStorageArchive, ESPMode::ThreadSafe> Archive = Runtime->Core.Archive.ToSharedRef();
	const TSharedRef<FWorldStorageAsyncState, ESPMode::ThreadSafe> AsyncState = Runtime->Core.AsyncState;
	Async(EAsyncExecution::ThreadPool,
		  [Archive, AsyncState, Coord, Completion = MoveTemp(Completion)]() mutable
		  {
			  FWorldCompressedChunk Result;
			  FString Error;
			  Archive->ReadCompressedChunk(Coord, Result, Error);
			  if (!AsyncState->bAcceptResults.Load())
			  {
				  return;
			  }
			  AsyncTask(ENamedThreads::GameThread,
						[AsyncState, Result = MoveTemp(Result), Error = MoveTemp(Error),
						 Completion = MoveTemp(Completion)]() mutable
						{
							if (AsyncState->bAcceptResults.Load())
							{
								Completion(MoveTemp(Result), MoveTemp(Error));
							}
						});
		  });
	return true;
}

bool UWorldStorageSubsystem::SubmitNetworkChunk(const FGuid& WorldId, FWorldCompressedChunk Chunk,
												FWorldNetworkChunkApplied&& Completion)
{
	check(IsInGameThread());
	if (!Runtime || Runtime->Core.bAuthority || !WorldId.IsValid() || !Chunk.IsValid())
	{
		return false;
	}
	if (Runtime->Core.ManifestInfo.WorldId.IsValid() && Runtime->Core.ManifestInfo.WorldId != WorldId)
	{
		return false;
	}
	const FNetworkChunkLoadKey LoadKey{Chunk.Coord, Chunk.Revision};
	if (FNetworkChunkLoadWaiters* Waiters = Runtime->Residency.NetworkLoadWaiters.Find(LoadKey))
	{
		if (Waiters->ContentHash != Chunk.ContentHash)
		{
			return false;
		}
		if (Completion)
		{
			Waiters->Completions.Add(MoveTemp(Completion));
		}
		return true;
	}
	FChunkRuntime& ChunkRuntime = Runtime->Residency.Chunks.FindOrAdd(Chunk.Coord);
	if ((ChunkRuntime.State == EChunkRuntimeState::Loading ||
		 ChunkRuntime.State == EChunkRuntimeState::PendingInjection ||
		 ChunkRuntime.State == EChunkRuntimeState::Resident) &&
		ChunkRuntime.Revision >= Chunk.Revision)
	{
		if (ChunkRuntime.State == EChunkRuntimeState::Resident)
		{
			if (Completion)
			{
				Completion(true, FString(), MoveTemp(Chunk));
			}
			return true;
		}
		return false;
	}
	Runtime->Core.ManifestInfo.WorldId = WorldId;
	Runtime->Metrics.BytesReceived += Chunk.Bytes.Num();
	const FWorldChunkCoord Coord = Chunk.Coord;
	ChunkRuntime.State = EChunkRuntimeState::Loading;
	ChunkRuntime.Revision = Chunk.Revision;
	Runtime->Residency.LoadsInFlight.Add(Coord);
	FNetworkChunkLoadWaiters& Waiters = Runtime->Residency.NetworkLoadWaiters.Add(LoadKey);
	Waiters.ContentHash = Chunk.ContentHash;
	if (Completion)
	{
		Waiters.Completions.Add(MoveTemp(Completion));
	}
	// Worker 只带回这次快照；所有等待者在 GameThread 完成整份注入后才获得 ACK 资格。
	const TWeakObjectPtr<UWorldStorageSubsystem> WeakThis(this);
	FWorldNetworkChunkApplied NotifyWaiters =
		[WeakThis, LoadKey](const bool bSuccess, const FString& Error, FWorldCompressedChunk&& CompletedChunk)
		{
			UWorldStorageSubsystem* Storage = WeakThis.Get();
			if (!Storage || !Storage->Runtime)
			{
				return;
			}
			FNetworkChunkLoadWaiters* Pending = Storage->Runtime->Residency.NetworkLoadWaiters.Find(LoadKey);
			if (!Pending)
			{
				return;
			}
			auto Completions = MoveTemp(Pending->Completions);
			Storage->Runtime->Residency.NetworkLoadWaiters.Remove(LoadKey);
			for (int32 Index = 0; Index < Completions.Num(); ++Index)
			{
				if (Index + 1 == Completions.Num())
				{
					Completions[Index](bSuccess, Error, MoveTemp(CompletedChunk));
				}
				else
				{
					FWorldCompressedChunk Copy = CompletedChunk;
					Completions[Index](bSuccess, Error, MoveTemp(Copy));
				}
			}
		};
	const TSharedRef<FWorldStorageAsyncState, ESPMode::ThreadSafe> AsyncState = Runtime->Core.AsyncState;
	Async(EAsyncExecution::ThreadPool,
		  [AsyncState, Chunk = MoveTemp(Chunk), Completion = MoveTemp(NotifyWaiters)]() mutable
		  {
			  TUniquePtr<FCompletedChunkLoad> Result = MakeUnique<FCompletedChunkLoad>();
			  Result->Compressed = MoveTemp(Chunk);
			  Result->ClientCompletion = MoveTemp(Completion);
			  FWorldChunkCodec::Decompress(Result->Compressed, Result->Decoded, Result->Error);
			  if (AsyncState->bAcceptResults.Load())
			  {
				  AsyncState->CompletedLoads.Enqueue(MoveTemp(Result));
			  }
		  });
	return true;
}

bool UWorldStorageSubsystem::ApplyNetworkUpsert(const FWorldPersistentEntityRecord& Record)
{
	return ApplyNetworkUpsertBatch(MakeArrayView(&Record, 1));
}

bool UWorldStorageSubsystem::ApplyNetworkUpsertBatch(
	const TConstArrayView<FWorldPersistentEntityRecord> Records)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_ApplyNetworkUpsertBatch);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldStorage, ApplyNetworkUpsertBatch);
	if (!Runtime || Runtime->Core.bAuthority || Records.IsEmpty())
	{
		return false;
	}

	struct FUpsertGroup final
	{
		EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
		FWorldChunkCoord HomeChunk;
		TArray<FWorldPersistentEntityRecord, TInlineAllocator<32>> Records;
	};
	TArray<FUpsertGroup, TInlineAllocator<16>> Groups;
	TMap<FNetworkApplyGroupKey, int32> GroupIndexByKey;
	TSet<FWorldEntityId> Seen;
	GroupIndexByKey.Reserve(Records.Num());
	Seen.Reserve(Records.Num());
	for (const FWorldPersistentEntityRecord& Record : Records)
	{
		bool bAlreadySeen = false;
		Seen.Add(Record.EntityId, &bAlreadySeen);
		if (!Record.IsValid() || bAlreadySeen || Runtime->Entities.TombstoneRevisions.Contains(Record.EntityId))
		{
			return false;
		}
		if (const FResidentEntry* Existing = Runtime->Entities.Residents.Find(Record.EntityId))
		{
			if (Existing->Domain != Record.Domain || Existing->StateRevision > Record.StateRevision)
			{
				return false;
			}
			if (Existing->StateRevision == Record.StateRevision)
			{
				continue;
			}
		}

		const FWorldChunkCoord HomeChunk =
			FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation());
		if (!Runtime->Core.Adapters.Contains(Record.Domain))
		{
			return false;
		}
		const FNetworkApplyGroupKey GroupKey{Record.Domain, HomeChunk, 0};
		int32 GroupIndex = INDEX_NONE;
		if (const int32* ExistingGroupIndex = GroupIndexByKey.Find(GroupKey))
		{
			GroupIndex = *ExistingGroupIndex;
		}
		else
		{
			GroupIndex = Groups.AddDefaulted();
			FUpsertGroup& NewGroup = Groups[GroupIndex];
			NewGroup.Domain = Record.Domain;
			NewGroup.HomeChunk = HomeChunk;
			GroupIndexByKey.Add(GroupKey, GroupIndex);
		}
		Groups[GroupIndex].Records.Add(Record);
	}

	for (const FUpsertGroup& Group : Groups)
	{
		const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Group.Domain);
		FString Error;
		if (!Adapter || !Adapter->Get().RestoreBatch(Group.HomeChunk, Group.Records, Error))
		{
			return false;
		}
	}
	for (const FUpsertGroup& Group : Groups)
	{
		for (const FWorldPersistentEntityRecord& Record : Group.Records)
		{
			FWorldResidentEntityRegistration Registration;
			Registration.EntityId = Record.EntityId;
			Registration.Domain = Record.Domain;
			Registration.HomeChunk = Group.HomeChunk;
			Registration.StateRevision = Record.StateRevision;
			RegisterResidentEntity(Registration);
		}
		Runtime->Snapshots.CurrentCache.Remove(Group.HomeChunk);
	}
	return true;
}

bool UWorldStorageSubsystem::ApplyNetworkRemove(const FWorldEntityId EntityId, const uint32 StateRevision,
												const bool bGameplayDestroy)
{
	const FWorldNetworkEntityRemoval Removal{EntityId, StateRevision, bGameplayDestroy};
	return ApplyNetworkRemoveBatch(MakeArrayView(&Removal, 1));
}

bool UWorldStorageSubsystem::ApplyNetworkRemoveBatch(
	const TConstArrayView<FWorldNetworkEntityRemoval> Removals)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_ApplyNetworkRemoveBatch);
	CSV_SCOPED_TIMING_STAT(ElementSandboxWorldStorage, ApplyNetworkRemoveBatch);
	if (!Runtime || Runtime->Core.bAuthority || Removals.IsEmpty())
	{
		return false;
	}

	struct FRemovalGroup final
	{
		EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
		FWorldChunkCoord HomeChunk;
		bool bGameplayDestroy = false;
		TArray<FWorldEntityId, TInlineAllocator<32>> EntityIds;
	};
	TArray<FRemovalGroup, TInlineAllocator<16>> Groups;
	TMap<FNetworkApplyGroupKey, int32> GroupIndexByKey;
	TArray<FWorldEntityId, TInlineAllocator<64>> ResidentIds;
	TSet<FWorldEntityId> Seen;
	GroupIndexByKey.Reserve(Removals.Num());
	ResidentIds.Reserve(Removals.Num());
	Seen.Reserve(Removals.Num());

	for (const FWorldNetworkEntityRemoval& Removal : Removals)
	{
		bool bAlreadySeen = false;
		Seen.Add(Removal.EntityId, &bAlreadySeen);
		if (!Removal.IsValid() || bAlreadySeen)
		{
			return false;
		}
		if (Removal.bGameplayDestroy)
		{
			uint32& TombstoneRevision = Runtime->Entities.TombstoneRevisions.FindOrAdd(Removal.EntityId);
			TombstoneRevision = FMath::Max(TombstoneRevision, Removal.StateRevision);
		}
		const FResidentEntry* Resident = Runtime->Entities.Residents.Find(Removal.EntityId);
		if (!Resident)
		{
			if (!Removal.bGameplayDestroy)
			{
				return false;
			}
			continue;
		}
		if (Resident->StateRevision > Removal.StateRevision)
		{
			return false;
		}
		const FNetworkApplyGroupKey GroupKey{
			Resident->Domain, Resident->HomeChunk, Removal.bGameplayDestroy ? uint8{1} : uint8{0}};
		int32 GroupIndex = INDEX_NONE;
		if (const int32* ExistingGroupIndex = GroupIndexByKey.Find(GroupKey))
		{
			GroupIndex = *ExistingGroupIndex;
		}
		else
		{
			GroupIndex = Groups.AddDefaulted();
			FRemovalGroup& NewGroup = Groups[GroupIndex];
			NewGroup.Domain = Resident->Domain;
			NewGroup.HomeChunk = Resident->HomeChunk;
			NewGroup.bGameplayDestroy = Removal.bGameplayDestroy;
			GroupIndexByKey.Add(GroupKey, GroupIndex);
		}
		Groups[GroupIndex].EntityIds.Add(Removal.EntityId);
		ResidentIds.Add(Removal.EntityId);
	}

	for (const FRemovalGroup& Group : Groups)
	{
		const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Group.Domain);
		FString Error;
		const bool bRemoved = Adapter
			&& (Group.bGameplayDestroy
				? Adapter->Get().GameplayDestroyBatch(Group.HomeChunk, Group.EntityIds, Error)
				: Adapter->Get().LeaveInterestBatch(Group.HomeChunk, Group.EntityIds, Error));
		if (!bRemoved)
		{
			return false;
		}
	}
	for (const FWorldEntityId ResidentId : ResidentIds)
	{
		RemoveResident(*Runtime, ResidentId);
	}
	return true;
}

bool UWorldStorageSubsystem::CaptureResidentRecord(const FWorldEntityId EntityId,
												   FWorldPersistentEntityRecord& OutRecord, FString& OutError)
{
	check(IsInGameThread());
	OutRecord = {};
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Entities.Residents.Contains(EntityId))
	{
		OutError = TEXT("只能捕获 Authority 上仍 Resident 的 Entity。");
		return false;
	}
	TArray<FWorldPersistentEntityRecord> Records;
	if (!CaptureRecords(*Runtime, MakeArrayView(&EntityId, 1), Records, OutError) || Records.Num() != 1)
	{
		return false;
	}
	OutRecord = MoveTemp(Records[0]);
	return true;
}

void UWorldStorageSubsystem::RecordClientCacheResult(const bool bHit)
{
	check(IsInGameThread());
	if (!Runtime || Runtime->Core.bAuthority)
	{
		return;
	}
	if (bHit)
	{
		++Runtime->Metrics.CacheHitCount;
	}
	else
	{
		++Runtime->Metrics.CacheMissCount;
	}
}

bool UWorldStorageSubsystem::RequestCheckpoint()
{
	check(IsInGameThread());
	return StartCheckpoint(false);
}

bool UWorldStorageSubsystem::StartCheckpoint(const bool bSynchronous)
{
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Core.Archive.IsValid() || Runtime->Checkpoint.bInFlight)
	{
		return false;
	}
	TMap<FWorldEntityId, FDirtyEntityState> DirtySnapshot = Runtime->Entities.DirtyEntities;
	for (auto It = DirtySnapshot.CreateIterator(); It; ++It)
	{
		if (It.Value().DelayedMutationBatchId != 0)
		{
			It.RemoveCurrent();
		}
	}
	TArray<FWorldEntityId> NeedCapture;
	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : DirtySnapshot)
	{
		if (!Pair.Value.bGameplayDestroyed && !Pair.Value.StoredRecord.IsSet())
		{
			NeedCapture.Add(Pair.Key);
		}
	}
	if (!NeedCapture.IsEmpty())
	{
		TArray<FWorldPersistentEntityRecord> Captured;
		FString Error;
		if (!CaptureRecords(*Runtime, NeedCapture, Captured, Error))
		{
			UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("Checkpoint Capture 失败：%s"), *Error);
			return false;
		}
		for (FWorldPersistentEntityRecord& Record : Captured)
		{
			FDirtyEntityState* Dirty = DirtySnapshot.Find(Record.EntityId);
			if (!Dirty)
			{
				return false;
			}
			Dirty->StoredRecord = MoveTemp(Record);
		}
	}
	uint64 FrozenSequence = 0;
	TMap<FWorldChunkCoord, uint32> ChunkRevisions;
	for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : DirtySnapshot)
	{
		FrozenSequence = FMath::Max(FrozenSequence, Pair.Value.MutationSequence);
		ChunkRevisions.Add(Pair.Value.CurrentChunk,
						   Runtime->Residency.Chunks.FindOrAdd(Pair.Value.CurrentChunk).Revision);
		for (const FWorldChunkCoord& OldChunk : Pair.Value.RemovedFromChunks)
		{
			ChunkRevisions.Add(OldChunk, Runtime->Residency.Chunks.FindOrAdd(OldChunk).Revision);
		}
	}
	FWorldStorageManifestInfo NewInfo = Runtime->Core.ManifestInfo;
	NewInfo.Generation = Runtime->Core.Archive->GetManifestInfo().Generation + 1;
	NewInfo.LastCheckpointUtc = FDateTime::UtcNow();
	const TSharedRef<FWorldStorageArchive, ESPMode::ThreadSafe> Archive = Runtime->Core.Archive.ToSharedRef();
	const TSharedRef<FWorldStorageAsyncState, ESPMode::ThreadSafe> AsyncState = Runtime->Core.AsyncState;
	Runtime->Checkpoint.bInFlight = true;
	auto Work = [Archive, AsyncState, DirtySnapshot = MoveTemp(DirtySnapshot),
				 ChunkRevisions = MoveTemp(ChunkRevisions), NewInfo, FrozenSequence]() mutable
	{
		TUniquePtr<FCheckpointCompletion> Completion = MakeUnique<FCheckpointCompletion>();
		Completion->FrozenMutationSequence = FrozenSequence;
		Completion->ManifestInfo = NewInfo;
		Completion->bSuccess =
			ExecuteCheckpointWork(Archive, DirtySnapshot, ChunkRevisions, NewInfo, Completion->Error);
		if (AsyncState->bAcceptResults.Load())
		{
			AsyncState->CompletedCheckpoints.Enqueue(MoveTemp(Completion));
		}
	};
	if (bSynchronous)
	{
		Work();
	}
	else
	{
		Runtime->Checkpoint.Future = Async(EAsyncExecution::ThreadPool, MoveTemp(Work));
	}
	return true;
}

void UWorldStorageSubsystem::DrainCheckpointCompletion()
{
	if (!Runtime)
	{
		return;
	}
	TUniquePtr<FCheckpointCompletion> Completion;
	while (Runtime->Core.AsyncState->CompletedCheckpoints.Dequeue(Completion))
	{
		Runtime->Checkpoint.bInFlight = false;
		if (!Completion->bSuccess)
		{
			UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("Checkpoint 失败：%s"), *Completion->Error);
			continue;
		}
		check(Runtime->Core.ManifestInfo.WorldId == Completion->ManifestInfo.WorldId);
		// Worker 持有的是 Checkpoint 开始时冻结的 Manifest。异步写盘期间 Authority 时钟和
		// NextEntityId 仍会继续前进，因此这里只发布本次提交真正拥有的字段；整份覆盖会让
		// 运行态时间和身份分配器倒退，最终产生无效 Motion，甚至复用 WorldEntityId。
		Runtime->Core.ManifestInfo.Generation = Completion->ManifestInfo.Generation;
		Runtime->Core.ManifestInfo.LastCheckpointUtc = Completion->ManifestInfo.LastCheckpointUtc;
		Runtime->Checkpoint.LastCompletedMutationSequence = Completion->FrozenMutationSequence;
		for (auto It = Runtime->Entities.DirtyEntities.CreateIterator(); It; ++It)
		{
					if (It.Value().DelayedMutationBatchId == 0
						&& It.Value().MutationSequence <= Completion->FrozenMutationSequence)
			{
				It.RemoveCurrent();
			}
		}
		Runtime->Entities.DirtyChunks.Reset();
		for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : Runtime->Entities.DirtyEntities)
		{
			Runtime->Entities.DirtyChunks.Add(Pair.Value.CurrentChunk);
			Runtime->Entities.DirtyChunks.Append(Pair.Value.RemovedFromChunks);
		}
		Runtime->Snapshots.CurrentCache.Reset();
	}
}

FGuid UWorldStorageSubsystem::GetWorldId() const { return Runtime ? Runtime->Core.ManifestInfo.WorldId : FGuid(); }

FWorldStorageManifestInfo UWorldStorageSubsystem::GetManifestInfo() const
{
	return Runtime ? Runtime->Core.ManifestInfo : FWorldStorageManifestInfo();
}

bool UWorldStorageSubsystem::TryGetMostPopulatedChunk(FWorldChunkCoord& OutChunk) const
{
	OutChunk = {};
	if (!Runtime || !Runtime->Core.bAuthority)
	{
		return false;
	}
	// 当前 Resident 是玩家实际正在观察和交互的场景投影。旗舰事件的展示落点
	// 必须先从这组数据选择；Archive 的全地图代表 Chunk 只用于没有任何 Resident
	// 投影的启动/自动化场景，否则百万种子世界会把事件排到几十公里外。
	int32 BestResidentCount = 0;
	double BestDistanceSquared = TNumericLimits<double>::Max();
	for (const TPair<FWorldChunkCoord, TSet<FWorldEntityId>>& Pair : Runtime->Entities.ResidentIdsByChunk)
	{
		const int32 Count = Pair.Value.Num();
		const double DistanceSquared = static_cast<double>(Pair.Key.X) * Pair.Key.X
			+ static_cast<double>(Pair.Key.Y) * Pair.Key.Y;
		if (Count > BestResidentCount
			|| (Count == BestResidentCount && DistanceSquared < BestDistanceSquared)
			|| (Count == BestResidentCount && DistanceSquared == BestDistanceSquared && Pair.Key < OutChunk))
		{
			BestResidentCount = Count;
			BestDistanceSquared = DistanceSquared;
			OutChunk = Pair.Key;
		}
	}
	if (BestResidentCount > 0)
	{
		return true;
	}

	return Runtime->Core.Archive.IsValid()
		&& Runtime->Core.Archive->TryGetMostPopulatedChunk(OutChunk);
}

FWorldStorageRuntimeStats UWorldStorageSubsystem::GetRuntimeStats() const
{
	FWorldStorageRuntimeStats Stats;
	if (!Runtime)
	{
		return Stats;
	}
	Stats.ResidentEntityCount = Runtime->Entities.Residents.Num();
	Stats.ResidentChunkCount = Runtime->Entities.ResidentIdsByChunk.Num();
	Stats.PendingLoadCount = Runtime->Residency.LoadsInFlight.Num();
	Stats.PendingInjectionCount = Runtime->Residency.PendingInjection.Num();
	Stats.DirtyEntityCount = Runtime->Entities.DirtyEntities.Num();
	for (const FResidencySourceSlot& Source : Runtime->Residency.Sources)
	{
		Stats.ResidencySourceCount += Source.bAlive ? 1 : 0;
	}
	Stats.AwakePhysicsPinnedEntityCount = Runtime->Entities.AwakePhysicsPinnedSinceSeconds.Num();
	const double NowSeconds = FPlatformTime::Seconds();
	for (const TPair<FWorldEntityId, double>& Pair : Runtime->Entities.AwakePhysicsPinnedSinceSeconds)
	{
		Stats.OldestAwakePhysicsPinSeconds = FMath::Max(Stats.OldestAwakePhysicsPinSeconds, NowSeconds - Pair.Value);
	}
	Stats.CacheHitCount = Runtime->Metrics.CacheHitCount;
	Stats.CacheMissCount = Runtime->Metrics.CacheMissCount;
	Stats.BytesReceived = Runtime->Metrics.BytesReceived;
	Stats.BytesSent = Runtime->Metrics.BytesSent;
	Stats.CompleteStructureCount = Runtime->Core.ManifestInfo.CompleteStructureCount;
	Stats.BuildingEntityCount = Runtime->Core.ManifestInfo.BuildingEntityCount;
	Stats.WorldObjectEntityCount = Runtime->Core.ManifestInfo.WorldObjectEntityCount;
	Stats.WorldSimulationTimeMilliseconds = Runtime->Core.ManifestInfo.WorldSimulationTimeMilliseconds;
	Stats.LastInjectionMilliseconds = Runtime->Metrics.LastInjectionMilliseconds;
	Stats.LastAuthorityStepMilliseconds = Runtime->Metrics.LastAuthorityStepMilliseconds;
	Stats.bCheckpointInFlight = Runtime->Checkpoint.bInFlight;
	return Stats;
}

int64 UWorldStorageSubsystem::GetWorldSimulationTimeMilliseconds() const
{
	return Runtime ? Runtime->Core.ManifestInfo.WorldSimulationTimeMilliseconds : 0;
}

bool UWorldStorageSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
