#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectEntityHandle.h"

#include "SettlementTreeTypes.generated.h"

inline const FName SettlementTreeDefinitionId(TEXT("Settlement.Tree"));
inline constexpr int32 SettlementTreeColorVariationCustomDataIndex = 0;
inline constexpr int32 SettlementTreeBurnAmountCustomDataIndex = 1;
inline constexpr int32 SettlementTreeCustomDataFloatCount = 2;

struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCandidate final
{
	FWorldObjectEntityHandle Entity;
	FWorldEntityId WorldEntityId;
	FTransform WorldTransform = FTransform::Identity;
	FBox WorldBounds = FBox(ForceInit);
	FIntPoint Cell = FIntPoint::ZeroValue;
	float ColorVariation = 0.5f;
	float BurnAmount = 0.0f;
};

/** 一个 100m Snapshot Shard；1km Cell 只组合这些不可变小段，不反复复制全部树。
 */
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeSnapshotShard final
{
	FIntPoint Shard = FIntPoint::ZeroValue;
	FBox AggregateBounds = FBox(ForceInit);
	TSharedPtr<const TArray<FSettlementTreeCandidate>, ESPMode::ThreadSafe> Trees;
};

/** 一个 1km Cell 的不可变 Worker 快照；生命周期批量写入与选择 Worker
 * 不共享可变数组。 */
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCellSnapshot final
{
	FIntPoint Cell = FIntPoint::ZeroValue;
	uint64 Revision = 0;
	FBox AggregateBounds = FBox(ForceInit);
	TSharedPtr<const TArray<FSettlementTreeSnapshotShard>, ESPMode::ThreadSafe> Shards;
	int32 TreeCount = 0;
};

/**
 * Catalog 一次发布中的单 Cell 最新状态。
 * Snapshot 供异步全量选择与在途 Worker 追帧；常规内容注入只消费本批
 * Upsert/Remove， 避免同一 1km Cell 随 100m Chunk
 * 逐步装填时反复扫描不断变大的整份 Snapshot。
 */
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCellChange final
{
	FIntPoint Cell = FIntPoint::ZeroValue;
	uint64 Revision = 0;
	TSharedPtr<const FSettlementTreeCellSnapshot, ESPMode::ThreadSafe> Snapshot;
	TSharedPtr<const TArray<FSettlementTreeCandidate>, ESPMode::ThreadSafe> UpsertedTrees;
	TSharedPtr<const TArray<FWorldObjectEntityHandle>, ESPMode::ThreadSafe> RemovedEntities;
};

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCatalogStats final
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 ResidentTreeCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 CellCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 Revision = 0;
	UPROPERTY(BlueprintReadOnly) int64 PublishedRevision = 0;
	UPROPERTY(BlueprintReadOnly) int64 CellPublishCount = 0;
	/** 为不可变 Worker Snapshot 实际复制过的 Candidate；按 100m Shard 有界。 */
	UPROPERTY(BlueprintReadOnly) int64 SnapshotCandidateCopyCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 SnapshotShardPublishCount = 0;
	UPROPERTY(BlueprintReadOnly) double LastPublishMilliseconds = 0.0;
};

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreePresentationStats final
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 ActiveCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 TransitionCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 GraceCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 PendingCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 HISMCellCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 InstanceCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 RenderHostCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 TreeBuildCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 CoalescedTreeBuildCount = 0;
	/** 树模块全局最多允许一个 HISM Cluster Tree 异步构建在途。 */
	UPROPERTY(BlueprintReadOnly) int32 InFlightTreeBuildCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 MaximumConcurrentTreeBuildsObserved = 0;
	UPROPERTY(BlueprintReadOnly) double LastSelectionMilliseconds = 0.0;
	UPROPERTY(BlueprintReadOnly) double LastSelectionWorkerMilliseconds = 0.0;
	UPROPERTY(BlueprintReadOnly) double LastApplyMilliseconds = 0.0;
	UPROPERTY(BlueprintReadOnly) double LastTreeBuildScheduleMilliseconds = 0.0;
	UPROPERTY(BlueprintReadOnly) double LastObservationMilliseconds = 0.0;
	UPROPERTY(BlueprintReadOnly) int64 LocalSelectionPassCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 FarSelectionPassCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 WorkerDispatchCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 WorkerDiscardCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 CandidateTestCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 CellDeltaEvaluationCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 HISMAddCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 HISMRemoveCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 HISMCustomDataUpdateCount = 0;
	/** 实际 Remove 时仍位于当前 Local 核心或可见水平视场的次数；正常运行必须为
	 * 0。 */
	UPROPERTY(BlueprintReadOnly) int64 InvalidVisibleRemovalCount = 0;
};

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCollisionStats final
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 SourceCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 CollisionInstanceCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 PendingAddCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 PendingRemoveCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 SourceSubmitCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 CatalogQueryCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 CandidateTestCount = 0;
};

struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCollisionSource final
{
	FVector SubjectLocation = FVector::ZeroVector;
	FVector ViewLocation = FVector::ZeroVector;
	FVector ViewDirection = FVector::ForwardVector;
	FVector Velocity = FVector::ZeroVector;
	FBox ImmediateBounds = FBox(ForceInit);
	FBox PrefetchBounds = FBox(ForceInit);
	FBox RetentionBounds = FBox(ForceInit);
	uint64 Revision = 0;

	bool IsValid() const
	{
		return !SubjectLocation.ContainsNaN() && !ViewLocation.ContainsNaN() && !ViewDirection.ContainsNaN() &&
			   !ViewDirection.IsNearlyZero() && !Velocity.ContainsNaN() && ImmediateBounds.IsValid != 0 &&
			   PrefetchBounds.IsValid != 0 && RetentionBounds.IsValid != 0 && Revision != 0;
	}
};

struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FSettlementTreeCollisionSourceHandle final
{
	int32 Slot = INDEX_NONE;
	uint32 Generation = 0;
	bool IsSet() const
	{
		return Slot != INDEX_NONE && Generation != 0;
	}
};

ELEMENTSANDBOXWORLDOBJECTCATALOG_API float ComputeSettlementTreeColorVariation(FWorldEntityId WorldEntityId);
