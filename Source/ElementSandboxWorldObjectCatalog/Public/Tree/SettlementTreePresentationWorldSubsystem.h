#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "Tree/SettlementTreeTypes.h"
#include "WorldObjectLifecycle.h"

#include "SettlementTreePresentationWorldSubsystem.generated.h"

class AActor;
class FSettlementTreePresentationData;
struct FSettlementTreeSelectionResult;
struct FPresentationSourceHandle;
struct FPresentationViewSource;
class UStaticMesh;

/** 树木独占的方向选择、四缓冲、HISM 和预算流水线；不调用通用 MeshPool。 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API USettlementTreePresentationWorldSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	USettlementTreePresentationWorldSubsystem();
	virtual ~USettlementTreePresentationWorldSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	FSettlementTreePresentationStats GetStats() const;
	/** Meteor ClientOnly 表现复用同一份来源树 Mesh；只读，不转移生命周期。 */
	/** O(1) 确认指定 WorldEntityId 的树 HISM 实例是否仍实际存在。 */
	bool HasWorldEntityProjection(FWorldEntityId WorldEntityId) const;
	/** 性能验收用稳定门：没有选择、Apply、Grace 或 BuildTree 工作时为 true。 */
	bool IsIdle() const;
#if WITH_DEV_AUTOMATION_TESTS
	/** 只供黑盒回归测试核对 HISM Remove-Swap 的双向 Owner/Index/Transform 映射。 */
	bool ValidateRenderMappingsForAutomation(FString& OutError) const;
#endif

private:
	void HandleViewSourceUpdated(const FPresentationViewSource& View);
	void HandleViewSourceRemoved(FPresentationSourceHandle Source);
	void HandleCellsPublished(TConstArrayView<FSettlementTreeCellChange> Changes);
	void DispatchPendingSelections(double NowSeconds);
	void AcceptSelectionResult(TUniquePtr<FSettlementTreeSelectionResult>&& Result);
	void QueueRemovedTrees(TConstArrayView<FWorldObjectLifecycleRecord> Records);
	void ApplyPendingChanges(double NowSeconds);
	void ProcessDeferredTreeBuilds(double NowSeconds);
	bool EnsureRenderHost();
	void ReleaseRenderHost();

	TPimplPtr<FSettlementTreePresentationData> Data;

	UPROPERTY(Transient)
	TObjectPtr<AActor> RenderHost = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> TreeMesh = nullptr;

	FDelegateHandle RuntimeEvictedHandle;
	FDelegateHandle GameplayDestroyedHandle;
	FDelegateHandle ViewSourceUpdatedHandle;
	FDelegateHandle ViewSourceRemovedHandle;
	FDelegateHandle CellsPublishedHandle;
};
