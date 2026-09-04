// UWorldStorageSubsystem：观察源、Chunk 引用计数、异步加载、预算注入与 RuntimeEvict。
// Worker 只写 AsyncState 信箱，稳定状态仅在明确发布边界进入 Runtime。

void UWorldStorageSubsystem::RefreshResidencySources(const bool bForce)
{
	if (!Runtime)
	{
		return;
	}
	for (FResidencySourceSlot& Source : Runtime->Residency.Sources)
	{
		if (!Source.bAlive || (!bForce && !Source.bForceRefresh))
		{
			continue;
		}
		Source.Center = FWorldChunkCoord::FromWorldLocation(Source.Location);
		TSet<FWorldChunkCoord> NewLoad;
		TSet<FWorldChunkCoord> NewRetention;
		const FWorldChunkBox LoadBox = FWorldChunkBox::Centered(Source.Center, LoadEdgeChunks);
		const FWorldChunkBox RetentionBox = FWorldChunkBox::Centered(Source.Center, RetentionEdgeChunks);
		if (Runtime->Core.bAuthority && Runtime->Core.Archive.IsValid())
		{
			TArray<FWorldChunkCoord> Occupied;
			Runtime->Core.Archive->QueryOccupiedChunks(LoadBox, Occupied);
			NewLoad.Append(Occupied);
			Runtime->Core.Archive->QueryOccupiedChunks(RetentionBox, Occupied);
			NewRetention.Append(Occupied);
		}
		for (const TPair<FWorldChunkCoord, TSet<FWorldEntityId>>& Pair : Runtime->Entities.ResidentIdsByChunk)
		{
			if (LoadBox.Contains(Pair.Key))
			{
				NewLoad.Add(Pair.Key);
			}
			if (RetentionBox.Contains(Pair.Key))
			{
				NewRetention.Add(Pair.Key);
			}
		}
		// RuntimeEvict 会把尚未 Checkpoint 的最新记录冻结在 Dirty Overlay 中。
		// 这些 Chunk 即使尚未写入 Archive、也已不再 Resident，仍必须能被同一 Residency
		// 来源立即重新加载；否则对象会错误地消失到下一次 Checkpoint 才恢复。
		for (const FWorldChunkCoord& Coord : Runtime->Entities.DirtyChunks)
		{
			if (LoadBox.Contains(Coord))
			{
				NewLoad.Add(Coord);
			}
			if (RetentionBox.Contains(Coord))
			{
				NewRetention.Add(Coord);
			}
		}
		for (const FWorldChunkCoord& Coord : Source.LoadChunks)
		{
			if (!NewLoad.Contains(Coord))
			{
				FChunkRuntime& Chunk = Runtime->Residency.Chunks.FindOrAdd(Coord);
				Chunk.LoadRefCount = FMath::Max(0, Chunk.LoadRefCount - 1);
			}
		}
		for (const FWorldChunkCoord& Coord : NewLoad)
		{
			if (!Source.LoadChunks.Contains(Coord))
			{
				++Runtime->Residency.Chunks.FindOrAdd(Coord).LoadRefCount;
			}
		}
		for (const FWorldChunkCoord& Coord : Source.RetentionChunks)
		{
			if (!NewRetention.Contains(Coord))
			{
				FChunkRuntime& Chunk = Runtime->Residency.Chunks.FindOrAdd(Coord);
				Chunk.RetentionRefCount = FMath::Max(0, Chunk.RetentionRefCount - 1);
			}
		}
		for (const FWorldChunkCoord& Coord : NewRetention)
		{
			if (!Source.RetentionChunks.Contains(Coord))
			{
				++Runtime->Residency.Chunks.FindOrAdd(Coord).RetentionRefCount;
			}
		}
		Source.LoadChunks = MoveTemp(NewLoad);
		Source.RetentionChunks = MoveTemp(NewRetention);
		Source.bForceRefresh = false;
		Runtime->Residency.bEvictionRequested = true;
	}
}

