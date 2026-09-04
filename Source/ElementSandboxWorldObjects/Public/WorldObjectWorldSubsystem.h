#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectTypes.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectLifecycle.h"
#include "WorldObjectRuntimeStats.h"

#include "WorldObjectWorldSubsystem.generated.h"

class AWorldObjectPhysicsProxyActor;
class FWorldObjectEntityRegistry;
class FWorldObjectWorldStorageAdapter;
class FWorldObjectWorldRuntime;
class IWorldObjectPersistenceExtension;
class UWorldObjectDefinition;
class UWorldObjectProxyComponent;
struct FWorldChunkCoord;
struct FWorldPersistentEntityRecord;

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FWorldObjectEntityPreDestroyEvent,
	FWorldObjectEntityHandle,
	bool& /* bCanDestroy */);
/** 每个 Game/PIE World 唯一的独立场景物件 ECS。 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTS_API UWorldObjectWorldSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UWorldObjectWorldSubsystem();
	virtual ~UWorldObjectWorldSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void Deinitialize() override;

	/** 跨 Subsystem 清理时用于拒绝访问已经释放的 WorldObject Runtime。 */
	bool HasRuntimeState() const
	{
		return Runtime.IsValid();
	}

	FWorldObjectEntityHandle CreateEntity(const FWorldObjectCreateDesc& Desc);
	/**
	 * 为跨域破坏事务完整创建 WorldObject，但暂不发布 Lifecycle/Query Upsert，也不标记持久 Dirty。
	 * 当前仅接受非 Attached 的世界所有对象；准备成功后必须 Commit 或 Rollback。
	 */
	bool StageCreateEntities(
		TConstArrayView<FWorldObjectCreateDesc> Descs,
		FWorldObjectStagedCreateBatch& OutBatch);
	bool CommitStagedCreateEntities(
		FWorldObjectStagedCreateBatch& Batch,
		TArray<FWorldObjectEntityHandle>& OutEntities);
	void RollbackStagedCreateEntities(FWorldObjectStagedCreateBatch& Batch);
	bool RegisterDefinition(UWorldObjectDefinition& Definition);
	UWorldObjectDefinition* FindDefinition(FName DefinitionId) const;
	bool RegisterPersistenceExtension(TSharedRef<IWorldObjectPersistenceExtension> Extension);
	bool UnregisterPersistenceExtension(
		FName SectionId,
		const IWorldObjectPersistenceExtension& Extension);
	bool DestroyEntity(FWorldObjectEntityHandle Entity);
	/** 与 Building 对齐：批量破坏期间逐目标保持语义，只合并 Query Snapshot 发布。 */
	bool BeginGameplayDestructionBatch();
	bool EndGameplayDestructionBatch(bool bCommit = true);
	bool SetMotionState(
		FWorldObjectEntityHandle Entity,
		EWorldObjectMotionState NewState);
	/** Dormant Portable 的显式物理激活；自动代理按需重建，自定义代理按需重启碰撞。 */
	bool ActivatePhysics(
		FWorldObjectEntityHandle Entity,
		const FVector& LinearVelocity,
		const FVector& AngularVelocityDegrees = FVector::ZeroVector);
	bool CommitPortableTransform(
		FWorldObjectEntityHandle Entity,
		const FTransform& WorldTransform);
	/** 原子替换实例交互包络与纯值 Shape，并发布前后查询快照。 */
	bool CommitInstanceGeometryChange(
		FWorldObjectEntityHandle Entity,
		const FBox& InteractionLocalBounds,
		const FWorldObjectShapeDefinition& ShapeGeometry);
	/** 上层模块修改 Persistent Fragment 后的统一 Revision/Dirty 提交入口。 */
	bool CanCommitPersistentStateChange(FWorldObjectEntityHandle Entity) const;
	bool CommitPersistentStateChange(FWorldObjectEntityHandle Entity);
	/** 只推进持久状态 Revision/Dirty；不会构造 Shape Snapshot 或发布 Host Metadata。 */
	bool CommitPersistentStateOnlyChange(FWorldObjectEntityHandle Entity);
	bool IsEntityAlive(FWorldObjectEntityHandle Entity) const;
	FWorldObjectEntityHandle FindEntity(FWorldEntityId WorldEntityId) const;
	FWorldEntityId GetWorldEntityId(FWorldObjectEntityHandle Entity) const;
	UWorldObjectProxyComponent* GetProxy(FWorldObjectEntityHandle Entity) const;

	FWorldObjectEntityRegistry& GetRegistry();
	const FWorldObjectEntityRegistry& GetRegistry() const;
	void QueryOverlap(
		const FBox& Bounds,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectEntityHandle>& OutEntities) const;
	void QueryPortableOverlap(
		const FBox& Bounds,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectEntityHandle>& OutEntities) const;
	void QueryRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectSpatialRayHit>& OutHits) const;
	void QueryPortableRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectSpatialRayHit>& OutHits) const;
	FWorldObjectSpatialIndex& GetSpatialIndex();
	const FWorldObjectSpatialIndex& GetSpatialIndex() const;
	FWorldObjectRuntimeStats GetRuntimeStats() const;
	bool CopyQuerySnapshotPage(
		int32 Offset,
		int32 MaximumShapes,
		FWorldObjectQuerySnapshotPage& OutPage) const;
		bool CopyEntityShapeSnapshot(
			FWorldObjectEntityHandle Entity,
			FWorldObjectShapeInstanceSnapshot& OutSnapshot) const;
	void QueryShapeSnapshots(
		const FBox& Bounds,
		TArray<FWorldObjectShapeInstanceSnapshot>& OutShapes) const;
	FWorldObjectQuerySnapshotBatchCommittedEvent& OnQuerySnapshotBatchCommitted();
	/**
	 * 确保本 World 本帧最终 Actor/附着 Transform 已投影进 WorldObject Registry 与空间索引。
	 * 同一引擎帧不重复采样或重建；仅允许 Game Thread。
	 */
	void EnsurePostActorStateCurrent();

	FWorldObjectEntityPreDestroyEvent& OnEntityPreDestroy()
	{
		return EntityPreDestroyEvent;
	}
	/** Create/Restore/同 ID 新 Revision 更新在事务成功后只发布一次。 */
	FWorldObjectEntitiesUpsertedEvent& OnEntitiesUpserted() { return EntitiesUpsertedEvent; }
	/** RuntimeEvict 与 GameplayDestroy 分开发布，接收者不得猜测删除语义。 */
	FWorldObjectEntitiesRuntimeEvictedEvent& OnEntitiesRuntimeEvicted() { return EntitiesRuntimeEvictedEvent; }
	FWorldObjectEntitiesGameplayDestroyedEvent& OnEntitiesGameplayDestroyed() { return EntitiesGameplayDestroyedEvent; }

	/** Proxy 回调只排队，统一在 Post-Actor-Tick 的安全边界提交。 */
	void QueueProxyMotionState(
		FWorldEntityId WorldEntityId,
		EWorldObjectMotionState NewState);
	void NotifyProxyEndPlay(
		FWorldEntityId WorldEntityId,
		UWorldObjectProxyComponent& Proxy,
		bool bRequestAuthorityDestroy);
	void RegisterProxy(UWorldObjectProxyComponent& Proxy);

