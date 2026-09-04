#pragma once

#include "Async/Async.h"
#include "Containers/Queue.h"
#include "WorldStorageSubsystem.h"

enum class EChunkRuntimeState : uint8
{
	Unloaded,
	Loading,
	PendingInjection,
	Resident,
	Failed
};

struct FResidentEntry final
{
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	FWorldChunkCoord HomeChunk;
	uint32 StateRevision = 0;
};

struct FDirtyEntityState final
{
	uint64 MutationSequence = 0;
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	FWorldChunkCoord CurrentChunk;
	TSet<FWorldChunkCoord> RemovedFromChunks;
	uint32 StateRevision = 0;
	bool bGameplayDestroyed = false;
	uint64 DelayedMutationBatchId = 0;
	TOptional<FWorldPersistentEntityRecord> StoredRecord;
};

struct FChunkRuntime final
{
	int32 LoadRefCount = 0;
	int32 RetentionRefCount = 0;
	uint32 Revision = 1;
	EChunkRuntimeState State = EChunkRuntimeState::Unloaded;
};

struct FResidencySourceSlot final
{
	uint32 Generation = 1;
	bool bAlive = false;
	bool bForceRefresh = true;
	FVector Location = FVector::ZeroVector;
	FVector Forward = FVector::ForwardVector;
	FWorldChunkCoord Center;
	TSet<FWorldChunkCoord> LoadChunks;
	TSet<FWorldChunkCoord> RetentionChunks;
};

struct FDelayedMutationBatchState final
{
        TSet<FWorldEntityId> EntityIds;
		bool bReady = false;
};

struct FCompletedChunkLoad final
{
	FWorldCompressedChunk Compressed;
	FWorldChunkData Decoded;
	FString Error;
	FWorldNetworkChunkApplied ClientCompletion;
};

struct FNetworkChunkLoadKey final
{
	FWorldChunkCoord Coord;
	uint32 Revision = 0;

	friend bool operator==(const FNetworkChunkLoadKey& Left, const FNetworkChunkLoadKey& Right)
	{
		return Left.Coord == Right.Coord && Left.Revision == Right.Revision;
	}
	friend uint32 GetTypeHash(const FNetworkChunkLoadKey& Key)
	{
		return HashCombineFast(GetTypeHash(Key.Coord), GetTypeHash(Key.Revision));
	}
};

/** 同一快照只解码、注入一次；重复网络或缓存请求共享最终完成通知。 */
struct FNetworkChunkLoadWaiters final
{
	FWorldChunkContentHash ContentHash;
	TArray<FWorldNetworkChunkApplied, TInlineAllocator<1>> Completions;
};

struct FDomainRestoreBatch final
{
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	EWorldStorageRestorePhase Phase = EWorldStorageRestorePhase::Primary;
	TArray<FWorldPersistentEntityRecord> Records;
	int32 NextRecordIndex = 0;
};

/** 解码结果在 GameThread 上按预算逐步转成领域投影。 */
struct FPendingChunkInjection final
{
	TUniquePtr<FCompletedChunkLoad> Load;
	TMap<EWorldEntityDomain, FDomainRestoreBatch> BatchesByDomain;
	TArray<FDomainRestoreBatch> OrderedBatches;
	TSet<FWorldEntityId> SeenEntityIds;
	TMap<EWorldEntityDomain, TArray<FWorldEntityId>> AppliedIdsByDomain;
	TArray<FWorldPersistentEntityRecord> BackupRecords;
	TSet<FWorldEntityId> BackupEntityIds;
	int32 NextScanIndex = 0;
	int32 NextBatchIndex = 0;
	bool bScanComplete = false;
};

struct FCheckpointCompletion final
{
	bool bSuccess = false;
	uint64 FrozenMutationSequence = 0;
	FWorldStorageManifestInfo ManifestInfo;
	FString Error;
};

struct FCompletedChunkSnapshot final
{
	FWorldChunkCoord Coord;
	uint32 Revision = 0;
	FWorldCompressedChunk Compressed;
	FString Error;
};

/** Worker 只持有这个线程安全信箱，不接触 GameThread Runtime。 */
struct FWorldStorageAsyncState final
{
	TAtomic<bool> bAcceptResults{true};
	TQueue<TUniquePtr<FCompletedChunkLoad>, EQueueMode::Mpsc> CompletedLoads;
	TQueue<TUniquePtr<FCheckpointCompletion>, EQueueMode::Mpsc> CompletedCheckpoints;
	TQueue<TUniquePtr<FCompletedChunkSnapshot>, EQueueMode::Mpsc> CompletedSnapshots;
};

/** Archive、领域 Adapter 与 Schema 注册表；生命周期等同于整个 WorldStorage。 */
struct FWorldStorageCoreState final
{
	TSharedPtr<FWorldStorageArchive, ESPMode::ThreadSafe> Archive;
	TSharedRef<FWorldStorageAsyncState, ESPMode::ThreadSafe> AsyncState =
		MakeShared<FWorldStorageAsyncState, ESPMode::ThreadSafe>();
	FWorldStorageManifestInfo ManifestInfo;
	TMap<EWorldEntityDomain, TSharedRef<IWorldStorageDomainAdapter>> Adapters;
	TMap<FWorldFragmentPersistenceKey, EWorldFragmentPersistence> FragmentPolicies;
	FString EphemeralAutomationArchiveRoot;
	bool bAuthority = false;
};

