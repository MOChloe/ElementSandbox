#pragma once

#include "CoreMinimal.h"
#include "MeshPoolTypes.h"
#include "PresentationViewSource.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"

#include "PresentationWorldSubsystem.generated.h"

class AMeshPoolRenderHost;
class FPresentationWorldData;
class UInstancedStaticMeshComponent;

DECLARE_DELEGATE_OneParam(FPresentationProjectorDelegate, const FPresentationViewSnapshot&);
DECLARE_MULTICAST_DELEGATE_OneParam(FPresentationViewSourceUpdatedDelegate, const FPresentationViewSource&);
DECLARE_MULTICAST_DELEGATE_OneParam(FPresentationViewSourceRemovedDelegate, FPresentationSourceHandle);
/** 物理 MeshPool 实例已从 Render Host 退出；参数仍是退休前的稳定 Generation Handle。 */
DECLARE_MULTICAST_DELEGATE_OneParam(FMeshPoolInstanceRetiredDelegate, FMeshPoolInstanceHandle);

struct ELEMENTSANDBOXPRESENTATION_API FPresentationProjectorHandle final
{
public:
	FPresentationProjectorHandle() = default;

	bool IsSet() const { return WorldId != 0 && Index != INDEX_NONE && Generation != 0; }
	int32 GetIndex() const { return Index; }
	uint32 GetGeneration() const { return Generation; }
	uint32 GetWorldId() const { return WorldId; }

private:
	FPresentationProjectorHandle(uint32 InWorldId, int32 InIndex, uint32 InGeneration)
		: WorldId(InWorldId), Index(InIndex), Generation(InGeneration)
	{
	}
	uint32 WorldId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;
	friend UPresentationWorldSubsystem;
};

/** 每 World 的观察源、Projector 调度与跨帧 MeshPool 唯一所有者。 */
UCLASS()
class ELEMENTSANDBOXPRESENTATION_API UPresentationWorldSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UPresentationWorldSubsystem();
	virtual ~UPresentationWorldSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	FPresentationSourceHandle RegisterSource(const FPresentationViewSource& Source);
	bool UpdateSource(FPresentationSourceHandle Source, const FPresentationViewSource& View);
	bool UnregisterSource(FPresentationSourceHandle Source);
	int32 GetSourceCount() const;
	/** 复制当前已注册观察源的不可变快照；不运行 Projector 或提交 MeshPool。 */
	bool CopyCurrentViewSnapshot(FPresentationViewSnapshot& OutSnapshot) const;
	/** 注册与更新均在源已经规范化并写入 Registry 后同步广播。 */
	FPresentationViewSourceUpdatedDelegate& OnViewSourceUpdated() { return ViewSourceUpdatedDelegate; }
	/** 在句柄 Generation 失效前同步广播，消费者只能把它当身份值使用。 */
	FPresentationViewSourceRemovedDelegate& OnViewSourceRemoved() { return ViewSourceRemovedDelegate; }

	FPresentationProjectorHandle RegisterProjector(FName Name, FPresentationProjectorDelegate Delegate);
	bool UnregisterProjector(FPresentationProjectorHandle Projector);
	/**
	 * 领域数据变化但观察源未变化时，显式唤醒下一次合并 Projector 周期。
	 * 重复请求只合并为一个 Dirty 标记，不会同步执行或逐实体 Flush。
	 */
	bool RequestProjectionCycle();

	FMeshPoolLayerHandle RegisterMeshLayer(FName Name);
	bool UnregisterMeshLayer(FMeshPoolLayerHandle Layer);
	bool ConsumeLayerReprojectionRequest(FMeshPoolLayerHandle Layer);

	FMeshPoolInstanceHandle QueueAdd(
		const FMeshPoolClusterKey& Cluster,
		const FTransform& WorldTransform,
		TConstArrayView<float> CustomData = {});
	bool QueueUpdate(
		FMeshPoolInstanceHandle Instance,
		const FTransform& WorldTransform,
		TConstArrayView<float> CustomData = {});
	bool QueueCustomData(FMeshPoolInstanceHandle Instance, TConstArrayView<float> CustomData);
	bool QueueMigrate(
		FMeshPoolInstanceHandle Instance,
		const FMeshPoolClusterKey& TargetCluster,
		const FTransform& WorldTransform,
		TConstArrayView<float> CustomData = {});
	bool QueueRemove(FMeshPoolInstanceHandle Instance);
	/** 提升当前待提交命令，下一次普通 Tick 先于背景装填处理；仍遵守帧预算，不同步 Flush。 */
	bool PrioritizePendingInstance(FMeshPoolInstanceHandle Instance);
	bool IsValidInstance(FMeshPoolInstanceHandle Instance) const;
	/** 逻辑删除后到物理 Flush 完成前仍返回 true，用于严格的源投影因果门。 */
	bool IsInstancePhysicallyResident(FMeshPoolInstanceHandle Instance) const;
	FMeshPoolInstanceRetiredDelegate& OnInstanceRetired() { return InstanceRetiredDelegate; }
	bool TryGetInstanceTransform(FMeshPoolInstanceHandle Instance, FTransform& OutWorldTransform) const;

	/** 只提交当前 Pending 命令，不额外调用 Projector。 */
	bool FlushNow();
	/** 立即执行一次“固化 Source -> Projector -> MeshPool Flush”完整周期。 */
	bool RunCycleNow();
	FMeshPoolStats GetMeshPoolStats() const;
	UInstancedStaticMeshComponent* GetClusterComponent(const FMeshPoolClusterKey& Cluster) const;
	AMeshPoolRenderHost* GetRenderHost() const { return RenderHost; }

private:
	bool RunScheduledCycle(bool bForceTreeBuild = false);
	bool FlushSlots(TConstArrayView<int32> RestrictToSlots = {}, bool bForceTreeBuild = false);
	bool FlushInstanceBatch(TConstArrayView<int32> SlotIndices, TSet<FMeshPoolClusterKey>& FrameTouchedClusters);
	void RecordMeshPoolStats() const;
	bool RecoverFailedLayers(const TSet<FMeshPoolLayerHandle>& Layers);
	bool EnsureRenderHost();
	void ReleaseInstanceSlot(int32 SlotIndex);

	TPimplPtr<FPresentationWorldData> Data;
	FPresentationViewSourceUpdatedDelegate ViewSourceUpdatedDelegate;
	FPresentationViewSourceRemovedDelegate ViewSourceRemovedDelegate;
	FMeshPoolInstanceRetiredDelegate InstanceRetiredDelegate;

	UPROPERTY(Transient)
	TObjectPtr<AMeshPoolRenderHost> RenderHost = nullptr;
};
