#pragma once

#include "CoreMinimal.h"
#include "Chunk/WorldChunkTypes.h"
#include "Storage/WorldStorageArchive.h"
#include "Storage/WorldStorageDomainAdapter.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"

#include "WorldStorageSubsystem.generated.h"

class FWorldStorageRuntime;
class UScriptStruct;

using FWorldCompressedChunkReady =
	TUniqueFunction<void(FWorldCompressedChunk&& Chunk, FString&& Error)>;
using FWorldNetworkChunkApplied =
	TUniqueFunction<void(bool bSuccess, const FString& Error, FWorldCompressedChunk&& Chunk)>;

DECLARE_MULTICAST_DELEGATE(FWorldResidencySourcesChangedEvent);

DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldStorageAuthorityMutationEvent,
	const FWorldStorageEntityMutation&);

DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldStorageAuthorityStepEvent,
	int64 /* WorldTimeMilliseconds */);

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageRuntimeEvictPreparation final
{
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	FWorldChunkCoord HomeChunk;
	TConstArrayView<FWorldEntityId> EntityIds;
	int64 WorldTimeMilliseconds = 0;
	FString Error;

	bool CanProceed() const { return Error.IsEmpty(); }
	void Reject(FString&& InError) { Error = MoveTemp(InError); }
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldStoragePrepareRuntimeEvictBatchEvent,
	FWorldStorageRuntimeEvictPreparation&);

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldResidencySourceHandle final
{
	GENERATED_BODY()

	UPROPERTY()
	uint32 Slot = MAX_uint32;

	UPROPERTY()
	uint32 Generation = 0;

	bool IsSet() const { return Slot != MAX_uint32 && Generation != 0; }

	friend bool operator==(const FWorldResidencySourceHandle& Left, const FWorldResidencySourceHandle& Right)
	{
		return Left.Slot == Right.Slot && Left.Generation == Right.Generation;
	}
};

USTRUCT()
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageMutationBatchHandle final
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 Value = 0;

	bool IsSet() const { return Value != 0; }
	friend bool operator==(const FWorldStorageMutationBatchHandle&, const FWorldStorageMutationBatchHandle&) = default;
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldResidentEntityRegistration final
{
	FWorldEntityId EntityId;
	EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
	FWorldChunkCoord HomeChunk;
	uint32 StateRevision = 1;
};

/** Client 网络投影删除的纯值输入；GameplayDestroy 与 LeaveInterest 保持显式语义。 */
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldNetworkEntityRemoval final
{
	FWorldEntityId EntityId;
	uint32 StateRevision = 0;
	bool bGameplayDestroy = false;

	bool IsValid() const { return EntityId.IsSet() && StateRevision != 0; }
};

enum class EWorldResidentUpsertResult : uint8
{
	Inserted,
	SameRevision,
	Updated,
	RejectedTypeCollision,
	RejectedOlderRevision,
	RejectedTombstone,
	Invalid
};

struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageRuntimeStats final
{
	int32 ResidentEntityCount = 0;
	int32 ResidentChunkCount = 0;
	int32 PendingLoadCount = 0;
	int32 PendingInjectionCount = 0;
	int32 DirtyEntityCount = 0;
	int32 ResidencySourceCount = 0;
	int32 AwakePhysicsPinnedEntityCount = 0;
	double OldestAwakePhysicsPinSeconds = 0.0;
	uint64 CacheHitCount = 0;
	uint64 CacheMissCount = 0;
	uint64 BytesReceived = 0;
	uint64 BytesSent = 0;
	int64 CompleteStructureCount = 0;
	int64 BuildingEntityCount = 0;
	int64 WorldObjectEntityCount = 0;
	int64 WorldSimulationTimeMilliseconds = 0;
	double LastInjectionMilliseconds = 0.0;
	double LastAuthorityStepMilliseconds = 0.0;
	bool bCheckpointInFlight = false;
};

/**
 * 世界存档、三维 Chunk Residency、统一永久身份和 8Hz Authority 时钟的唯一 World 边界。
 * Client 不读取服务器存档；它只接受经服务器 Offer/Hash 校验的 Chunk Blob。
 */
UCLASS()
class ELEMENTSANDBOXWORLDSTORAGE_API UWorldStorageSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UWorldStorageSubsystem();
	virtual ~UWorldStorageSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

	FWorldEntityId AllocateEntityId();
	bool RegisterDomainAdapter(TSharedRef<IWorldStorageDomainAdapter> Adapter);
	bool UnregisterDomainAdapter(EWorldEntityDomain Domain, const IWorldStorageDomainAdapter& Adapter);
