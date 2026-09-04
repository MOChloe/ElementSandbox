#include "PresentationWorldSubsystem.h"

#include "ElementSandboxPresentation.h"
#include "MeshPoolRenderHost.h"
#include "PresentationWorldData.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

namespace
{
	bool CopyCustomData(
		const int32 ExpectedCount,
		const TConstArrayView<float> Input,
		TArray<float, TInlineAllocator<4>>& Output,
		const bool bAllowEmptyAsZero)
	{
		if (ExpectedCount < 0 || ExpectedCount > 8
			|| (!Input.IsEmpty() && Input.Num() != ExpectedCount)
			|| (Input.IsEmpty() && ExpectedCount > 0 && !bAllowEmptyAsZero))
		{
			return false;
		}
		Output.Reset();
		if (Input.IsEmpty())
		{
			Output.Init(0.0f, ExpectedCount);
			return true;
		}
		for (const float Value : Input)
		{
			if (!FMath::IsFinite(Value))
			{
				return false;
			}
		}
		Output.Append(Input);
		return true;
	}

	bool IsEquivalentViewSample(
		const FPresentationViewSource& Left,
		const FPresentationViewSource& Right)
	{
		constexpr double LocationToleranceCentimeters = 0.1;
		constexpr double DirectionTolerance = 1.0e-4;
		return Left.ViewLocation.Equals(Right.ViewLocation, LocationToleranceCentimeters)
			&& Left.SubjectLocation.Equals(Right.SubjectLocation, LocationToleranceCentimeters)
			&& Left.Forward.Equals(Right.Forward, DirectionTolerance)
			&& Left.Right.Equals(Right.Right, DirectionTolerance)
			&& Left.Up.Equals(Right.Up, DirectionTolerance)
			&& FMath::IsNearlyEqual(Left.HorizontalFOVDegrees, Right.HorizontalFOVDegrees, 0.01f)
			&& FMath::IsNearlyEqual(Left.AspectRatio, Right.AspectRatio, 1.0e-4f)
			&& Left.ViewportSize == Right.ViewportSize
			&& Left.Priority == Right.Priority;
	}
}

UPresentationWorldSubsystem::UPresentationWorldSubsystem() = default;
UPresentationWorldSubsystem::~UPresentationWorldSubsystem() = default;

void UPresentationWorldSubsystem::ReleaseInstanceSlot(const int32 SlotIndex)
{
	check(Data && Data->Instances.IsValidIndex(SlotIndex));
	FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[SlotIndex];
	check(Slot.bAllocated);
	const bool bWasPhysicallyResident = Slot.bCommitted;
	const FMeshPoolInstanceHandle RetiredHandle(Data->PoolId, SlotIndex, Slot.Generation);
	Data->ReleaseInstance(SlotIndex);
	if (bWasPhysicallyResident)
	{
		InstanceRetiredDelegate.Broadcast(RetiredHandle);
	}
}

void UPresentationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Data = MakePimpl<FPresentationWorldData>();
	Data->bEnabled = GetWorld() && !GetWorld()->IsNetMode(NM_DedicatedServer);
}

void UPresentationWorldSubsystem::PostInitialize()
{
	Super::PostInitialize();
	if (Data && Data->bEnabled)
	{
		EnsureRenderHost();
	}
}

void UPresentationWorldSubsystem::Deinitialize()
{
	if (IsValid(RenderHost))
	{
		RenderHost->ClearAll();
		if (!GetWorld() || !GetWorld()->IsBeingCleanedUp())
		{
			RenderHost->Destroy();
		}
	}
	RenderHost = nullptr;
	Data.Reset();
	Super::Deinitialize();
}

void UPresentationWorldSubsystem::Tick(const float DeltaTime)
{
	if (!Data || !Data->bEnabled
		|| (!Data->bProjectionDirty && Data->PendingInstanceCount == 0
			&& !Data->bDeferredTreeBuildPending))
	{
		return;
	}
	// Dirty 驱动时每帧最多推进一次完整客户端表现周期。客户端表现不继承服务器 Authority 8 Hz；
	// 将 GameplayDestroy 聚合 125 ms 会同时造成源投影延迟和集中 HISM 提交尖峰。
	if (Data->bProjectionDirty)
	{
		RunScheduledCycle();
	}
	else
	{
		FlushSlots();
	}
}

