#include "Rendering/MeshPoolHierarchicalInstancedStaticMeshComponent.h"

#include "HAL/PlatformTime.h"
#include "InstancedStaticMeshDelegates.h"

UMeshPoolHierarchicalInstancedStaticMeshComponent::UMeshPoolHierarchicalInstancedStaticMeshComponent()
{
	// Add/Remove 的自动建树入口不认识 MeshPool 的帧预算；统一由批尾登记、Host 调度启动。
	bAutoRebuildTreeOnInstanceChanges = false;
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::OnRegister()
{
	TreeBuiltHandle = FHierarchicalInstancedStaticMeshDelegates::OnTreeBuilt.AddUObject(
		this, &UMeshPoolHierarchicalInstancedStaticMeshComponent::HandleTreeBuilt);
	Super::OnRegister();
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::OnUnregister()
{
	FHierarchicalInstancedStaticMeshDelegates::OnTreeBuilt.Remove(TreeBuiltHandle);
	TreeBuiltHandle.Reset();
	Super::OnUnregister();
}

int32 UMeshPoolHierarchicalInstancedStaticMeshComponent::AddInstance(
	const FTransform& InstanceTransform, const bool bWorldSpace)
{
	const int32 InstanceIndex = Super::AddInstance(InstanceTransform, bWorldSpace);
	if (InstanceIndex != INDEX_NONE)
	{
		MarkRenderStateDirty();
	}
	return InstanceIndex;
}

TArray<int32> UMeshPoolHierarchicalInstancedStaticMeshComponent::AddInstances(
	const TArray<FTransform>& InstanceTransforms, const bool bShouldReturnIndices,
	const bool bWorldSpace, const bool bUpdateNavigation)
{
	const int32 PreviousInstanceCount = GetInstanceCount();
	TArray<int32> InstanceIndices = Super::AddInstances(
		InstanceTransforms, bShouldReturnIndices, bWorldSpace, bUpdateNavigation);
	if (GetInstanceCount() != PreviousInstanceCount)
	{
		// HISM SceneProxy 在创建时复制 InstanceCountToRender 与 UnbuiltBounds。大 Cluster
		// 延迟建树时，MarkRenderInstancesDirty 只上传缓冲，旧 Proxy 不会绘制追加区间。
		// UE 把同帧多次 Dirty 合并到一次帧尾刷新；旧树和未建树实例可一起绘制，无需同步建树。
		MarkRenderStateDirty();
	}
	return InstanceIndices;
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::BeginBulkEdit()
{
	check(IsInGameThread());
	++BulkEditDepth;
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::EndBulkEdit(const double CurrentTimeSeconds,
																	const bool bWasEdited)
{
	check(IsInGameThread());
	check(BulkEditDepth > 0);
	--BulkEditDepth;
	if (BulkEditDepth == 0)
	{
		const uint64 RequestCount = FMath::Max<uint64>(BulkDeferredRequestCount, bWasEdited ? 1 : 0);
		BulkDeferredRequestCount = 0;
		if (RequestCount > 0)
		{
			RecordDeferredTreeBuild(CurrentTimeSeconds, RequestCount);
		}
	}
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::CancelBulkEdit()
{
	check(IsInGameThread());
	check(BulkEditDepth > 0);
	BulkEditDepth = 0;
	BulkDeferredRequestCount = 0;
	bTreeBuildPending = false;
	FirstDeferredTimeSeconds = 0.0;
	LastDeferredTimeSeconds = 0.0;
}

bool UMeshPoolHierarchicalInstancedStaticMeshComponent::SetMeshPoolCustomDataRange(
	const int32 InstanceIndexStart, const int32 InstanceIndexEnd,
	const TConstArrayView<float> CustomDataFloats)
{
	const int32 InstanceCount = InstanceIndexEnd - InstanceIndexStart + 1;
	if (InstanceCount <= 0 || InstanceIndexStart < 0 || InstanceIndexEnd >= GetInstanceCount()
		|| CustomDataFloats.IsEmpty() || CustomDataFloats.Num() != InstanceCount * NumCustomDataFloats)
	{
		return false;
	}

	// 基类的 Range API 不经过 HISM 的单实例 override。异步时只让首实例经过
	// override 以设置 bConcurrentChanges，整段数据仍用连续 Range API 一次写入。
	const int32 FloatsPerInstance = CustomDataFloats.Num() / InstanceCount;
	if (IsAsyncBuilding()
		&& !UHierarchicalInstancedStaticMeshComponent::SetCustomData(
			InstanceIndexStart, CustomDataFloats.Left(FloatsPerInstance), false))
	{
		return false;
	}
	return UInstancedStaticMeshComponent::SetCustomData(
		InstanceIndexStart, InstanceIndexEnd, CustomDataFloats, false);
}

bool UMeshPoolHierarchicalInstancedStaticMeshComponent::PublishSmallTreeImmediately(
	const int32 MaximumInstanceCount)
{
	check(IsInGameThread());
	const int32 InstanceCount = GetInstanceCount();
	if (BulkEditDepth > 0 || IsAsyncBuilding() || MaximumInstanceCount <= 0
		|| InstanceCount <= 0 || InstanceCount > MaximumInstanceCount)
	{
		return false;
	}

	if (IsTreeFullyBuilt() && GetNumRenderInstances() == InstanceCount)
	{
		bTreeBuildPending = false;
		FirstDeferredTimeSeconds = 0.0;
		LastDeferredTimeSeconds = 0.0;
		return false;
	}

	// HISM SceneProxy 缓存 NumBuiltInstances；仅提交增量实例缓冲不会让新实例进入 Draw。
	// 小型交互 Cluster 直接发布完整树，避免玩家新放建筑等待全局大树队列。
	bAllowTreeBuildNow = true;
	const bool bRequested = BuildTreeIfOutdated(/*Async*/ false, /*ForceUpdate*/ false);
	bAllowTreeBuildNow = false;
	if (IsTreeFullyBuilt() && GetNumRenderInstances() == InstanceCount)
	{
		bTreeBuildPending = false;
		FirstDeferredTimeSeconds = 0.0;
		LastDeferredTimeSeconds = 0.0;
	}
	return bRequested;
}

bool UMeshPoolHierarchicalInstancedStaticMeshComponent::TryStartDeferredTreeBuild(const double CurrentTimeSeconds,
														  const double QuietSeconds,
														  const double MaxDeferralSeconds,
														  const bool bForce)
{
	check(IsInGameThread());
	if (!bTreeBuildPending || BulkEditDepth > 0 || IsAsyncBuilding())
	{
		return false;
	}
	if (IsTreeFullyBuilt() && GetNumRenderInstances() == GetInstanceCount())
	{
		// 纯 Custom Data 通过实例缓冲发布，不需要为同一份几何再构建 Cluster Tree。
		ClearDeferredTreeBuild();
		return false;
	}
	const bool bQuietPeriodElapsed = CurrentTimeSeconds - LastDeferredTimeSeconds >= FMath::Max(0.0, QuietSeconds);
	const bool bMaximumDeferralElapsed =
		CurrentTimeSeconds - FirstDeferredTimeSeconds >= FMath::Max(0.0, MaxDeferralSeconds);
	if (!bForce && !bQuietPeriodElapsed && !bMaximumDeferralElapsed)
	{
		return false;
	}

	bAllowTreeBuildNow = true;
	const bool bRequested = BuildTreeIfOutdated(/*Async*/ true, /*ForceUpdate*/ bForce);
	bAllowTreeBuildNow = false;
	if (!IsAsyncBuilding())
	{
		bManagedBuildInFlight = false;
	}
	if (bRequested)
	{
		bTreeBuildPending = false;
		FirstDeferredTimeSeconds = 0.0;
		LastDeferredTimeSeconds = 0.0;
	}
	return bRequested;
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::RecordDeferredTreeBuild(const double CurrentTimeSeconds,
																				const uint64 RequestCount)
{
	if (RequestCount == 0)
	{
		return;
	}
	DeferredTreeBuildRequestCount += RequestCount;
	if (bTreeBuildPending)
	{
		CoalescedTreeBuildRequestCount += RequestCount;
	}
	else
	{
		bTreeBuildPending = true;
		FirstDeferredTimeSeconds = CurrentTimeSeconds;
		CoalescedTreeBuildRequestCount += RequestCount - 1;
	}
	LastDeferredTimeSeconds = CurrentTimeSeconds;
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::BuildTree()
{
	if (!bAllowTreeBuildNow)
	{
		if (BulkEditDepth > 0)
		{
			++BulkDeferredRequestCount;
		}
		else
		{
			RecordDeferredTreeBuild(FPlatformTime::Seconds());
		}
		return;
	}
	++TreeBuildRequestCount;
	Super::BuildTree();
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::BuildTreeAsync()
{
	if (BulkEditDepth > 0 || (!bAllowTreeBuildNow && !bManagedBuildInFlight))
	{
		if (BulkEditDepth > 0)
		{
			++BulkDeferredRequestCount;
		}
		else
		{
			RecordDeferredTreeBuild(FPlatformTime::Seconds());
		}
		return;
	}

	// UE 的完成回调在 Bulk Edit 外执行。只有当前已批准构建的内部重试可继续通过。
	if (!bAllowTreeBuildNow) ++TreeBuildRetryCount;
	bManagedBuildInFlight = true;
	++TreeBuildRequestCount;
	Super::BuildTreeAsync();
	if (!IsAsyncBuilding())
	{
		bManagedBuildInFlight = false;
		if (IsTreeFullyBuilt()) ClearDeferredTreeBuild();
	}
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::HandleTreeBuilt(
	UHierarchicalInstancedStaticMeshComponent* Component, const bool bWasAsyncBuild)
{
	(void)bWasAsyncBuild;
	if (Component != this) return;
	// UE 的 ApplyBuildTree 不是虚函数，使用发布通知即时结束权限，不等待下次 Host 轮询。
	bManagedBuildInFlight = false;
	ClearDeferredTreeBuild();
}

void UMeshPoolHierarchicalInstancedStaticMeshComponent::ClearDeferredTreeBuild()
{
	bTreeBuildPending = false;
	FirstDeferredTimeSeconds = 0.0;
	LastDeferredTimeSeconds = 0.0;
}
