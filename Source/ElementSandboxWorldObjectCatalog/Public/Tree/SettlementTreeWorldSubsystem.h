#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "Tree/SettlementTreeTypes.h"
#include "WorldObjectLifecycle.h"

#include "SettlementTreeWorldSubsystem.generated.h"

class FSettlementTreeWorldData;
class USettlementTreeDefinition;
class UWorldObjectWorldSubsystem;

DECLARE_MULTICAST_DELEGATE_OneParam(
	FSettlementTreeCellsPublishedDelegate,
	TConstArrayView<FSettlementTreeCellChange>);

/**
 * Settlement.Tree 的内容注册与可丢弃 1km 紧凑索引。WorldObject ECS 仍是唯一 Gameplay 真值；
 * 本索引只服务树木表现和碰撞，不包含 HISM 或 Chaos 状态。
 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API USettlementTreeWorldSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	USettlementTreeWorldSubsystem();
	virtual ~USettlementTreeWorldSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	bool TryGetTree(FWorldObjectEntityHandle Entity, FSettlementTreeCandidate& OutTree) const;
	/**
	 * 从已提交的 Fire 权威状态刷新树材质派生值。该值不进入 WorldStorage，
	 * 恢复时先建立冷宿主投影，Element Dependent 恢复后再由 Outbox 重建烧黑值。
	 */
	bool CommitBurnAmount(FWorldObjectEntityHandle Entity, float BurnAmount);
	void CopyCellSnapshots(TArray<FSettlementTreeCellSnapshot>& OutCells, uint64& OutRevision) const;
	void QueryTrees(const FBox& Bounds, TArray<FSettlementTreeCandidate>& OutTrees) const;
	FSettlementTreeCatalogStats GetStats() const;
	FSettlementTreeCellsPublishedDelegate& OnCellsPublished() { return CellsPublishedDelegate; }
	USettlementTreeDefinition* GetDefinition() const { return Definition; }

private:
	void HandleUpserted(TConstArrayView<FWorldObjectLifecycleRecord> Records);
	void HandleRemoved(TConstArrayView<FWorldObjectLifecycleRecord> Records);

	TPimplPtr<FSettlementTreeWorldData> Data;

	UPROPERTY(Transient)
	TObjectPtr<USettlementTreeDefinition> Definition = nullptr;

	TWeakObjectPtr<UWorldObjectWorldSubsystem> WorldObjects;
	FDelegateHandle UpsertedHandle;
	FDelegateHandle RuntimeEvictedHandle;
	FDelegateHandle GameplayDestroyedHandle;
	FSettlementTreeCellsPublishedDelegate CellsPublishedDelegate;
};