bool UPresentationWorldSubsystem::IsTickable() const
{
	return Data && Data->bEnabled
		&& (Data->bProjectionDirty || Data->PendingInstanceCount > 0
			|| Data->bDeferredTreeBuildPending);
}

TStatId UPresentationWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPresentationWorldSubsystem, STATGROUP_Tickables);
}

bool UPresentationWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::GamePreview;
}

FPresentationSourceHandle UPresentationWorldSubsystem::RegisterSource(
	const FPresentationViewSource& Source)
{
	check(IsInGameThread());
	if (!Data || !Data->bEnabled || !Source.IsValid())
	{
		return {};
	}
	int32 Index = INDEX_NONE;
	if (Data->FirstFreeSource != INDEX_NONE)
	{
		Index = Data->FirstFreeSource;
		Data->FirstFreeSource = Data->Sources[Index].NextFree;
	}
	else
	{
		Index = Data->Sources.AddDefaulted();
	}
	FPresentationWorldData::FSourceSlot& Slot = Data->Sources[Index];
	Slot.bAlive = true;
	Slot.NextFree = INDEX_NONE;
	Slot.View = Source;
	const FPresentationSourceHandle Handle(Data->WorldId, Index, Slot.Generation);
	Slot.View.SourceHandle = Handle;
	Slot.View.Forward.Normalize();
	Slot.View.Right.Normalize();
	Slot.View.Up.Normalize();
	++Data->SourceCount;
	++Data->SourceRegistryRevision;
	Data->bProjectionDirty = true;
	ViewSourceUpdatedDelegate.Broadcast(Slot.View);
	return Handle;
}

bool UPresentationWorldSubsystem::UpdateSource(
	const FPresentationSourceHandle Source,
	const FPresentationViewSource& View)
{
	check(IsInGameThread());
	FPresentationWorldData::FSourceSlot* Slot = Data ? Data->FindSource(Source) : nullptr;
	if (!Slot || !View.IsValid())
	{
		return false;
	}
	if (IsEquivalentViewSample(Slot->View, View))
	{
		return true;
	}
	Slot->View = View;
	Slot->View.SourceHandle = Source;
	Slot->View.Forward.Normalize();
	Slot->View.Right.Normalize();
	Slot->View.Up.Normalize();
	++Data->SourceRegistryRevision;
	Data->bProjectionDirty = true;
	ViewSourceUpdatedDelegate.Broadcast(Slot->View);
	return true;
}

bool UPresentationWorldSubsystem::UnregisterSource(const FPresentationSourceHandle Source)
{
	check(IsInGameThread());
	FPresentationWorldData::FSourceSlot* Slot = Data ? Data->FindSource(Source) : nullptr;
	if (!Slot)
	{
		return false;
	}
	const int32 Index = Source.GetIndex();
	ViewSourceRemovedDelegate.Broadcast(Source);
	Slot->bAlive = false;
	Slot->View = {};
	FPresentationWorldData::AdvanceGeneration(Slot->Generation);
	Slot->NextFree = Data->FirstFreeSource;
	Data->FirstFreeSource = Index;
	--Data->SourceCount;
	++Data->SourceRegistryRevision;
	Data->bProjectionDirty = true;
	return true;
}

int32 UPresentationWorldSubsystem::GetSourceCount() const
{
	return Data ? Data->SourceCount : 0;
}