/** Resident Directory 与所有尚未封入 Checkpoint 的 COW 变更。 */
struct FWorldStorageEntityState final
{
	TMap<FWorldEntityId, FResidentEntry> Residents;
	/** Client 拒绝晚到 Snapshot；Server 永久删除由 COW Chunk 移除持久化。 */
	TMap<FWorldEntityId, uint32> TombstoneRevisions;
	TMap<FWorldChunkCoord, TSet<FWorldEntityId>> ResidentIdsByChunk;
	TMap<FWorldEntityId, FDirtyEntityState> DirtyEntities;
	TSet<FWorldChunkCoord> DirtyChunks;
	TMap<FWorldEntityId, double> AwakePhysicsPinnedSinceSeconds;
	uint64 NextMutationSequence = 1;
	uint64 NextDelayedMutationBatchId = 1;
	uint64 ActiveDelayedMutationBatchId = 0;
	TMap<uint64, FDelayedMutationBatchState> DelayedMutationBatches;
};

/** Chunk 引用计数、观察源、异步加载与分帧注入状态。 */
struct FWorldStorageResidencyState final
{
	TMap<FWorldChunkCoord, FChunkRuntime> Chunks;
	TArray<FResidencySourceSlot> Sources;
	TArray<int32> FreeSourceSlots;
	TSet<FWorldChunkCoord> LoadsInFlight;
	TMap<FNetworkChunkLoadKey, FNetworkChunkLoadWaiters> NetworkLoadWaiters;
	TArray<TUniquePtr<FPendingChunkInjection>> PendingInjection;
	double PollAccumulator = 0.0;
	bool bEvictionRequested = false;
};

/** 当前 Revision Snapshot 的去重缓存与异步准备窗口。 */
struct FWorldStorageSnapshotState final
{
	TMap<FWorldChunkCoord, FWorldCompressedChunk> CurrentCache;
	TSet<FWorldChunkCoord> PreparationsInFlight;
	TMap<FWorldChunkCoord, TArray<FWorldCompressedChunkReady>> Waiters;
};

/** Checkpoint 冻结序列和异步提交生命周期。 */
struct FWorldStorageCheckpointState final
{
	TFuture<void> Future;
	uint64 LastCompletedMutationSequence = 0;
	double AutosaveAccumulator = 0.0;
	bool bInFlight = false;
};

/** 只用于预算、诊断与 HUD 的统计，不参与持久化正确性。 */
struct FWorldStorageMetrics final
{
	double AuthorityTickAccumulator = 0.0;
	double LastInjectionMilliseconds = 0.0;
	double LastAuthorityStepMilliseconds = 0.0;
	double LastAwakePhysicsDiagnosticSeconds = -DBL_MAX;
	uint64 CacheHitCount = 0;
	uint64 CacheMissCount = 0;
	uint64 BytesReceived = 0;
	uint64 BytesSent = 0;
};

/** 薄协调对象；每组可变状态由对应生命周期子对象持有。 */
class FWorldStorageRuntime final
{
public:
	FResidencySourceSlot* FindSource(const FWorldResidencySourceHandle Handle)
	{
		return Handle.IsSet() && Residency.Sources.IsValidIndex(Handle.Slot) && Residency.Sources[Handle.Slot].bAlive &&
					   Residency.Sources[Handle.Slot].Generation == Handle.Generation
				   ? &Residency.Sources[Handle.Slot]
				   : nullptr;
	}

	const FResidencySourceSlot* FindSource(const FWorldResidencySourceHandle Handle) const
	{
		return const_cast<FWorldStorageRuntime*>(this)->FindSource(Handle);
	}

	void IncrementChunkRevision(const FWorldChunkCoord& Coord)
	{
		FChunkRuntime& Chunk = Residency.Chunks.FindOrAdd(Coord);
		Chunk.Revision = Chunk.Revision == MAX_uint32 ? 1 : Chunk.Revision + 1;
		Entities.DirtyChunks.Add(Coord);
		Snapshots.CurrentCache.Remove(Coord);
	}

	void ReferenceNewResidentChunk(const FWorldChunkCoord& Coord)
	{
		FChunkRuntime& Chunk = Residency.Chunks.FindOrAdd(Coord);
		for (FResidencySourceSlot& Source : Residency.Sources)
		{
			if (!Source.bAlive)
				continue;
			if (FWorldChunkBox::Centered(Source.Center, UWorldStorageSubsystem::LoadEdgeChunks).Contains(Coord) &&
				!Source.LoadChunks.Contains(Coord))
			{
				Source.LoadChunks.Add(Coord);
				++Chunk.LoadRefCount;
			}
			if (FWorldChunkBox::Centered(Source.Center, UWorldStorageSubsystem::RetentionEdgeChunks).Contains(Coord) &&
				!Source.RetentionChunks.Contains(Coord))
			{
				Source.RetentionChunks.Add(Coord);
				++Chunk.RetentionRefCount;
			}
		}
	}

	FWorldStorageCoreState Core;
	FWorldStorageEntityState Entities;
	FWorldStorageResidencyState Residency;
	FWorldStorageSnapshotState Snapshots;
	FWorldStorageCheckpointState Checkpoint;
	FWorldStorageMetrics Metrics;
};
