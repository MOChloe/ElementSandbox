#pragma once

#include "CoreMinimal.h"
#include "Audit/BuildRegistrationAudit.h"
#include "Collision/BuildCollisionTypes.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/WorldEntityId.h"
#include "Snapshot/BuildQuerySnapshotStream.h"
#include "Processing/BuildProcessor.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"

#include "BuildingWorldSubsystem.generated.h"

class ABuildCollisionHost;
class FBuildEntityRegistry;
class FBuildingWorldStorageAdapter;
class FBuildRenderDirtySet;
class FBuildSpatialIndex;
class FBuildingWorldRuntime;
class UBuildingDefinition;
class IBuildingPersistenceExtension;
enum class EBuildRenderStorageClass : uint8;
struct FBuildPlacementEvaluation;
struct FMeshPoolInstanceHandle;
struct FBuildPlacementSurfaceHit;
struct FBuildPresentationSelectionStats;
struct FWorldChunkCoord;
struct FWorldPersistentEntityRecord;

/**
 * Building 销毁前的同步生命周期边界。监听方只能把 bCanDestroy 从 true 改为 false；
 * 用于跨域投影先解除对宿主的引用，而不让 Building 反向依赖具体系统。
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(
	FBuildEntityPreDestroyEvent,
	FBuildEntityHandle,
	bool& /* bCanDestroy */);

/** Entity 已完成销毁后的稳定网络身份通知；此时本地 Handle 已不可再解引用。 */
DECLARE_MULTICAST_DELEGATE_OneParam(
	FBuildEntityDestroyedEvent,
	FWorldEntityId);

/**
 * Entity 已从本 World 的 Registry 移除后的本地投影通知。
 * GameplayDestroy 与 RuntimeEvict 仍由各自入口决定语义；该事件只负责清理以 Handle 为键的可丢弃表现。
 */
DECLARE_MULTICAST_DELEGATE_OneParam(
	FBuildEntityLocalRemovedEvent,
	FBuildEntityHandle);

/** 指定 Definition 的 Entity 完成核心注册后发布；观察者按 DefinitionId 稀疏创建。 */
DECLARE_MULTICAST_DELEGATE_OneParam(
	FBuildDefinitionEntityUpsertedEvent,
	FBuildEntityHandle);

/**
 * 每个 Game/PIE World 唯一的 Building Runtime 装配边界。
 *
 * Subsystem 拥有 Registry、Spatial Index、轻量 Processor Scheduler、Render Dirty Set
	 * 以及独立的 Collision/Presentation Projector。所有支持的 World 都创建 Collision Host；
	 * 非 Dedicated Server World 向通用 Presentation Subsystem 注册 Mesh Layer 与 Projector。
	 * Actor Tick 只处理 Gameplay/Collision；表现只在观察源或实例命令变脏时合并选择与提交。
 */