FPresentationProjectorHandle UPresentationWorldSubsystem::RegisterProjector(
	const FName Name,
	FPresentationProjectorDelegate Delegate)
{
	check(IsInGameThread());
	if (!Data || !Data->bEnabled || Name.IsNone() || !Delegate.IsBound())
	{
		return {};
	}
	int32 Index = INDEX_NONE;
	if (Data->FirstFreeProjector != INDEX_NONE)
	{
		Index = Data->FirstFreeProjector;
		Data->FirstFreeProjector = Data->Projectors[Index].NextFree;
	}
	else
	{
		Index = Data->Projectors.AddDefaulted();
	}
	FPresentationWorldData::FProjectorSlot& Slot = Data->Projectors[Index];
	Slot.bAlive = true;
	Slot.NextFree = INDEX_NONE;
	Slot.Name = Name;
	Slot.Delegate = MoveTemp(Delegate);
	Data->bProjectionDirty = true;
	return FPresentationProjectorHandle(Data->WorldId, Index, Slot.Generation);
}

bool UPresentationWorldSubsystem::UnregisterProjector(
	const FPresentationProjectorHandle Projector)
{
	check(IsInGameThread());
	FPresentationWorldData::FProjectorSlot* Slot = Data ? Data->FindProjector(Projector) : nullptr;
	if (!Slot)
	{
		return false;
	}
	Slot->bAlive = false;
	Slot->Name = NAME_None;
	Slot->Delegate.Unbind();
	FPresentationWorldData::AdvanceGeneration(Slot->Generation);
	Slot->NextFree = Data->FirstFreeProjector;
	Data->FirstFreeProjector = Projector.GetIndex();
	return true;
}

bool UPresentationWorldSubsystem::RequestProjectionCycle()
{
	check(IsInGameThread());
	if (!Data)
	{
		return false;
	}
	if (Data->bEnabled)
	{
		Data->bProjectionDirty = true;
	}
	return true;
}

FMeshPoolLayerHandle UPresentationWorldSubsystem::RegisterMeshLayer(const FName Name)
{
	check(IsInGameThread());
	if (!Data || !Data->bEnabled || Name.IsNone())
	{
		return {};
	}
	int32 Index = INDEX_NONE;
	if (Data->FirstFreeLayer != INDEX_NONE)
	{
		Index = Data->FirstFreeLayer;
		Data->FirstFreeLayer = Data->Layers[Index].NextFree;
	}
	else
	{
		Index = Data->Layers.AddDefaulted();
	}
	FPresentationWorldData::FLayerSlot& Slot = Data->Layers[Index];
	Slot.bAlive = true;
	Slot.bReprojectionRequested = false;
	Slot.NextFree = INDEX_NONE;
	Slot.Name = Name;
	return FMeshPoolLayerHandle(Data->WorldId, Index, Slot.Generation);
}

bool UPresentationWorldSubsystem::UnregisterMeshLayer(const FMeshPoolLayerHandle Layer)
{
	check(IsInGameThread());
	FPresentationWorldData::FLayerSlot* LayerSlot = Data ? Data->FindLayer(Layer) : nullptr;
	if (!LayerSlot)
	{
		return false;
	}
	if (IsValid(RenderHost))
	{
		RenderHost->ClearLayer(Layer);
	}
	for (int32 Index = 0; Index < Data->Instances.Num(); ++Index)
	{
		const FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[Index];
		if (Slot.bAllocated
			&& (Slot.DesiredCluster.Layer == Layer || Slot.CurrentCluster.Layer == Layer))
		{
			ReleaseInstanceSlot(Index);
		}
	}
	LayerSlot = &Data->Layers[Layer.GetIndex()];
	LayerSlot->bAlive = false;
	LayerSlot->Name = NAME_None;
	LayerSlot->bReprojectionRequested = false;
	FPresentationWorldData::AdvanceGeneration(LayerSlot->Generation);
	LayerSlot->NextFree = Data->FirstFreeLayer;
	Data->FirstFreeLayer = Layer.GetIndex();
	return true;
}

bool UPresentationWorldSubsystem::ConsumeLayerReprojectionRequest(
	const FMeshPoolLayerHandle Layer)
{
	FPresentationWorldData::FLayerSlot* Slot = Data ? Data->FindLayer(Layer) : nullptr;
	if (!Slot || !Slot->bReprojectionRequested)
	{
		return false;
	}
	Slot->bReprojectionRequested = false;
	return true;
}

