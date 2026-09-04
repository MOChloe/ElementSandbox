#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "Tree/SettlementTreeTypes.h"

#include "SettlementTreeCollisionWorldSubsystem.generated.h"

class AActor;
class FSettlementTreeCollisionData;
class UInstancedStaticMeshComponent;

/** 树干近场碰撞的独立 Source、预测、预算与隐藏 ISM 所有者。 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API USettlementTreeCollisionWorldSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	USettlementTreeCollisionWorldSubsystem();
	virtual ~USettlementTreeCollisionWorldSubsystem() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	FSettlementTreeCollisionSourceHandle RegisterSource(const FSettlementTreeCollisionSource& Source);
	bool UpdateSource(FSettlementTreeCollisionSourceHandle Handle, const FSettlementTreeCollisionSource& Source);
	bool UnregisterSource(FSettlementTreeCollisionSourceHandle Handle);
	/** Source 更新后在 CharacterMovement 前同步建立 4m 与相机走廊的阻挡。 */
	void FlushImmediateCollisionChanges();
	FSettlementTreeCollisionStats GetStats() const;
	/** 没有 Dirty Source、碰撞队列或 Grace 到期任务时为 true。 */
	bool IsIdle() const;

private:
	void HandleCellsPublished(TConstArrayView<FSettlementTreeCellChange> Changes);
	void RefreshDirtySources();
	void ApplyAdds(int32 Budget, bool bImmediateOnly);
	void ApplyRemoves(int32 Budget);
	void ExpireGrace(double NowSeconds);
	bool EnsureCollisionHost();
	void ReleaseCollisionHost();

	TPimplPtr<FSettlementTreeCollisionData> Data;

	UPROPERTY(Transient)
	TObjectPtr<AActor> CollisionHost = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> CollisionInstances = nullptr;

	FDelegateHandle CellsPublishedHandle;
};
