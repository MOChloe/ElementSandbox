#pragma once
#include "CoreMinimal.h"
#include "WorldObjects/WoodProductFlight.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "WoodProductPresentationWorldSubsystem.generated.h"

class FWoodProductPresentationRuntime;
class UWoodProductFlightMaterialSet;
class UHierarchicalInstancedStaticMeshComponent;
struct FWorldObjectQuerySnapshotBatch;

/** 木块唯一显示所有者：飞行与普通 WorldObject 共用 HISM 实例，碰撞仍由近场物件系统负责。 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API UWoodProductPresentationWorldSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	UWoodProductPresentationWorldSubsystem();
	virtual ~UWoodProductPresentationWorldSubsystem() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	/** GameThread 批量接收生命周期变更；可先于 ECS/碰撞创建，不能触发 Gameplay。 */
	void QueueFlightChanges(TConstArrayView<FWoodProductFlight> Changes);
	void RetireFlightBurst(uint64 BurstId);
	bool FindInstance(FWorldEntityId Id, UHierarchicalInstancedStaticMeshComponent*& Component, int32& Index) const;
	uint64 GetTransformUpdateCount() const;
	uint64 GetInstanceAddCount() const;
	uint64 GetInstanceRemoveCount() const;
	int32 GetInstanceCount() const;
protected:
	virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
private:
	/** 异步材质就绪后在 GameThread 创建显示宿主；此前的生命周期事件保留在 Pending。 */
	void HandleMaterialsLoaded();
	void HandleSnapshotBatch(const FWorldObjectQuerySnapshotBatch& Batch);
	void PopulateInitialSnapshot();
	void RefreshRetention();
	UPROPERTY(Transient) TObjectPtr<AActor> RenderHost;
	UPROPERTY(Transient) TObjectPtr<UWoodProductFlightMaterialSet> Materials;
	TPimplPtr<FWoodProductPresentationRuntime> Runtime;
	FDelegateHandle SnapshotBatchHandle;
	FDelegateHandle ResidencyHandle;
};
