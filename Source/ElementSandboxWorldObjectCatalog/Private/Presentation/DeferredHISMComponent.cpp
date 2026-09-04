#include "Presentation/DeferredHISMComponent.h"

#include "HAL/PlatformTime.h"

void UDeferredHISMComponent::BeginBulkEdit()
{
	check(IsInGameThread());
	++BulkEditDepth;
}

void UDeferredHISMComponent::EndBulkEdit(
	const double CurrentTimeSeconds,
	const bool bWasEdited)
{
	check(IsInGameThread() && BulkEditDepth > 0);
	if (--BulkEditDepth == 0)
	{
		const uint64 Requests = FMath::Max<uint64>(BulkDeferredRequests, bWasEdited ? 1 : 0);
		BulkDeferredRequests = 0;
		if (Requests > 0)
		{
			RecordDeferredBuild(CurrentTimeSeconds, Requests);
		}
	}
}

void UDeferredHISMComponent::SetNumCustomDataFloats(const int32 InNumCustomDataFloats)
{
	check(IsInGameThread());
	const int32 NewFloatCount = FMath::Max(0, InNumCustomDataFloats);
	if (NumCustomDataFloats == NewFloatCount)
	{
		return;
	}

	UInstancedStaticMeshComponent::SetNumCustomDataFloats(NewFloatCount);
	if (IsAsyncBuilding())
	{
		// BuildTreeIfOutdated 在已有异步任务时只设置 bConcurrentChanges；完成回调随后
		// 丢弃旧参数布局并通过本类的受控重试入口重新捕获当前数据。
		UHierarchicalInstancedStaticMeshComponent::BuildTreeIfOutdated(
			/*Async*/ true, /*ForceUpdate*/ true);
	}
}

bool UDeferredHISMComponent::SetCustomDataRange(
	const int32 InstanceIndexStart,
	const int32 InstanceIndexEnd,
	const TConstArrayView<float> CustomDataFloats)
{
	const int32 InstanceCount = InstanceIndexEnd - InstanceIndexStart + 1;
	if (InstanceCount <= 0 || InstanceIndexStart < 0 || InstanceIndexEnd >= GetInstanceCount()
		|| CustomDataFloats.IsEmpty() || CustomDataFloats.Num() != InstanceCount * NumCustomDataFloats)
	{
		return false;
	}

	// HISM 没有 Range override。异步时先让首实例经过单实例 override，只为设置
	// bConcurrentChanges；整段数据仍由原生连续 Range API 一次写入。
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

bool UDeferredHISMComponent::PrewarmEmptyTree()
{
	check(IsInGameThread());
	if (BulkEditDepth > 0 || GetInstanceCount() != 0 || IsAsyncBuilding())
	{
		return false;
	}
	bAllowBuild = true;
	const bool bBuilt = BuildTreeIfOutdated(true, true);
	bAllowBuild = false;
	if (!IsAsyncBuilding())
	{
		bManagedBuildInFlight = false;
	}
	bBuildPending = false;
	FirstDeferredSeconds = 0.0;
	LastDeferredSeconds = 0.0;
	return bBuilt || IsTreeFullyBuilt();
}

void UDeferredHISMComponent::RecordDeferredBuild(
	const double CurrentTimeSeconds,
	const uint64 RequestCount)
{
	if (RequestCount == 0)
	{
		return;
	}
	if (bBuildPending)
	{
		CoalescedTreeBuildCount += RequestCount;
	}
	else
	{
		bBuildPending = true;
		FirstDeferredSeconds = CurrentTimeSeconds;
		CoalescedTreeBuildCount += RequestCount - 1;
	}
	LastDeferredSeconds = CurrentTimeSeconds;
}

bool UDeferredHISMComponent::TryStartDeferredTreeBuild(
	const double CurrentTimeSeconds,
	const double QuietSeconds,
	const double MaxDeferralSeconds,
	const bool bForce)
{
	if (!bBuildPending || BulkEditDepth > 0 || IsAsyncBuilding())
	{
		return false;
	}
	if (!bForce
		&& CurrentTimeSeconds - LastDeferredSeconds < FMath::Max(0.0, QuietSeconds)
		&& CurrentTimeSeconds - FirstDeferredSeconds < FMath::Max(0.0, MaxDeferralSeconds))
	{
		return false;
	}
	bAllowBuild = true;
	const bool bStarted = BuildTreeIfOutdated(true, bForce);
	bAllowBuild = false;
	if (!IsAsyncBuilding())
	{
		bManagedBuildInFlight = false;
	}
	if (bStarted)
	{
		bBuildPending = false;
		FirstDeferredSeconds = 0.0;
		LastDeferredSeconds = 0.0;
	}
	else if (IsTreeFullyBuilt())
	{
		bBuildPending = false;
		FirstDeferredSeconds = 0.0;
		LastDeferredSeconds = 0.0;
	}
	return bStarted;
}

void UDeferredHISMComponent::NotifyAsyncBuildObservedComplete()
{
	check(IsInGameThread());
	if (!IsAsyncBuilding())
	{
		bManagedBuildInFlight = false;
		// ApplyBuildTreeAsync 已把并发编辑折叠进内部重试；树与当前实例一致时，
		// 外层在构建期间记录的 Pending 不再代表新工作。
		if (IsTreeFullyBuilt())
		{
			bBuildPending = false;
			FirstDeferredSeconds = 0.0;
			LastDeferredSeconds = 0.0;
		}
	}
}

void UDeferredHISMComponent::BuildTree()
{
	if (!bAllowBuild)
	{
		if (BulkEditDepth > 0)
		{
			++BulkDeferredRequests;
		}
		else
		{
			RecordDeferredBuild(FPlatformTime::Seconds());
		}
		return;
	}
	++TreeBuildCount;
	Super::BuildTree();
}

void UDeferredHISMComponent::BuildTreeAsync()
{
	if (!bAllowBuild && !bManagedBuildInFlight)
	{
		if (BulkEditDepth > 0)
		{
			++BulkDeferredRequests;
		}
		else
		{
			RecordDeferredBuild(FPlatformTime::Seconds());
		}
		return;
	}

	// ApplyBuildTreeAsync 检测到并发编辑时会再次调用此虚函数。内部重试必须穿过
	// 外层门禁，否则组件会永久停在旧 Cluster Tree。
	bManagedBuildInFlight = true;
	++TreeBuildCount;
	Super::BuildTreeAsync();
}