FMeshPoolInstanceHandle UPresentationWorldSubsystem::QueueAdd(const FMeshPoolClusterKey& Cluster,
															  const FTransform& WorldTransform,
															  const TConstArrayView<float> CustomData)
{
	check(IsInGameThread());
	if (!Data || !Data->bEnabled || !Cluster.IsSet() || !Data->FindLayer(Cluster.Layer) || WorldTransform.ContainsNaN())
	{
		return {};
	}
	const FMeshPoolInstanceHandle Handle = Data->AllocateInstance();
	FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[Handle.GetIndex()];
	Slot.DesiredCluster = Cluster;
	Slot.DesiredTransform = WorldTransform;
	Slot.bTransformDirty = true;
	Slot.bCustomDataDirty = Cluster.CustomDataFloatCount > 0;
	const SIZE_T PreviousCustomDataSize = Slot.DesiredCustomData.GetAllocatedSize();
	if (!CopyCustomData(Cluster.CustomDataFloatCount, CustomData, Slot.DesiredCustomData, true))
	{
		ReleaseInstanceSlot(Handle.GetIndex());
		return {};
	}
	Data->UpdateCustomDataAllocation(PreviousCustomDataSize, Slot);
	Data->MarkInstanceDirty(Handle.GetIndex());
	if (Cluster.Backend == EMeshPoolBackend::ImmediateMovable)
	{
		const int32 Index = Handle.GetIndex();
		if (!FlushSlots(MakeArrayView(&Index, 1)))
		{
			// Flush 失败会先清空失败 Layer 的物理状态；调用方拿不到句柄时
			// 必须同步回收逻辑 Slot，不能留下无人可达的 Pending 实例。
			if (Data->FindInstance(Handle, false))
			{
				ReleaseInstanceSlot(Index);
			}
			return {};
		}
	}
	return Handle;
}

bool UPresentationWorldSubsystem::PrioritizePendingInstance(const FMeshPoolInstanceHandle Instance)
{
	check(IsInGameThread());
	FPresentationWorldData::FInstanceSlot* Slot = Data ? Data->FindInstance(Instance) : nullptr;
	if (!Slot)
	{
		return false;
	}
	if (Slot->IsQueuedForFlush())
	{
		Data->MarkInstanceUrgent(Instance.GetIndex());
	}
	return true;
}

bool UPresentationWorldSubsystem::QueueUpdate(const FMeshPoolInstanceHandle Instance, const FTransform& WorldTransform,
											  const TConstArrayView<float> CustomData)
{
	check(IsInGameThread());
	FPresentationWorldData::FInstanceSlot* Slot = Data ? Data->FindInstance(Instance) : nullptr;
	if (!Slot || WorldTransform.ContainsNaN())
	{
		return false;
	}
	TArray<float, TInlineAllocator<4>> NewCustomData;
	if (!CustomData.IsEmpty())
	{
		if (!CopyCustomData(Slot->DesiredCluster.CustomDataFloatCount, CustomData, NewCustomData, false))
		{
			return false;
		}
	}
	Slot->DesiredTransform = WorldTransform;
	Slot->bTransformDirty = true;
	if (!CustomData.IsEmpty())
	{
		const SIZE_T PreviousCustomDataSize = Slot->DesiredCustomData.GetAllocatedSize();
		Slot->DesiredCustomData = MoveTemp(NewCustomData);
		Data->UpdateCustomDataAllocation(PreviousCustomDataSize, *Slot);
		Slot->bCustomDataDirty = true;
	}
	Data->MarkInstanceDirty(Instance.GetIndex());
	if (Slot->bCommitted && Slot->CurrentCluster.Backend == EMeshPoolBackend::ImmediateMovable)
	{
		const int32 Index = Instance.GetIndex();
		return FlushSlots(MakeArrayView(&Index, 1));
	}
	return true;
}