void UWorldStorageSubsystem::ScheduleRequiredChunkLoads()
{
	if (!Runtime || !Runtime->Core.bAuthority || !Runtime->Core.Archive.IsValid())
	{
		return;
	}
	const int32 OutstandingCount = Runtime->Residency.LoadsInFlight.Num() + Runtime->Residency.PendingInjection.Num();
	const int32 AvailableReadSlots = FMath::Min(MaximumConcurrentChunkLoads - Runtime->Residency.LoadsInFlight.Num(),
												MaximumPendingChunkInjections - OutstandingCount);
	if (AvailableReadSlots <= 0)
	{
		return;
	}

	struct FLoadCandidate final
	{
		FWorldChunkCoord Coord;
		double PriorityScore = TNumericLimits<double>::Max();
		bool bActivationCore = false;
	};
	const auto IsHigherPriority = [](const FLoadCandidate& Left, const FLoadCandidate& Right)
	{
		if (Left.bActivationCore != Right.bActivationCore)
		{
			return Left.bActivationCore;
		}
		return Left.PriorityScore != Right.PriorityScore ? Left.PriorityScore < Right.PriorityScore
														 : Left.Coord < Right.Coord;
	};
	TArray<FLoadCandidate> Candidates;
	Candidates.Reserve(AvailableReadSlots);
	for (const TPair<FWorldChunkCoord, FChunkRuntime>& Pair : Runtime->Residency.Chunks)
	{
		if (Pair.Value.LoadRefCount <= 0 || Pair.Value.State != EChunkRuntimeState::Unloaded ||
			Runtime->Residency.LoadsInFlight.Contains(Pair.Key))
		{
			continue;
		}
		FLoadCandidate Candidate;
		Candidate.Coord = Pair.Key;
		for (const FResidencySourceSlot& Source : Runtime->Residency.Sources)
		{
			if (!Source.bAlive || !Source.LoadChunks.Contains(Pair.Key))
			{
				continue;
			}
			Candidate.PriorityScore =
				FMath::Min(Candidate.PriorityScore, ChunkLoadPriorityScore(Pair.Key, Source.Location, Source.Forward));
			const FWorldChunkCoord Delta(Pair.Key.X - Source.Center.X, Pair.Key.Y - Source.Center.Y,
										 Pair.Key.Z - Source.Center.Z);
			Candidate.bActivationCore |=
				FMath::Abs(Delta.X) <= 1 && FMath::Abs(Delta.Y) <= 1 && FMath::Abs(Delta.Z) <= 1;
		}

		int32 Lower = 0;
		int32 Upper = Candidates.Num();
		while (Lower < Upper)
		{
			const int32 Middle = Lower + (Upper - Lower) / 2;
			if (IsHigherPriority(Candidates[Middle], Candidate))
			{
				Lower = Middle + 1;
			}
			else
			{
				Upper = Middle;
			}
		}
		if (Lower < AvailableReadSlots)
		{
			Candidates.Insert(MoveTemp(Candidate), Lower);
			if (Candidates.Num() > AvailableReadSlots)
			{
				Candidates.Pop(EAllowShrinking::No);
			}
		}
	}
	const int32 StartCount = FMath::Min(AvailableReadSlots, Candidates.Num());
	for (int32 Index = 0; Index < StartCount; ++Index)
	{
		const FWorldChunkCoord Coord = Candidates[Index].Coord;
		const uint32 ChunkRevision = Runtime->Residency.Chunks.FindChecked(Coord).Revision;
		Runtime->Residency.Chunks.FindChecked(Coord).State = EChunkRuntimeState::Loading;
		Runtime->Residency.LoadsInFlight.Add(Coord);
		const TSharedRef<FWorldStorageArchive, ESPMode::ThreadSafe> Archive = Runtime->Core.Archive.ToSharedRef();
		const TSharedRef<FWorldStorageAsyncState, ESPMode::ThreadSafe> AsyncState = Runtime->Core.AsyncState;
		TMap<FWorldEntityId, FDirtyEntityState> DirtyEntities;
		if (Runtime->Entities.DirtyChunks.Contains(Coord))
		{
			for (const TPair<FWorldEntityId, FDirtyEntityState>& Pair : Runtime->Entities.DirtyEntities)
			{
				if (Pair.Value.CurrentChunk == Coord || Pair.Value.RemovedFromChunks.Contains(Coord))
				{
					DirtyEntities.Add(Pair.Key, Pair.Value);
				}
			}
		}
		Async(EAsyncExecution::ThreadPool,
			  [Archive, AsyncState, Coord, ChunkRevision, DirtyEntities = MoveTemp(DirtyEntities)]() mutable
			  {
				  TUniquePtr<FCompletedChunkLoad> Result = MakeUnique<FCompletedChunkLoad>();
				  Result->Compressed.Coord = Coord;
				  Result->Compressed.Revision = ChunkRevision;
				  FWorldChunkOffer ExistingOffer;
				  const bool bArchiveContainsChunk = Archive->TryGetChunkOffer(Coord, ExistingOffer);
				  if (DirtyEntities.IsEmpty() && !bArchiveContainsChunk)
				  {
					  // Residency 表示“该坐标的世界状态已经确定”，并不要求磁盘上必须存在 Blob。
					  // 大范围 Lease 必然覆盖大量从未写入过的空 Chunk；它们应一次成为空 Resident，
					  // 不能进入 Failed 后被每帧重复读取。
					  Result->Decoded.Coord = Coord;
					  Result->Decoded.Revision = FMath::Max(1u, ChunkRevision);
				  }
				  else
				  {
					  const bool bLoaded = DirtyEntities.IsEmpty()
											   ? Archive->ReadCompressedChunk(Coord, Result->Compressed, Result->Error)
											   : BuildCapturedChunkSnapshot(Archive, Coord, ChunkRevision, DirtyEntities,
																					{}, Result->Compressed, Result->Error);
					  if (bLoaded)
					  {
						  FWorldChunkCodec::Decompress(Result->Compressed, Result->Decoded, Result->Error);
					  }
				  }
				  if (AsyncState->bAcceptResults.Load())
				  {
					  AsyncState->CompletedLoads.Enqueue(MoveTemp(Result));
				  }
			  });
	}
}

