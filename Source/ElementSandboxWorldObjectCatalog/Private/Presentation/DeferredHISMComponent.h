#pragma once

#include "Components/HierarchicalInstancedStaticMeshComponent.h"

#include "DeferredHISMComponent.generated.h"

/**
 * 批量实例编辑期间只记录层级树失效；静默窗口到达后再合并启动一次异步 BuildTree。
 * 该组件只管理 HISM 的不可抢占 Cluster Tree 生命周期，不拥有任何领域 Entity。
 */
UCLASS(Transient, NotBlueprintable)
class UDeferredHISMComponent final : public UHierarchicalInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	void BeginBulkEdit();
	void EndBulkEdit(double CurrentTimeSeconds, bool bWasEdited);
	/**
	 * UE 5.6 的原生接口不会在布局变化时通知正在构建的 HISM 快照。
	 * 本组件的所有布局变化必须经过这里，避免旧快照在新布局之后覆盖实例缓冲。
	 */
	void SetNumCustomDataFloats(int32 InNumCustomDataFloats);
	bool SetCustomDataRange(
		int32 InstanceIndexStart,
		int32 InstanceIndexEnd,
		TConstArrayView<float> CustomDataFloats);
	/** 在首批实例到达前建立空基线，避免第一棵大 Cluster Tree 在 GameThread 同步构建。 */
	bool PrewarmEmptyTree();
	bool TryStartDeferredTreeBuild(
		double CurrentTimeSeconds,
		double QuietSeconds,
		double MaxDeferralSeconds,
		bool bForce = false);
	bool HasPendingTreeBuild() const { return bBuildPending; }
	uint64 GetTreeBuildCount() const { return TreeBuildCount; }
	uint64 GetCoalescedTreeBuildCount() const { return CoalescedTreeBuildCount; }
	void NotifyAsyncBuildObservedComplete();

protected:
	virtual void BuildTree() override;
	virtual void BuildTreeAsync() override;

private:
	void RecordDeferredBuild(double CurrentTimeSeconds, uint64 RequestCount = 1);

	int32 BulkEditDepth = 0;
	uint64 BulkDeferredRequests = 0;
	bool bBuildPending = false;
	bool bAllowBuild = false;
	bool bManagedBuildInFlight = false;
	double FirstDeferredSeconds = 0.0;
	double LastDeferredSeconds = 0.0;
	uint64 TreeBuildCount = 0;
	uint64 CoalescedTreeBuildCount = 0;
};