bool UPresentationWorldSubsystem::QueueCustomData(const FMeshPoolInstanceHandle Instance,
												  const TConstArrayView<float> CustomData)
{
	FPresentationWorldData::FInstanceSlot* Slot = Data ? Data->FindInstance(Instance) : nullptr;
	TArray<float, TInlineAllocator<4>> NewCustomData;
	if (!Slot || !CopyCustomData(Slot->DesiredCluster.CustomDataFloatCount, CustomData, NewCustomData, false))
	{
		return false;
	}
	const SIZE_T PreviousCustomDataSize = Slot->DesiredCustomData.GetAllocatedSize();
	Slot->DesiredCustomData = MoveTemp(NewCustomData);
	Data->UpdateCustomDataAllocation(PreviousCustomDataSize, *Slot);
	Slot->bCustomDataDirty = true;
	Data->MarkInstanceDirty(Instance.GetIndex());
	if (Slot->bCommitted && Slot->CurrentCluster.Backend == EMeshPoolBackend::ImmediateMovable)
	{
		const int32 Index = Instance.GetIndex();
		return FlushSlots(MakeArrayView(&Index, 1));
	}
	return true;
}

bool UPresentationWorldSubsystem::QueueMigrate(const FMeshPoolInstanceHandle Instance,
											   const FMeshPoolClusterKey& TargetCluster,
											   const FTransform& WorldTransform,
											   const TConstArrayView<float> CustomData)
{
	check(IsInGameThread());
	FPresentationWorldData::FInstanceSlot* Slot = Data ? Data->FindInstance(Instance) : nullptr;
	if (!Slot || !TargetCluster.IsSet() || !Data->FindLayer(TargetCluster.Layer) || WorldTransform.ContainsNaN())
	{
		return false;
	}
	TArray<float, TInlineAllocator<4>> NewCustomData;
	if (CustomData.IsEmpty() && TargetCluster.CustomDataFloatCount == Slot->DesiredCluster.CustomDataFloatCount)
	{
		NewCustomData = Slot->DesiredCustomData;
	}
	else if (!CopyCustomData(TargetCluster.CustomDataFloatCount, CustomData, NewCustomData, true))
	{
		return false;
	}
	Slot->DesiredCluster = TargetCluster;
	Slot->DesiredTransform = WorldTransform;
	const SIZE_T PreviousCustomDataSize = Slot->DesiredCustomData.GetAllocatedSize();
	Slot->DesiredCustomData = MoveTemp(NewCustomData);
	Data->UpdateCustomDataAllocation(PreviousCustomDataSize, *Slot);
	Slot->bTransformDirty = true;
	Slot->bCustomDataDirty = TargetCluster.CustomDataFloatCount > 0;
	Data->MarkInstanceDirty(Instance.GetIndex());
	if (TargetCluster.Backend == EMeshPoolBackend::ImmediateMovable)
	{
		const int32 Index = Instance.GetIndex();
		return FlushSlots(MakeArrayView(&Index, 1));
	}
	return true;
}

bool UPresentationWorldSubsystem::QueueRemove(const FMeshPoolInstanceHandle Instance)
{
	check(IsInGameThread());
	FPresentationWorldData::FInstanceSlot* Slot = Data ? Data->FindInstance(Instance) : nullptr;
	if (!Slot)
	{
		return false;
	}
	if (!Slot->bCommitted)
	{
		// Add -> Remove 在提交前完全抵消，不触碰引擎组件。
		ReleaseInstanceSlot(Instance.GetIndex());
		return true;
	}
	Slot->bLogicallyAlive = false;
	Slot->bRemoveQueued = true;
	Data->MarkInstanceDirty(Instance.GetIndex());
	// GameplayDestroy 后的源投影退出必须先于新碎片显示；删除进入独立高优先队列，
	// 普通 MeshPool Tick 仍按实际耗时预算分批推进，避免销毁风暴重新制造长帧。
	Data->MarkInstanceUrgent(Instance.GetIndex());
	if (Slot->CurrentCluster.Backend == EMeshPoolBackend::ImmediateMovable)
	{
		const int32 Index = Instance.GetIndex();
		return FlushSlots(MakeArrayView(&Index, 1));
	}
	return true;
}