#if WITH_DEV_AUTOMATION_TESTS
	/** 只让 Automation 直接覆盖生产 Domain Adapter 的整批事务语义。 */
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

	FWorldObjectEntityHandle CreateEntityInternal(
		UWorldObjectDefinition& Definition,
		const FTransform& WorldTransform,
		EWorldObjectMotionState MotionState,
		FWorldEntityId WorldEntityId,
		uint32 StateRevision,
		UWorldObjectProxyComponent* Proxy,
		const TOptional<FBox>& InstanceInteractionBounds,
		const TOptional<FWorldObjectShapeDefinition>& InstanceShapeGeometry,
		uint64 InstanceShapeRevision,
		const FWorldObjectPhysicsBodyInit* PhysicsBody,
		bool bWorldStorageOwned,
		bool bPublishLifecycle = true,
			bool bPublishQuerySnapshot = true);
	bool DestroyEntityInternal(
		FWorldObjectEntityHandle Entity,
		ERemovalSemantic Semantic,
		bool bDestroyProxyActor,
		bool bPublishLifecycle = true,
			bool bPublishQuerySnapshot = true);
	bool BuildLifecycleRecord(FWorldObjectEntityHandle Entity, FWorldObjectLifecycleRecord& OutRecord) const;
	bool BuildShapeSnapshot(
		FWorldObjectEntityHandle Entity,
		FWorldObjectShapeInstanceSnapshot& OutSnapshot) const;
	bool PublishShapeTransition(
		const TOptional<FWorldObjectShapeInstanceSnapshot>& Previous,
		const TOptional<FWorldObjectShapeInstanceSnapshot>& Current,
			EWorldObjectQuerySnapshotChangeKind RetainedKind,
			EWorldObjectQuerySnapshotChangeKind RemovedKind,
		int64 EffectiveTimeMilliseconds = 0);
	bool SetMotionStateInternal(
		FWorldObjectEntityHandle Entity,
		EWorldObjectMotionState NewState,
		bool bGameplayMutation,
			bool bPublishQuerySnapshot = true);
	bool CommitTransformInternal(
		FWorldObjectEntityHandle Entity,
		const FTransform& WorldTransform,
		bool bPublishStableTransform,
			bool bPublishQuerySnapshot = true);
	bool BindProxyToEntity(
		FWorldObjectEntityHandle Entity,
		UWorldObjectProxyComponent& Proxy);
	void UnbindProxy(UWorldObjectProxyComponent& Proxy);
	void AddActorActive(FWorldObjectEntityHandle Entity);
	void RemoveActorActive(FWorldObjectEntityHandle Entity);
	/** 只在可见/持久状态已经提交后，为自动 Chaos Proxy 打开延迟释放窗口。 */
	void ScheduleAutomaticPhysicsRelease(FWorldObjectEntityHandle Entity);
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void ProcessPendingProxyEvents();
	void SyncActiveTransforms();
	TPimplPtr<FWorldObjectWorldRuntime> Runtime;
	FWorldObjectEntityPreDestroyEvent EntityPreDestroyEvent;
	FWorldObjectEntitiesUpsertedEvent EntitiesUpsertedEvent;
	FWorldObjectEntitiesRuntimeEvictedEvent EntitiesRuntimeEvictedEvent;
	FWorldObjectEntitiesGameplayDestroyedEvent EntitiesGameplayDestroyedEvent;

	friend FWorldObjectWorldStorageAdapter;
};