#if WITH_DEV_AUTOMATION_TESTS
	/** 仅供 WorldStorage 黑盒测试隔离领域实现；运行时不得调用。 */
	bool ReplaceDomainAdapterForAutomation(TSharedRef<IWorldStorageDomainAdapter> Adapter);
#endif

	bool RegisterFragmentPersistence(
		EWorldEntityDomain Domain,
		const UScriptStruct& FragmentType,
		EWorldFragmentPersistence Persistence);
	TOptional<EWorldFragmentPersistence> FindFragmentPersistence(
		EWorldEntityDomain Domain,
		const UScriptStruct& FragmentType) const;

	EWorldResidentUpsertResult RegisterResidentEntity(const FWorldResidentEntityRegistration& Registration);
	/** 仅撤销尚未 MarkDirty/发布的创建准备；不写 Tombstone，不可用于 RuntimeEvict。 */
	bool RollbackUnpublishedResidentRegistration(
		const FWorldResidentEntityRegistration& Registration);
	bool IsResident(FWorldEntityId EntityId) const;
	bool MarkEntityDirty(FWorldEntityId EntityId, uint32 StateRevision);
	bool UpdateEntityLocation(FWorldEntityId EntityId, const FVector& NewLocation, uint32 StateRevision);
	bool GameplayDestroy(FWorldEntityId EntityId, uint32 StateRevision);

	FWorldResidencySourceHandle RegisterResidencySource(
		const FVector& Location,
		const FVector& Forward = FVector::ForwardVector);
	bool UpdateResidencySource(
		FWorldResidencySourceHandle Source,
		const FVector& Location,
		const FVector& Forward,
		bool bForceRefresh = false);
	bool UnregisterResidencySource(FWorldResidencySourceHandle Source);
	/** GameThread 的观察源范围；包括还没有 ECS 的空 Chunk，供临时表现按同一兴趣并集释放。 */
	void CopyResidencyRetentionBoxes(TArray<FWorldChunkBox>& OutBoxes) const;
	FWorldResidencySourcesChangedEvent& OnResidencySourcesChanged() { return ResidencySourcesChangedEvent; }
	/** Authority/GameThread：只为连接尚未 ACK 的 Chunk 准备 Offer/Snapshot；相关集仍包含全部兴趣坐标。 */
	void GetRelevantChunkOffers(
		FWorldResidencySourceHandle Source,
		TConstArrayView<FWorldChunkCoord> RequiredProjectionChunks,
		const TSet<FWorldChunkCoord>& AcknowledgedChunks,
		TArray<FWorldChunkOffer>& OutOffers,
		TSet<FWorldChunkCoord>& OutRelevantChunks);

	/** 未修改 Chunk 在 Worker 读取 Pack；Dirty Chunk 先在 GameThread Capture POD，再由 Worker 合并和压缩。 */
	bool RequestCurrentCompressedChunk(
		FWorldChunkCoord Coord,
		FWorldCompressedChunkReady&& Completion);
	/**
	 * Completion 只在 Chunk 已经完整 Restore 为 Client ECS 投影后成功。
	 * 相同 Coord/Revision/Hash 的在途请求共享一次解码和注入，不能因尚未完成而误报损坏。
	 */
	bool SubmitNetworkChunk(
		const FGuid& WorldId,
		FWorldCompressedChunk Chunk,
		FWorldNetworkChunkApplied&& Completion = {});
	bool ApplyNetworkUpsert(const FWorldPersistentEntityRecord& Record);
	/**
	 * Client-only Live Delta 批量入口。先完成整批身份、Revision 与 Adapter 预检，再按
	 * Domain/HomeChunk 各调用一次 RestoreBatch，避免 Settlement Upsert 退化为逐实体事务。
	 */
	bool ApplyNetworkUpsertBatch(TConstArrayView<FWorldPersistentEntityRecord> Records);
	bool ApplyNetworkRemove(
		FWorldEntityId EntityId,
		uint32 StateRevision,
		bool bGameplayDestroy);
	/** 同一网络批次按领域、HomeChunk 与销毁语义合并后一次提交生命周期和查询快照。 */
	bool ApplyNetworkRemoveBatch(TConstArrayView<FWorldNetworkEntityRemoval> Removals);
	bool CaptureResidentRecord(
		FWorldEntityId EntityId,
		FWorldPersistentEntityRecord& OutRecord,
		FString& OutError);
	void RecordClientCacheResult(bool bHit);

	bool RequestCheckpoint();
	/** 打开后异步封口既有变更；Batch Ready 前不允许写入，因而不会阻塞 Game Thread 等待磁盘。 */
	FWorldStorageMutationBatchHandle BeginDelayedMutationBatch();
	/** 既有 Checkpoint 已完成且 Batch 可以接收原子 Mutation。必要时会推进下一次异步封口。 */
	bool IsDelayedMutationBatchReady(FWorldStorageMutationBatchHandle Batch);
	bool ExecuteInDelayedMutationBatch(
		FWorldStorageMutationBatchHandle Batch,
		TFunctionRef<bool()> Mutation);
	bool CommitDelayedMutationBatch(FWorldStorageMutationBatchHandle Batch);
	/** 只允许取消尚未写入任何 Mutation 的 Batch。 */
	bool CancelEmptyDelayedMutationBatch(FWorldStorageMutationBatchHandle Batch);
	/** Authority 端 Manifest/稀疏索引已经打开；Client 端协议端点已经可接收快照。 */
	bool IsStorageReady() const;
	/**
	 * 以 WorldStorage 的进程职责为准，而不是 Actor Role。
	 * 启动外部本地服务器前的 Standalone 客户端 Actor 仍可能暂时拥有 Authority Role。
	 */
	bool IsAuthorityStorage() const;
	/** 仅表示该逻辑 Chunk 已完成全部领域 Restore，不把 Pack/解码缓存误算为 Resident。 */
	bool IsChunkResident(FWorldChunkCoord Coord) const;
	/** Activation 使用：非空 Chunk 必须 Resident；Archive 与 Dirty Overlay 都为空的 Chunk 可作为已知空基线。 */
	bool IsAuthorityChunkReadyForActivation(FWorldChunkCoord Coord) const;
	bool IsChunkDirty(FWorldChunkCoord Coord) const;
	int32 GetChunkResidentEntityCount(FWorldChunkCoord Coord) const;
	/** Authority 优先选择当前 Resident 最密集 Chunk；没有运行投影时才读取 Archive 代表 Chunk。 */
	bool TryGetMostPopulatedChunk(FWorldChunkCoord& OutChunk) const;
	FGuid GetWorldId() const;
	FWorldStorageManifestInfo GetManifestInfo() const;
	FWorldStorageRuntimeStats GetRuntimeStats() const;
	int64 GetWorldSimulationTimeMilliseconds() const;
	FWorldStorageAuthorityMutationEvent& OnAuthorityMutation()
	{
		return AuthorityMutationEvent;
	}
	/** Server Authority 每 125 ms 发布一次稳定 Gameplay/存档提交边界。 */
	FWorldStorageAuthorityStepEvent& OnAuthorityStep()
	{
		return AuthorityStepEvent;
	}
	/** RuntimeEvict 捕获存档前的同步封口点；监听者可结算状态、标脏或拒绝本批驱逐。 */
	FWorldStoragePrepareRuntimeEvictBatchEvent& OnPrepareRuntimeEvictBatch()
	{
		return PrepareRuntimeEvictBatchEvent;
	}

	static constexpr double AuthorityTickIntervalSeconds = 0.125;
	static constexpr double AutosaveIntervalSeconds = 60.0;
	static constexpr double ResidencyPollIntervalSeconds = 1.0;
	static constexpr int32 LoadEdgeChunks = 128;
	static constexpr int32 RetentionEdgeChunks = 150;
	static constexpr int32 ActivationCoreEdgeChunks = 3;
	static constexpr int32 MaximumConcurrentChunkLoads = 16;
	static constexpr int32 MaximumPendingChunkInjections = 32;
	static constexpr int32 MaximumServerRecordsPerRestoreBatch = 8;
	static constexpr int32 MaximumClientRecordsPerRestoreBatch = 8;
	/** 检查发生在一个原子 Restore slice 之后，给硬门槛预留 slice 尾部与 Tick 固定开销。 */
	static constexpr double ServerInjectionBudgetMilliseconds = 0.5;
	static constexpr double ClientInjectionBudgetMilliseconds = 2.5;

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	void RefreshResidencySources(bool bForce);
	void ScheduleRequiredChunkLoads();
	void DrainCompletedLoads(double BudgetMilliseconds);
	bool StartCurrentChunkSnapshotPreparation(FWorldChunkCoord Coord, FString& OutError);
	void DrainSnapshotPreparations();
	void EvictUnretainedChunks();
	bool StartCheckpoint(bool bSynchronous);
	void DrainCheckpointCompletion();

	TPimplPtr<FWorldStorageRuntime> Runtime;
	FWorldStorageAuthorityMutationEvent AuthorityMutationEvent;
	FWorldStorageAuthorityStepEvent AuthorityStepEvent;
	FWorldStoragePrepareRuntimeEvictBatchEvent PrepareRuntimeEvictBatchEvent;
	FWorldResidencySourcesChangedEvent ResidencySourcesChangedEvent;
};