bool UPresentationWorldSubsystem::IsValidInstance(const FMeshPoolInstanceHandle Instance) const
{
	return Data && Data->FindInstance(Instance) != nullptr;
}

bool UPresentationWorldSubsystem::IsInstancePhysicallyResident(
	const FMeshPoolInstanceHandle Instance) const
{
	const FPresentationWorldData::FInstanceSlot* Slot = Data
		? Data->FindInstance(Instance, false) : nullptr;
	return Slot && Slot->bCommitted;
}

bool UPresentationWorldSubsystem::TryGetInstanceTransform(
	const FMeshPoolInstanceHandle Instance,
	FTransform& OutWorldTransform) const
{
	const FPresentationWorldData::FInstanceSlot* Slot = Data
		? Data->FindInstance(Instance)
		: nullptr;
	if (!Slot)
	{
		return false;
	}
	OutWorldTransform = Slot->DesiredTransform;
	return true;
}

bool UPresentationWorldSubsystem::FlushNow()
{
	check(IsInGameThread());
	return Data && (!Data->bEnabled || FlushSlots({}, /*bForceTreeBuild*/ true));
}

bool UPresentationWorldSubsystem::RunCycleNow()
{
	check(IsInGameThread());
	return Data && (!Data->bEnabled || RunScheduledCycle(/*bForceTreeBuild*/ true));
}

bool UPresentationWorldSubsystem::CopyCurrentViewSnapshot(FPresentationViewSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	OutSnapshot = {};
	if (!Data)
	{
		return false;
	}
	OutSnapshot.Revision = Data->SourceRegistryRevision;
	OutSnapshot.Sources.Reserve(Data->SourceCount);
	for (const FPresentationWorldData::FSourceSlot& Slot : Data->Sources)
	{
		if (Slot.bAlive)
		{
			OutSnapshot.Sources.Add(Slot.View);
		}
	}
	OutSnapshot.Sources.StableSort([](const FPresentationViewSource& Left, const FPresentationViewSource& Right)
	{
		return Left.Priority > Right.Priority;
	});
	return true;
}

bool UPresentationWorldSubsystem::RunScheduledCycle(const bool bForceTreeBuild)
{
	check(IsInGameThread());
	if (!Data || !Data->bEnabled)
	{
		return Data != nullptr;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_ScheduledCycle);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, ScheduledCycle);
	Data->bProjectionDirty = false;
	++Data->ScheduledCycleCount;
	FPresentationViewSnapshot Snapshot;
	CopyCurrentViewSnapshot(Snapshot);

	TArray<int32> ProjectorIndices;
	for (int32 Index = 0; Index < Data->Projectors.Num(); ++Index)
	{
		if (Data->Projectors[Index].bAlive)
		{
			ProjectorIndices.Add(Index);
		}
	}
	for (const int32 Index : ProjectorIndices)
	{
		FPresentationWorldData::FProjectorSlot& Slot = Data->Projectors[Index];
		if (Slot.bAlive && Slot.Delegate.IsBound())
		{
			Slot.Delegate.Execute(Snapshot);
		}
	}
	return FlushSlots({}, bForceTreeBuild);
}

bool UPresentationWorldSubsystem::RecoverFailedLayers(const TSet<FMeshPoolLayerHandle>& Layers)
{
	Data->bProjectionDirty = true;
	for (const FMeshPoolLayerHandle Layer : Layers)
	{
		if (FPresentationWorldData::FLayerSlot* LayerSlot = Data->FindLayer(Layer))
		{
			LayerSlot->bReprojectionRequested = true;
		}
		if (IsValid(RenderHost))
		{
			RenderHost->ClearLayer(Layer);
		}
	}
	for (int32 Index = 0; Index < Data->Instances.Num(); ++Index)
	{
		FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[Index];
		if (!Slot.bAllocated ||
			(!Layers.Contains(Slot.CurrentCluster.Layer) && !Layers.Contains(Slot.DesiredCluster.Layer)))
		{
			continue;
		}
		if (Slot.bRemoveQueued)
		{
			ReleaseInstanceSlot(Index);
			continue;
		}
		Slot.CurrentCluster = {};
		Slot.bCommitted = false;
		Slot.bTransformDirty = true;
		Slot.bCustomDataDirty = Slot.DesiredCluster.CustomDataFloatCount > 0;
		Data->MarkInstanceDirty(Index);
	}
	return true;
}