UCLASS()
class ELEMENTSANDBOXBUILDING_API UBuildingWorldSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UBuildingWorldSubsystem();
	virtual ~UBuildingWorldSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

	/** 跨 Subsystem 清理时用于拒绝访问已经释放的 Building Runtime。 */
	bool HasRuntimeState() const
	{
		return Runtime.IsValid();
	}

	FBuildEntityRegistry& GetRegistry();
	const FBuildEntityRegistry& GetRegistry() const;

	FBuildSpatialIndex& GetSpatialIndex();
	const FBuildSpatialIndex& GetSpatialIndex() const;

	/** 注册一个可由网络记录解析的共享 Definition；相同 ID 的不同对象会被拒绝。 */
	bool RegisterDefinition(UBuildingDefinition& Definition);
	UBuildingDefinition* FindDefinition(FName DefinitionId) const;
	bool RegisterPersistenceExtension(TSharedRef<IBuildingPersistenceExtension> Extension);
	bool UnregisterPersistenceExtension(FName SectionId, const IBuildingPersistenceExtension& Extension);

	bool IsEntityAlive(FBuildEntityHandle Entity) const;
	FBuildEntityHandle FindEntity(FWorldEntityId WorldEntityId) const;
	FWorldEntityId GetWorldEntityId(FBuildEntityHandle Entity) const;
	/** 只读查询该实体是否仍拥有已提交的 Building 表现实例；可在 ECS GameplayDestroy 后查询旧 Handle。 */
	bool IsEntityPresentationResident(FBuildEntityHandle Entity) const;

	/** 分页复制当前全部中性 Shape；Cursor 与页面来自同一个已提交事务。 */
	bool CopyQuerySnapshotPage(
		int32 Offset,
		int32 MaximumShapes,
		FBuildQuerySnapshotPage& OutPage) const;
		/** 只编译一个实体；不扫描 Shard 或其他 Entity。 */
	bool CopyEntityShapeSnapshots(
		FBuildEntityHandle Entity,
		TArray<FBuildShapeInstanceSnapshot>& OutShapes) const;
		/** 用宿主空间索引返回真正与 Bounds 相交的 Part Shape。 */
	void QueryShapeSnapshots(
		const FBox& Bounds,
		TArray<FBuildShapeInstanceSnapshot>& OutShapes) const;
	FBuildQuerySnapshotBatchCommittedEvent& OnQuerySnapshotBatchCommitted();

		/** 从一个真实 Render Part 追踪其宿主 Shape 注册结果。 */
		FBuildRenderPartShapeAudit AuditRenderPartShape(
			FBuildEntityHandle Entity,
			int32 MeshPartId) const;
		/** 从一个真实 Render Part 追踪 Collision 配置、Required、预算与 Host Body。 */
		FBuildRenderPartCollisionAudit AuditRenderPartCollision(
			FBuildEntityHandle Entity,
			int32 MeshPartId) const;

	/** 客户端预览和服务器裁决共用的 Building/世界 Simple Collision 判定。 */
	bool EvaluatePlacement(
		const UBuildingDefinition& Definition,
		const FTransform& CandidateTransform,
		const FVector& BuilderLocation,
		double MaximumDistance,
		FBuildPlacementEvaluation& OutEvaluation,
		double PenetrationTolerance = 0.5) const;
	/**
	 * 合并 Building ECS Simple Collision 与 UE World 的最近射线命中。
	 * Collision Host 是可丢弃本地投影，始终从 World 查询中排除。
	 */
	bool QueryPlacementSurface(
		const FVector& Start,
		const FVector& End,
		const FCollisionQueryParams& WorldQueryParams,
		FBuildPlacementSurfaceHit& OutHit) const;
	ABuildCollisionHost* GetCollisionHost() const;

	/** 把轻量 Processor 所有权移交给本 World 的 Ready Scheduler。 */
	FBuildProcessorRegistrationHandle RegisterProcessor(
		TUniquePtr<FBuildProcessor> Processor);
	bool UnregisterProcessor(FBuildProcessorRegistrationHandle Registration);
	bool TryGetProcessorStats(
		FBuildProcessorRegistrationHandle Registration,
		FBuildProcessorStats& OutStats) const;

	/** Catalog 等上层模块修改 Persistent Fragment 后的统一 Revision/Dirty 提交入口。 */
	bool CanCommitPersistentStateChange(FBuildEntityHandle Entity) const;
	bool CommitPersistentStateChange(FBuildEntityHandle Entity);
	/** 只推进持久状态 Revision/Dirty；不会重新编译 Shape 或发布 Host Metadata。 */
	bool CommitPersistentStateOnlyChange(FBuildEntityHandle Entity);

	/**
	 * 原子创建可表现 Building：写 Registry、由共享 Mesh Part 计算 Bounds、注册空间索引，
	 * 并提交一次 Render Rebuild。任一步失败都会回滚此前写入。
	 */
	FBuildEntityHandle CreateEntity(
			const UBuildingDefinition& Definition,
			const FTransform& InitialWorldTransform,
			EBuildSpatialMobility Mobility = EBuildSpatialMobility::Static);

	/** 从空间索引与 Registry 销毁 Entity，并提交表现删除。 */
	bool DestroyEntity(FBuildEntityHandle Entity);
	/**
	 * 为同一 GameThread 提交屏障内的大批销毁合并 Query Snapshot 发布。
	 * Begin/End 之间仍逐目标执行完整销毁契约；仅把昂贵的快照复制压缩为一次提交。
	 */
	bool BeginGameplayDestructionBatch();
	bool EndGameplayDestructionBatch(bool bCommit = true);

	FBuildEntityPreDestroyEvent& OnEntityPreDestroy()
	{
		return EntityPreDestroyEvent;
	}

	FBuildEntityDestroyedEvent& OnEntityDestroyed()
	{
		return EntityDestroyedEvent;
	}

	FBuildEntityLocalRemovedEvent& OnEntityLocalRemoved()
	{
		return EntityLocalRemovedEvent;
	}

	/** 只为明确请求的 Definition 建立回调，不为所有 Entity 建全局注册流。 */
	FBuildDefinitionEntityUpsertedEvent& OnDefinitionEntityUpserted(FName DefinitionId);

	/**
	 * Entity World Transform 修改完成后，更新空间边界并刷新全部 Render Part。
	 */
	bool CommitEntityTransformChange(FBuildEntityHandle Entity);

	/**
	 * 仅提交指定 PartId。是否需要同步重算空间边界由 Definition 的契约决定。
	 */
	bool CommitPartTransformChange(
		FBuildEntityHandle Entity,
		TConstArrayView<int32> PartIds);

	/** 只刷新 PerInstance Custom Data；不更新空间、碰撞或 Part Transform。 */
	bool CommitRenderCustomDataChange(FBuildEntityHandle Entity);

	/** 注册局部 Chaos Collision Source；Handle 不能跨 World 使用。 */
	FBuildCollisionSourceHandle RegisterCollisionSource(
			const FBuildCollisionSource& SourceData);
	bool UpdateCollisionSource(
			FBuildCollisionSourceHandle Source,
			const FBuildCollisionSource& SourceData);
	bool UnregisterCollisionSource(FBuildCollisionSourceHandle Source);

	int32 GetActiveCollisionBodyCount() const;
	int32 GetCollisionSourceCount() const;
	int32 GetActiveCollisionEntityCount() const;
	int32 GetRequiredCollisionPartCount() const;
	int32 GetCachedOnlyCollisionPartCount() const;
	int32 GetPendingCollisionPrefetchAddCount() const;
	int32 GetCollisionEvictionCandidateCount() const;
	int32 GetLastQueriedCollisionEntityCount() const;
	int32 GetLastInspectedCollisionPartCount() const;
	int32 GetLastChangedCollisionPartCount() const;
	FBuildCollisionActivationConfig GetCollisionActivationConfig() const;
	bool HasPendingCollisionWork() const;
	bool TryGetPartCollisionInstance(
		FBuildEntityHandle Entity,
		int32 CollisionPartId,
		FBuildCollisionInstanceHandle& OutInstance) const;

	/** 运动钉住会让可晋升 Part 保持 Hot ISM，直到显式清除。 */
	bool SetPresentationMotionActive(FBuildEntityHandle Entity, bool bActive);
	bool IsPresentationMotionActive(FBuildEntityHandle Entity) const;
	bool TryGetPartRenderStorageClass(
		FBuildEntityHandle Entity,
		int32 PartId,
		EBuildRenderStorageClass& OutStorageClass) const;

	/** 核心 ECS/空间/表现映射与 Host CPU Instance 数据的估算，不含 Mesh 与 RHI。 */
	SIZE_T GetEstimatedBuildingCPUAllocatedSize() const;
	int32 GetRenderedBuildingCount() const;
	int32 GetRenderedInstanceCount() const;
	int32 GetRenderClusterCount() const;
	uint64 GetHierarchicalTreeBuildRequestCount() const;
	FBuildPresentationSelectionStats GetPresentationSelectionStats() const;
	double GetStaticRenderCellSize() const;
	double GetLastRenderFlushMilliseconds() const;

	/** 测试/基准同步入口：立即执行一次 Source→Projector→MeshPool 完整周期。 */
	bool FlushRenderChanges();
	bool FlushCollisionChanges();
	/** 自动化测试可注入时间，验证 Retention Grace 而不依赖真实帧等待。 */
	bool FlushCollisionChanges(double CurrentTimeSeconds);

	#if WITH_DEV_AUTOMATION_TESTS
	/** 只让 Automation 直接覆盖生产 Domain Adapter 的整批持久化事务。 */
	bool CapturePersistentBatchForTesting(
		TConstArrayView<FWorldEntityId> EntityIds,
		TArray<FWorldPersistentEntityRecord>& OutRecords,
		FString& OutError) const;
	bool RestorePersistentBatchForTesting(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldPersistentEntityRecord> Records,
		FString& OutError);
	bool RuntimeEvictPersistentBatchForTesting(
		const FWorldChunkCoord& HomeChunk,
		TConstArrayView<FWorldEntityId> EntityIds,
		FString& OutError);
	#endif

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	enum class ERemovalSemantic : uint8
	{
		GameplayDestroy,
		RuntimeEvict,
		LeaveInterest,
		FailedRegistrationRollback
	};

		FBuildEntityHandle CreateEntityInternal(
			UBuildingDefinition& Definition,
			const FTransform& InitialWorldTransform,
			EBuildSpatialMobility Mobility,
			FWorldEntityId WorldEntityId,
			uint32 StateRevision,
			bool bGameplayMutation);
	bool DestroyEntityInternal(FBuildEntityHandle Entity, ERemovalSemantic Semantic);
	/** 已完成全批只读预检后，只提交不可失败的本地 Registry/空间/表现生命周期变更。 */
	bool CommitPrevalidatedEntityRemoval(
		FBuildEntityHandle Entity,
		FWorldEntityId WorldEntityId,
		ERemovalSemantic Semantic,
		bool bPresentationProjectionAlreadyRequested);
	bool CommitEntityTransformChangeInternal(
		FBuildEntityHandle Entity,
		bool bGameplayMutation);
	bool RegisterPresentationProjector();
	void ReleasePresentationProjector();
	bool RequestPresentationProjection();
	bool ResolveRenderChanges();
	void HandlePresentationProjection(const struct FPresentationViewSnapshot& Views);
	void HandleMeshPoolInstanceRetired(FMeshPoolInstanceHandle Instance);
	bool CreateCollisionHost();
	void ReleaseCollisionHost();
	void HandleWorldPostActorTick(
		UWorld* World,
		ELevelTick TickType,
		float DeltaSeconds);

	TPimplPtr<FBuildingWorldRuntime> Runtime;
	FBuildEntityPreDestroyEvent EntityPreDestroyEvent;
	FBuildEntityDestroyedEvent EntityDestroyedEvent;
	FBuildEntityLocalRemovedEvent EntityLocalRemovedEvent;

	UPROPERTY(Transient)
	TObjectPtr<ABuildCollisionHost> CollisionHost = nullptr;

	friend FBuildingWorldStorageAdapter;
};