void UWorldStorageSubsystem::DrainCompletedLoads(const double BudgetMilliseconds)
{
	if (!Runtime)
	{
		return;
	}
	TUniquePtr<FCompletedChunkLoad> Completed;
	while (Runtime->Core.AsyncState->CompletedLoads.Dequeue(Completed))
	{
		Runtime->Residency.LoadsInFlight.Remove(Completed->Compressed.Coord);
		if (Completed->Error.IsEmpty() && Completed->Decoded.IsValid() && Runtime->Core.bAuthority &&
			!ApplyDirtyOverlayToDecodedChunk(*Runtime, Completed->Decoded, Completed->Error))
		{
			Completed->Decoded = {};
		}
		if (!Completed->Error.IsEmpty() || !Completed->Decoded.IsValid())
		{
			FChunkRuntime& Chunk = Runtime->Residency.Chunks.FindOrAdd(Completed->Compressed.Coord);
			Chunk.State = EChunkRuntimeState::Failed;
			UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("Chunk (%d,%d,%d) 加载失败：%s"),
				   Completed->Compressed.Coord.X, Completed->Compressed.Coord.Y, Completed->Compressed.Coord.Z,
				   *Completed->Error);
			if (Completed->ClientCompletion)
			{
				FWorldNetworkChunkApplied Completion = MoveTemp(Completed->ClientCompletion);
				Completion(false, Completed->Error, MoveTemp(Completed->Compressed));
			}
			continue;
		}
		Runtime->Residency.Chunks.FindOrAdd(Completed->Decoded.Coord).State = EChunkRuntimeState::PendingInjection;
		TUniquePtr<FPendingChunkInjection> Pending = MakeUnique<FPendingChunkInjection>();
		Pending->Load = MoveTemp(Completed);
		Runtime->Residency.PendingInjection.Add(MoveTemp(Pending));
	}

	const auto GetPriority = [this](const FWorldChunkCoord& Coord)
	{
		TPair<bool, double> Result(false, TNumericLimits<double>::Max());
		for (const FResidencySourceSlot& Source : Runtime->Residency.Sources)
		{
			if (!Source.bAlive || !Source.LoadChunks.Contains(Coord))
			{
				continue;
			}
			Result.Value = FMath::Min(Result.Value, ChunkLoadPriorityScore(Coord, Source.Location, Source.Forward));
			const FWorldChunkCoord Delta(Coord.X - Source.Center.X, Coord.Y - Source.Center.Y,
										 Coord.Z - Source.Center.Z);
			Result.Key |= FMath::Abs(Delta.X) <= 1 && FMath::Abs(Delta.Y) <= 1 && FMath::Abs(Delta.Z) <= 1;
		}
		return Result;
	};
	Runtime->Residency.PendingInjection.Sort(
		[&GetPriority](const TUniquePtr<FPendingChunkInjection>& Left, const TUniquePtr<FPendingChunkInjection>& Right)
		{
			const TPair<bool, double> LeftPriority = GetPriority(Left->Load->Decoded.Coord);
			const TPair<bool, double> RightPriority = GetPriority(Right->Load->Decoded.Coord);
			if (LeftPriority.Key != RightPriority.Key)
			{
				return LeftPriority.Key;
			}
			return LeftPriority.Value != RightPriority.Value ? LeftPriority.Value < RightPriority.Value
															 : Left->Load->Decoded.Coord < Right->Load->Decoded.Coord;
		});

	const double StartSeconds = FPlatformTime::Seconds();
	const auto IsBudgetExhausted = [StartSeconds, BudgetMilliseconds]()
	{ return (FPlatformTime::Seconds() - StartSeconds) * 1000.0 >= BudgetMilliseconds; };
	const auto RollbackApplied = [this](FPendingChunkInjection& Pending, const FWorldChunkCoord& Coord)
	{
		struct FEvictBatch final
		{
			EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
			EWorldStorageRestorePhase Phase = EWorldStorageRestorePhase::Primary;
			TArray<FWorldEntityId>* EntityIds = nullptr;
		};
		TArray<FEvictBatch> Evictions;
		for (TPair<EWorldEntityDomain, TArray<FWorldEntityId>>& Pair : Pending.AppliedIdsByDomain)
		{
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Pair.Key);
			if (Adapter && !Pair.Value.IsEmpty())
			{
				Evictions.Add({Pair.Key, Adapter->Get().GetRestorePhase(), &Pair.Value});
			}
		}
		Evictions.Sort([](const FEvictBatch& Left, const FEvictBatch& Right)
					   { return static_cast<uint8>(Left.Phase) > static_cast<uint8>(Right.Phase); });
		for (const FEvictBatch& Eviction : Evictions)
		{
			FString Ignored;
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Eviction.Domain);
			if (Adapter)
			{
				Adapter->Get().RollbackRestoreBatch(Coord, *Eviction.EntityIds, Ignored);
			}
			for (const FWorldEntityId EntityId : *Eviction.EntityIds)
			{
				RemoveResident(*Runtime, EntityId);
			}
		}

		Pending.BackupRecords.Sort(
			[this](const FWorldPersistentEntityRecord& Left, const FWorldPersistentEntityRecord& Right)
			{
				const TSharedRef<IWorldStorageDomainAdapter>* LeftAdapter = Runtime->Core.Adapters.Find(Left.Domain);
				const TSharedRef<IWorldStorageDomainAdapter>* RightAdapter = Runtime->Core.Adapters.Find(Right.Domain);
				const uint8 LeftPhase =
					LeftAdapter ? static_cast<uint8>(LeftAdapter->Get().GetRestorePhase()) : MAX_uint8;
				const uint8 RightPhase =
					RightAdapter ? static_cast<uint8>(RightAdapter->Get().GetRestorePhase()) : MAX_uint8;
				return LeftPhase != RightPhase ? LeftPhase < RightPhase : Left.EntityId < Right.EntityId;
			});
		for (const FWorldPersistentEntityRecord& Backup : Pending.BackupRecords)
		{
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Backup.Domain);
			const FWorldChunkCoord BackupChunk =
				FWorldChunkCoord::FromWorldLocation(Backup.WorldTransform.GetLocation());
			FString Ignored;
			if (Adapter && Adapter->Get().RestoreBatch(BackupChunk, MakeArrayView(&Backup, 1), Ignored))
			{
				FWorldResidentEntityRegistration Registration;
				Registration.EntityId = Backup.EntityId;
				Registration.Domain = Backup.Domain;
				Registration.HomeChunk = BackupChunk;
				Registration.StateRevision = Backup.StateRevision;
				RegisterResidentEntity(Registration);
			}
		}
	};

	while (!Runtime->Residency.PendingInjection.IsEmpty())
	{
		FPendingChunkInjection& Pending = *Runtime->Residency.PendingInjection[0];
		const FWorldChunkCoord Coord = Pending.Load->Decoded.Coord;
		FChunkRuntime& ChunkRuntime = Runtime->Residency.Chunks.FindOrAdd(Coord);
		if (Runtime->Core.bAuthority && ChunkRuntime.LoadRefCount <= 0)
		{
			ChunkRuntime.State = EChunkRuntimeState::Unloaded;
			Runtime->Residency.PendingInjection.RemoveAt(0, 1, EAllowShrinking::No);
			continue;
		}

		bool bRejected = false;
		FString Error;
		while (!Pending.bScanComplete && Pending.NextScanIndex < Pending.Load->Decoded.Records.Num())
		{
			FWorldPersistentEntityRecord& Record = Pending.Load->Decoded.Records[Pending.NextScanIndex++];
			if (!Record.IsValid() ||
				FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != Coord ||
				Pending.SeenEntityIds.Contains(Record.EntityId))
			{
				bRejected = true;
				Error = TEXT("Chunk 包含无效、重复或 HomeChunk 不一致的记录。");
				break;
			}
			Pending.SeenEntityIds.Add(Record.EntityId);
			if (const FResidentEntry* Existing = Runtime->Entities.Residents.Find(Record.EntityId))
			{
				if (Existing->Domain != Record.Domain)
				{
					bRejected = true;
					Error = TEXT("Chunk EntityId 与 Resident Directory 的领域类型冲突。");
					break;
				}
				if (Existing->StateRevision >= Record.StateRevision)
				{
					continue;
				}
			}
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Record.Domain);
			if (!Adapter)
			{
				bRejected = true;
				Error = TEXT("Chunk 领域尚未注册持久化 Adapter。");
				break;
			}
			FDomainRestoreBatch& Batch = Pending.BatchesByDomain.FindOrAdd(Record.Domain);
			Batch.Domain = Record.Domain;
			Batch.Phase = Adapter->Get().GetRestorePhase();
			Batch.Records.Add(MoveTemp(Record));
			if ((Pending.NextScanIndex & 31) == 0 && IsBudgetExhausted())
			{
				return;
			}
		}
		if (!bRejected && !Pending.bScanComplete && Pending.NextScanIndex == Pending.Load->Decoded.Records.Num())
		{
			for (TPair<EWorldEntityDomain, FDomainRestoreBatch>& Pair : Pending.BatchesByDomain)
			{
				Pending.OrderedBatches.Add(MoveTemp(Pair.Value));
			}
			Pending.BatchesByDomain.Reset();
			Pending.OrderedBatches.Sort(
				[](const FDomainRestoreBatch& Left, const FDomainRestoreBatch& Right)
				{
					return Left.Phase != Right.Phase
							   ? static_cast<uint8>(Left.Phase) < static_cast<uint8>(Right.Phase)
							   : static_cast<uint8>(Left.Domain) < static_cast<uint8>(Right.Domain);
				});
			Pending.bScanComplete = true;
		}

		while (!bRejected && Pending.bScanComplete && Pending.NextBatchIndex < Pending.OrderedBatches.Num())
		{
			FDomainRestoreBatch& Batch = Pending.OrderedBatches[Pending.NextBatchIndex];
			if (Batch.NextRecordIndex >= Batch.Records.Num())
			{
				++Pending.NextBatchIndex;
				continue;
			}
			const int32 MaximumRecordsPerSlice =
				Runtime->Core.bAuthority ? MaximumServerRecordsPerRestoreBatch : MaximumClientRecordsPerRestoreBatch;
			const int32 EndIndex = FMath::Min(Batch.NextRecordIndex + MaximumRecordsPerSlice, Batch.Records.Num());
			const TConstArrayView<FWorldPersistentEntityRecord> CandidateSlice =
				MakeArrayView(Batch.Records).Slice(Batch.NextRecordIndex, EndIndex - Batch.NextRecordIndex);
			TArray<FWorldEntityId,
				TInlineAllocator<MaximumServerRecordsPerRestoreBatch + MaximumClientRecordsPerRestoreBatch>> ExistingIds;
			TArray<uint8,
				TInlineAllocator<MaximumServerRecordsPerRestoreBatch + MaximumClientRecordsPerRestoreBatch>> InclusionFlags;
			InclusionFlags.Reserve(CandidateSlice.Num());
			bool bNeedsFilteredSlice = false;
			for (const FWorldPersistentEntityRecord& Record : CandidateSlice)
			{
				bool bInclude = true;
				if (const FResidentEntry* Existing = Runtime->Entities.Residents.Find(Record.EntityId))
				{
					if (Existing->Domain != Record.Domain)
					{
						bRejected = true;
						Error = TEXT("Restore 前检测到跨领域 EntityId 冲突。");
						break;
					}
					if (Existing->StateRevision >= Record.StateRevision)
					{
						bInclude = false;
					}
					else if (!Runtime->Entities.TombstoneRevisions.Contains(Record.EntityId)
						&& !Pending.BackupEntityIds.Contains(Record.EntityId))
					{
						ExistingIds.Add(Record.EntityId);
					}
				}
				if (Runtime->Entities.TombstoneRevisions.Contains(Record.EntityId))
				{
					bInclude = false;
				}
				InclusionFlags.Add(bInclude ? 1 : 0);
				bNeedsFilteredSlice |= !bInclude;
			}
			TArray<FWorldPersistentEntityRecord,
				TInlineAllocator<MaximumServerRecordsPerRestoreBatch + MaximumClientRecordsPerRestoreBatch>> FilteredSlice;
			TConstArrayView<FWorldPersistentEntityRecord> Slice = CandidateSlice;
			if (!bRejected && bNeedsFilteredSlice)
			{
				FilteredSlice.Reserve(CandidateSlice.Num());
				for (int32 Index = 0; Index < CandidateSlice.Num(); ++Index)
				{
					if (InclusionFlags[Index] != 0)
					{
						FilteredSlice.Add(CandidateSlice[Index]);
					}
				}
				Slice = FilteredSlice;
			}
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Batch.Domain);
			if (!bRejected && Adapter && !ExistingIds.IsEmpty())
			{
				TArray<FWorldPersistentEntityRecord> Backups;
				if (!Adapter->Get().CaptureBatch(ExistingIds, Backups, Error) || Backups.Num() != ExistingIds.Num())
				{
					bRejected = true;
				}
				else
				{
					for (FWorldPersistentEntityRecord& Backup : Backups)
					{
						Pending.BackupEntityIds.Add(Backup.EntityId);
						Pending.BackupRecords.Add(MoveTemp(Backup));
					}
				}
			}
			if (!bRejected && !Slice.IsEmpty())
			{
				bool bRestored = false;
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_ChunkInjection_AdapterRestore);
					bRestored = Adapter && Adapter->Get().RestoreBatch(Coord, Slice, Error);
				}
				if (!bRestored)
				{
					bRejected = true;
				}
			}
			if (!bRejected)
			{
				for (const FWorldPersistentEntityRecord& Record : Slice)
				{
					Pending.AppliedIdsByDomain.FindOrAdd(Record.Domain).AddUnique(Record.EntityId);
				}
				for (const FWorldPersistentEntityRecord& Record : Slice)
				{
					FWorldResidentEntityRegistration Registration;
					Registration.EntityId = Record.EntityId;
					Registration.Domain = Record.Domain;
					Registration.HomeChunk = Coord;
					Registration.StateRevision = Record.StateRevision;
					const EWorldResidentUpsertResult Result = RegisterResidentEntity(Registration);
					if (Result == EWorldResidentUpsertResult::RejectedTypeCollision ||
						Result == EWorldResidentUpsertResult::RejectedOlderRevision ||
						Result == EWorldResidentUpsertResult::RejectedTombstone ||
						Result == EWorldResidentUpsertResult::Invalid)
					{
						bRejected = true;
						Error = TEXT("Restore 后无法登记 Resident Entity。");
						break;
					}
				}
			}
			Batch.NextRecordIndex = EndIndex;
			if (IsBudgetExhausted())
			{
				return;
			}
		}

		if (bRejected)
		{
			RollbackApplied(Pending, Coord);
			ChunkRuntime.State = EChunkRuntimeState::Failed;
			UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("Chunk %d,%d,%d 注入失败：%s"), Coord.X, Coord.Y, Coord.Z,
				   *Error);
			if (Pending.Load->ClientCompletion)
			{
				FWorldNetworkChunkApplied Completion = MoveTemp(Pending.Load->ClientCompletion);
				Completion(false, Error, MoveTemp(Pending.Load->Compressed));
			}
			Runtime->Residency.PendingInjection.RemoveAt(0, 1, EAllowShrinking::No);
			continue;
		}
		if (!Pending.bScanComplete || Pending.NextBatchIndex < Pending.OrderedBatches.Num())
		{
			return;
		}

		ChunkRuntime.State = EChunkRuntimeState::Resident;
		ChunkRuntime.Revision = Pending.Load->Decoded.Revision;
		if (Pending.Load->ClientCompletion)
		{
			FWorldNetworkChunkApplied Completion = MoveTemp(Pending.Load->ClientCompletion);
			Completion(true, FString(), MoveTemp(Pending.Load->Compressed));
		}
		if (!Runtime->Core.bAuthority)
		{
			// RegisterResidentEntity 已逐条把当前 Chunk 纳入匹配来源；空 Chunk 也在这里
			// 补一次。不能在每个网络 Chunk 完成时全量 Refresh——它会反复扫描全部
			// Resident Chunk，令装填过程随已加载数量退化成近似 O(n^2)。
			Runtime->ReferenceNewResidentChunk(Coord);
			Runtime->Residency.bEvictionRequested = true;
		}
		Runtime->Residency.PendingInjection.RemoveAt(0, 1, EAllowShrinking::No);
		if (IsBudgetExhausted())
		{
			break;
		}
	}
}