bool UPresentationWorldSubsystem::EnsureRenderHost()
{
	if (IsValid(RenderHost))
	{
		return true;
	}
	UWorld* World = GetWorld();
	if (!Data || !Data->bEnabled || !World || World->IsNetMode(NM_DedicatedServer))
	{
		return false;
	}
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RenderHost = World->SpawnActor<AMeshPoolRenderHost>(Parameters);
	return IsValid(RenderHost);
}

FMeshPoolStats UPresentationWorldSubsystem::GetMeshPoolStats() const
{
	FMeshPoolStats Stats;
	if (!Data || !Data->bEnabled)
	{
		return Stats;
	}
	Stats.ResidentInstanceCount = IsValid(RenderHost) ? RenderHost->GetInstanceCount() : 0;
	Stats.ClusterCount = IsValid(RenderHost) ? RenderHost->GetClusterCount() : 0;
	Stats.HierarchicalTreeBuildRequests = IsValid(RenderHost) ? RenderHost->GetHierarchicalTreeBuildRequestCount() : 0;
	Stats.HierarchicalTreeBuildRetries = IsValid(RenderHost) ? RenderHost->GetHierarchicalTreeBuildRetryCount() : 0;
	Stats.HierarchicalTreeBuildDeferredRequests =
		IsValid(RenderHost) ? RenderHost->GetHierarchicalTreeBuildDeferredRequestCount() : 0;
	Stats.HierarchicalTreeBuildCoalescedRequests =
		IsValid(RenderHost) ? RenderHost->GetHierarchicalTreeBuildCoalescedRequestCount() : 0;
	Stats.FlushCount = Data->FlushCount;
	Stats.ScheduledCycleCount = Data->ScheduledCycleCount;
	Stats.EmptyFlushCount = Data->EmptyFlushCount;
	Stats.LastVisitedDirtySlotCount = Data->LastVisitedDirtySlotCount;
	Stats.TotalVisitedDirtySlotCount = Data->TotalVisitedDirtySlotCount;
	Stats.FailedFlushCount = Data->FailedFlushCount;
	Stats.LastFlushClusterCount = Data->LastFlushClusterCount;
	Stats.LastFlushBatchCount = Data->LastFlushBatchCount;
	Stats.LastFlushInstanceCount = Data->LastFlushInstanceCount;
	Stats.MaxFlushClusterCount = Data->MaxFlushClusterCount;
	Stats.MaxFlushInstanceCount = Data->MaxFlushInstanceCount;
	Stats.LastInstanceApplyMilliseconds = Data->LastInstanceApplyMilliseconds;
	Stats.TotalFlushedInstanceCount = Data->TotalFlushedInstanceCount;
	Stats.EstimatedCPUAllocatedSize = Data->Sources.GetAllocatedSize()
		+ Data->Projectors.GetAllocatedSize()
		+ Data->Layers.GetAllocatedSize()
		+ Data->Instances.GetAllocatedSize()
		+ Data->PendingClusters.GetAllocatedSize()
		+ Data->PendingClusterLookup.GetAllocatedSize()
		+ Data->CustomDataAllocatedSize
		+ (IsValid(RenderHost) ? RenderHost->GetEstimatedCPUAllocatedSize() : 0);
	Stats.PendingInstanceCount = Data->PendingInstanceCount;
	return Stats;
}

UInstancedStaticMeshComponent* UPresentationWorldSubsystem::GetClusterComponent(
	const FMeshPoolClusterKey& Cluster) const
{
	return IsValid(RenderHost) ? RenderHost->GetClusterComponent(Cluster) : nullptr;
}
