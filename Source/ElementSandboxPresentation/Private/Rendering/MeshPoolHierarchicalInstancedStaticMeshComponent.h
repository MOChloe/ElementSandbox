#pragma once

#include "Components/HierarchicalInstancedStaticMeshComponent.h"

#include "MeshPoolHierarchicalInstancedStaticMeshComponent.generated.h"

/**
 * MeshPool 专用 HISM：实例数据立即提交，追加实例在帧尾刷新渲染对象的未建树绘制范围；
 * Cluster Tree 独立等待静默期，不能阻塞新实例显示。
 * 外部实例编辑只登记 Dirty；已经启动的 UE 内部重试允许完成，发布树时立即结束这一轮权限。
 */
UCLASS(Transient, NotBlueprintable)
class UMeshPoolHierarchicalInstancedStaticMeshComponent final : public UHierarchicalInstancedStaticMeshComponent
{
	GENERATED_BODY()

  public:
	UMeshPoolHierarchicalInstancedStaticMeshComponent();
	/** UE 的 HISM SceneProxy 缓存追加区间；仅更新实例缓冲不足以让新实例参与绘制。 */
	virtual int32 AddInstance(const FTransform& InstanceTransform, bool bWorldSpace = false) override;
	virtual TArray<int32> AddInstances(const TArray<FTransform>& InstanceTransforms, bool bShouldReturnIndices,
		bool bWorldSpace = false, bool bUpdateNavigation = true) override;

	void BeginBulkEdit();
	void EndBulkEdit(double CurrentTimeSeconds, bool bWasEdited);
	void CancelBulkEdit();
	bool SetMeshPoolCustomDataRange(int32 InstanceIndexStart, int32 InstanceIndexEnd,
										TConstArrayView<float> CustomDataFloats);
	/** 小 Cluster 的同步树发布只覆盖交互规模；大 Cluster 仍由静默窗口异步合并。 */
	bool PublishSmallTreeImmediately(int32 MaximumInstanceCount);
	bool TryStartDeferredTreeBuild(double CurrentTimeSeconds, double QuietSeconds, double MaxDeferralSeconds,
									   bool bForce);
	bool HasDeferredTreeBuild() const
	{
		return bTreeBuildPending;
	}
	uint64 GetTreeBuildRequestCount() const
	{
		return TreeBuildRequestCount;
	}
	uint64 GetTreeBuildRetryCount() const { return TreeBuildRetryCount; }
	uint64 GetDeferredTreeBuildRequestCount() const
	{
		return DeferredTreeBuildRequestCount;
	}
	uint64 GetCoalescedTreeBuildRequestCount() const
	{
		return CoalescedTreeBuildRequestCount;
	}

  protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BuildTree() override;
	virtual void BuildTreeAsync() override;

  private:
	void RecordDeferredTreeBuild(double CurrentTimeSeconds, uint64 RequestCount = 1);
	void ClearDeferredTreeBuild();
	void HandleTreeBuilt(UHierarchicalInstancedStaticMeshComponent* Component, bool bWasAsyncBuild);
	FDelegateHandle TreeBuiltHandle;

	int32 BulkEditDepth = 0;
	uint64 BulkDeferredRequestCount = 0;
	bool bTreeBuildPending = false;
	bool bAllowTreeBuildNow = false;
	bool bManagedBuildInFlight = false;
	double FirstDeferredTimeSeconds = 0.0;
	double LastDeferredTimeSeconds = 0.0;
	uint64 TreeBuildRequestCount = 0;
	uint64 TreeBuildRetryCount = 0;
	uint64 DeferredTreeBuildRequestCount = 0;
	uint64 CoalescedTreeBuildRequestCount = 0;
};