void UWorldStorageSubsystem::EvictUnretainedChunks()
{
	if (!Runtime)
	{
		return;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	for (auto It = Runtime->Entities.AwakePhysicsPinnedSinceSeconds.CreateIterator(); It; ++It)
	{
		const FResidentEntry* Resident = Runtime->Entities.Residents.Find(It.Key());
		const FChunkRuntime* Chunk = Resident ? Runtime->Residency.Chunks.Find(Resident->HomeChunk) : nullptr;
		if (!Resident || Resident->Domain != EWorldEntityDomain::WorldObject || !Chunk || Chunk->RetentionRefCount > 0)
		{
			It.RemoveCurrent();
		}
	}

	TArray<FWorldChunkCoord> Candidates;
	for (const TPair<FWorldChunkCoord, FChunkRuntime>& Pair : Runtime->Residency.Chunks)
	{
		if (Pair.Value.RetentionRefCount == 0 && Pair.Value.State == EChunkRuntimeState::Resident)
		{
			Candidates.Add(Pair.Key);
		}
	}
	for (const FWorldChunkCoord& Coord : Candidates)
	{
		TSet<FWorldEntityId>* ChunkIds = Runtime->Entities.ResidentIdsByChunk.Find(Coord);
		if (!ChunkIds || ChunkIds->IsEmpty())
		{
			Runtime->Residency.Chunks.FindOrAdd(Coord).State = EChunkRuntimeState::Unloaded;
			continue;
		}
		TMap<EWorldEntityDomain, TArray<FWorldEntityId>> EvictByDomain;
		for (const FWorldEntityId EntityId : *ChunkIds)
		{
			const FResidentEntry* Resident = Runtime->Entities.Residents.Find(EntityId);
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter =
				Resident ? Runtime->Core.Adapters.Find(Resident->Domain) : nullptr;
			if (Resident && Adapter && Adapter->Get().CanRuntimeEvict(EntityId))
			{
				Runtime->Entities.AwakePhysicsPinnedSinceSeconds.Remove(EntityId);
				EvictByDomain.FindOrAdd(Resident->Domain).Add(EntityId);
			}
			else if (Resident && Adapter && Resident->Domain == EWorldEntityDomain::WorldObject)
			{
				Runtime->Entities.AwakePhysicsPinnedSinceSeconds.FindOrAdd(EntityId, NowSeconds);
			}
		}

		struct FEvictDomainBatch final
		{
			EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
			EWorldStorageRestorePhase Phase = EWorldStorageRestorePhase::Primary;
			TArray<FWorldEntityId> EntityIds;
		};
		TArray<FEvictDomainBatch> OrderedEvictions;
		for (TPair<EWorldEntityDomain, TArray<FWorldEntityId>>& Pair : EvictByDomain)
		{
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Pair.Key);
			if (!Adapter)
			{
				continue;
			}
			FEvictDomainBatch& Batch = OrderedEvictions.AddDefaulted_GetRef();
			Batch.Domain = Pair.Key;
			Batch.Phase = Adapter->Get().GetRestorePhase();
			Batch.EntityIds = MoveTemp(Pair.Value);
		}
		OrderedEvictions.Sort([](const FEvictDomainBatch& Left, const FEvictDomainBatch& Right)
							  { return static_cast<uint8>(Left.Phase) > static_cast<uint8>(Right.Phase); });

		for (FEvictDomainBatch& Batch : OrderedEvictions)
		{
			if (Runtime->Core.bAuthority)
			{
				FWorldStorageRuntimeEvictPreparation Preparation;
				Preparation.Domain = Batch.Domain;
				Preparation.HomeChunk = Coord;
				Preparation.EntityIds = Batch.EntityIds;
				Preparation.WorldTimeMilliseconds = Runtime->Core.ManifestInfo.WorldSimulationTimeMilliseconds;
				PrepareRuntimeEvictBatchEvent.Broadcast(Preparation);
				if (!Preparation.CanProceed())
				{
					UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("RuntimeEvict 封口失败：%s"), *Preparation.Error);
					continue;
				}
			}

			TArray<FWorldEntityId> DirtyIds;
			if (Runtime->Core.bAuthority)
			{
				for (const FWorldEntityId EntityId : Batch.EntityIds)
				{
					if (FDirtyEntityState* Dirty = Runtime->Entities.DirtyEntities.Find(EntityId);
						Dirty && !Dirty->bGameplayDestroyed)
					{
						DirtyIds.Add(EntityId);
					}
				}
			}
			if (!DirtyIds.IsEmpty())
			{
				TArray<FWorldPersistentEntityRecord> Captured;
				FString CaptureError;
				if (!CaptureRecords(*Runtime, DirtyIds, Captured, CaptureError))
				{
					UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("RuntimeEvict 捕获失败：%s"), *CaptureError);
					continue;
				}
				for (FWorldPersistentEntityRecord& Record : Captured)
				{
					Runtime->Entities.DirtyEntities.FindChecked(Record.EntityId).StoredRecord = MoveTemp(Record);
				}
			}

			FString EvictError;
			const TSharedRef<IWorldStorageDomainAdapter>* Adapter = Runtime->Core.Adapters.Find(Batch.Domain);
			const bool bRemoved =
				Adapter &&
				(Runtime->Core.bAuthority ? Adapter->Get().RuntimeEvictBatch(Coord, Batch.EntityIds, EvictError)
										  : Adapter->Get().LeaveInterestBatch(Coord, Batch.EntityIds, EvictError));
			if (!bRemoved)
			{
				UE_LOG(LogElementSandboxWorldStorage, Error, TEXT("RuntimeEvict 失败：%s"), *EvictError);
				continue;
			}
			for (const FWorldEntityId EntityId : Batch.EntityIds)
			{
				RemoveResident(*Runtime, EntityId);
			}
		}
		if (!Runtime->Entities.ResidentIdsByChunk.Contains(Coord))
		{
			Runtime->Residency.Chunks.FindOrAdd(Coord).State = EChunkRuntimeState::Unloaded;
		}
	}

	double OldestPinnedSeconds = 0.0;
	for (const TPair<FWorldEntityId, double>& Pair : Runtime->Entities.AwakePhysicsPinnedSinceSeconds)
	{
		OldestPinnedSeconds = FMath::Max(OldestPinnedSeconds, NowSeconds - Pair.Value);
	}
	if (!Runtime->Entities.AwakePhysicsPinnedSinceSeconds.IsEmpty() && OldestPinnedSeconds >= 30.0 &&
		NowSeconds - Runtime->Metrics.LastAwakePhysicsDiagnosticSeconds >= 30.0)
	{
		Runtime->Metrics.LastAwakePhysicsDiagnosticSeconds = NowSeconds;
		UE_LOG(LogElementSandboxWorldStorage, Warning,
			   TEXT("%d 个远距离 WorldObject 因 Awake Physics 保持 Resident，最久 %.1f 秒。"),
			   Runtime->Entities.AwakePhysicsPinnedSinceSeconds.Num(), OldestPinnedSeconds);
	}
}
