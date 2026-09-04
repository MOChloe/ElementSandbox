#include "PresentationWorldSubsystem.h"

#include "ElementSandboxPresentation.h"
#include "MeshPoolRenderHost.h"
#include "PresentationSettings.h"
#include "PresentationWorldData.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

namespace
{
	struct FMeshPoolClusterPair final
	{
		FMeshPoolClusterKey Source;
		FMeshPoolClusterKey Target;
		friend bool operator==(const FMeshPoolClusterPair& Left, const FMeshPoolClusterPair& Right)
		{
			return Left.Source == Right.Source && Left.Target == Right.Target;
		}
		friend uint32 GetTypeHash(const FMeshPoolClusterPair& Pair)
		{
			return HashCombineFast(GetTypeHash(Pair.Source), GetTypeHash(Pair.Target));
		}
	};

	struct FAddBatch final
	{
		TArray<int32> SlotIndices;
		TArray<FMeshPoolInstanceHandle> Instances;
		TArray<FTransform> Transforms;
		TArray<float> CustomData;
	};

	struct FUpdateBatch final
	{
		TArray<int32> SlotIndices;
		TArray<FMeshPoolInstanceUpdate> Updates;
	};

	struct FCustomBatch final
	{
		TArray<int32> SlotIndices;
		TArray<FMeshPoolCustomDataUpdate> Updates;
	};

	struct FMigrationBatch final
	{
		TArray<int32> SlotIndices;
		TArray<FMeshPoolInstanceUpdate> Updates;
	};

	struct FRemovalBatch final
	{
		TArray<int32> SlotIndices;
		TArray<FMeshPoolInstanceHandle> Instances;
	};

}

bool UPresentationWorldSubsystem::FlushSlots(const TConstArrayView<int32> RestrictToSlots, const bool bForceTreeBuild)
{
	check(IsInGameThread());
	if (!Data || !Data->bEnabled)
	{
		return Data && !Data->bEnabled;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_MeshPool_Flush);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, MeshPoolFlush);
	const UPresentationSettings* Settings = GetDefault<UPresentationSettings>();
	const double StartSeconds = FPlatformTime::Seconds();
	const double BudgetSeconds = FMath::Max(0.0, Settings->InstanceApplyTargetMilliseconds) * 0.001;
	const int32 BatchSize = FMath::Max(1, Settings->MaximumNativeInstanceBatchSize);
	Data->LastVisitedDirtySlotCount = 0;
	Data->LastFlushInstanceCount = 0;
	Data->LastFlushBatchCount = 0;
	Data->LastInstanceApplyMilliseconds = 0.0;
	TSet<FMeshPoolClusterKey> TouchedClusters;
	TArray<int32> Slots;
	bool bSucceeded = true;
	do
	{
		Slots.Reset();
		if (!RestrictToSlots.IsEmpty())
		{
			Slots.Append(RestrictToSlots);
		}
		else
		{
			Data->SelectPendingBatch(BatchSize, Slots);
		}
		if (Slots.IsEmpty())
			break;
		++Data->LastFlushBatchCount;
		bSucceeded = FlushInstanceBatch(Slots, TouchedClusters);
		// 原生调用不可抢占。每帧至少完成一批，之后按本次真实墙钟成本决定是否继续。
		if (!bSucceeded || !RestrictToSlots.IsEmpty())
			break;
	} while (bForceTreeBuild || FPlatformTime::Seconds() - StartSeconds < BudgetSeconds);

	Data->LastFlushClusterCount = TouchedClusters.Num();
	Data->MaxFlushClusterCount = FMath::Max(Data->MaxFlushClusterCount, Data->LastFlushClusterCount);
	Data->MaxFlushInstanceCount = FMath::Max(Data->MaxFlushInstanceCount, Data->LastFlushInstanceCount);
	if (Data->LastFlushInstanceCount > 0)
	{
		++Data->FlushCount;
		Data->LastInstanceApplyMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	}
	else
	{
		++Data->EmptyFlushCount;
	}
	if (IsValid(RenderHost))
	{
		RenderHost->ProcessDeferredTreeBuilds(FPlatformTime::Seconds(), Settings->HISMTreeBuildQuietSeconds,
		                                      Settings->HISMTreeBuildMaxDeferralSeconds, bForceTreeBuild);
		Data->bDeferredTreeBuildPending = RenderHost->HasDeferredTreeBuilds();
	}
	else
	{
		Data->bDeferredTreeBuildPending = false;
	}
	RecordMeshPoolStats();
	return bSucceeded;
}

bool UPresentationWorldSubsystem::FlushInstanceBatch(const TConstArrayView<int32> SlotIndices,
                                                     TSet<FMeshPoolClusterKey>& FrameTouchedClusters)
{
	const int32 VisitsBefore = Data->LastVisitedDirtySlotCount;
	TMap<FMeshPoolClusterKey, FRemovalBatch> Removals;
	TMap<FMeshPoolClusterPair, FMigrationBatch> Migrations;
	TMap<FMeshPoolClusterKey, FAddBatch> Adds;
	TMap<FMeshPoolClusterKey, FUpdateBatch> Updates;
	TMap<FMeshPoolClusterKey, FCustomBatch> CustomUpdates;
	TSet<int32> TouchedSlots;
	TSet<FMeshPoolClusterKey> TouchedClusters;

	for (const int32 SlotIndex : SlotIndices)
	{
		if (!Data->Instances.IsValidIndex(SlotIndex))
		{
			continue;
		}
		FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[SlotIndex];
		if (!Slot.IsQueuedForFlush())
		{
			continue;
		}
		Data->RemovePendingInstance(SlotIndex);
		++Data->LastVisitedDirtySlotCount;
		if (!Slot.bAllocated)
		{
			continue;
		}
		const FMeshPoolInstanceHandle Handle(Data->PoolId, SlotIndex, Slot.Generation);
		if (Slot.bRemoveQueued)
		{
			FRemovalBatch& Batch = Removals.FindOrAdd(Slot.CurrentCluster);
			Batch.SlotIndices.Add(SlotIndex);
			Batch.Instances.Add(Handle);
			TouchedClusters.Add(Slot.CurrentCluster);
			TouchedSlots.Add(SlotIndex);
			continue;
		}
		if (!Slot.bCommitted)
		{
			FAddBatch& Batch = Adds.FindOrAdd(Slot.DesiredCluster);
			Batch.SlotIndices.Add(SlotIndex);
			Batch.Instances.Add(Handle);
			Batch.Transforms.Add(Slot.DesiredTransform);
			Batch.CustomData.Append(Slot.DesiredCustomData);
			TouchedClusters.Add(Slot.DesiredCluster);
			TouchedSlots.Add(SlotIndex);
			continue;
		}
		if (Slot.CurrentCluster != Slot.DesiredCluster)
		{
			FMigrationBatch& Batch = Migrations.FindOrAdd({Slot.CurrentCluster, Slot.DesiredCluster});
			Batch.SlotIndices.Add(SlotIndex);
			FMeshPoolInstanceUpdate& Update = Batch.Updates.AddDefaulted_GetRef();
			Update.Instance = Handle;
			Update.WorldTransform = Slot.DesiredTransform;
			Update.CustomData = Slot.DesiredCustomData;
			TouchedClusters.Add(Slot.CurrentCluster);
			TouchedClusters.Add(Slot.DesiredCluster);
			TouchedSlots.Add(SlotIndex);
			continue;
		}
		if (Slot.bTransformDirty)
		{
			FUpdateBatch& Batch = Updates.FindOrAdd(Slot.CurrentCluster);
			Batch.SlotIndices.Add(SlotIndex);
			FMeshPoolInstanceUpdate& Update = Batch.Updates.AddDefaulted_GetRef();
			Update.Instance = Handle;
			Update.WorldTransform = Slot.DesiredTransform;
			if (Slot.bCustomDataDirty)
			{
				Update.CustomData = Slot.DesiredCustomData;
			}
			TouchedClusters.Add(Slot.CurrentCluster);
			TouchedSlots.Add(SlotIndex);
		}
		else if (Slot.bCustomDataDirty)
		{
			FCustomBatch& Batch = CustomUpdates.FindOrAdd(Slot.CurrentCluster);
			Batch.SlotIndices.Add(SlotIndex);
			FMeshPoolCustomDataUpdate& Update = Batch.Updates.AddDefaulted_GetRef();
			Update.Instance = Handle;
			Update.CustomData = Slot.DesiredCustomData;
			TouchedClusters.Add(Slot.CurrentCluster);
			TouchedSlots.Add(SlotIndex);
		}
	}
	Data->TotalVisitedDirtySlotCount += static_cast<uint64>(Data->LastVisitedDirtySlotCount - VisitsBefore);
	Data->LastFlushInstanceCount += TouchedSlots.Num();
	FrameTouchedClusters.Append(TouchedClusters);
	if (TouchedSlots.IsEmpty())
		return true;
	if (!EnsureRenderHost())
	{
		TSet<FMeshPoolLayerHandle> FailedLayers;
		for (const FMeshPoolClusterKey& Cluster : TouchedClusters)
		{
			FailedLayers.Add(Cluster.Layer);
		}
		++Data->FailedFlushCount;
		RecoverFailedLayers(FailedLayers);
		return false;
	}

	TSet<FMeshPoolLayerHandle> FailedLayers;
	RenderHost->BeginBulkEdit();
	for (TPair<FMeshPoolClusterKey, FRemovalBatch>& Pair : Removals)
	{
		if (!RenderHost->RemoveInstances(Pair.Value.Instances))
		{
			FailedLayers.Add(Pair.Key.Layer);
			continue;
		}
		for (const int32 SlotIndex : Pair.Value.SlotIndices)
		{
			ReleaseInstanceSlot(SlotIndex);
		}
	}
	for (TPair<FMeshPoolClusterPair, FMigrationBatch>& Pair : Migrations)
	{
		if (!RenderHost->MigrateInstances(Pair.Key.Source, Pair.Key.Target, Pair.Value.Updates))
		{
			FailedLayers.Add(Pair.Key.Source.Layer);
			FailedLayers.Add(Pair.Key.Target.Layer);
			continue;
		}
		for (const int32 SlotIndex : Pair.Value.SlotIndices)
		{
			FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[SlotIndex];
			Slot.CurrentCluster = Slot.DesiredCluster;
			Slot.bTransformDirty = false;
			Slot.bCustomDataDirty = false;
			Data->RefreshPendingState(Slot);
		}
	}
	for (TPair<FMeshPoolClusterKey, FAddBatch>& Pair : Adds)
	{
		if (!RenderHost->AddInstances(Pair.Key, Pair.Value.Instances, Pair.Value.Transforms, Pair.Value.CustomData))
		{
			FailedLayers.Add(Pair.Key.Layer);
			continue;
		}
		for (const int32 SlotIndex : Pair.Value.SlotIndices)
		{
			FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[SlotIndex];
			Slot.CurrentCluster = Slot.DesiredCluster;
			Slot.bCommitted = true;
			Slot.bTransformDirty = false;
			Slot.bCustomDataDirty = false;
			Data->RefreshPendingState(Slot);
		}
	}
	for (TPair<FMeshPoolClusterKey, FUpdateBatch>& Pair : Updates)
	{
		if (!RenderHost->UpdateInstances(Pair.Key, Pair.Value.Updates))
		{
			FailedLayers.Add(Pair.Key.Layer);
			continue;
		}
		for (const int32 SlotIndex : Pair.Value.SlotIndices)
		{
			FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[SlotIndex];
			Slot.bTransformDirty = false;
			Slot.bCustomDataDirty = false;
			Data->RefreshPendingState(Slot);
		}
	}
	for (TPair<FMeshPoolClusterKey, FCustomBatch>& Pair : CustomUpdates)
	{
		if (!RenderHost->UpdateCustomData(Pair.Key, Pair.Value.Updates))
		{
			FailedLayers.Add(Pair.Key.Layer);
			continue;
		}
		for (const int32 SlotIndex : Pair.Value.SlotIndices)
		{
			FPresentationWorldData::FInstanceSlot& Slot = Data->Instances[SlotIndex];
			Slot.bCustomDataDirty = false;
			Data->RefreshPendingState(Slot);
		}
	}
	const double CurrentTimeSeconds = FPlatformTime::Seconds();
	const UPresentationSettings* Settings = GetDefault<UPresentationSettings>();
	RenderHost->EndBulkEdit(CurrentTimeSeconds, Settings->HISMSynchronousBuildMaxInstances);

	Data->TotalFlushedInstanceCount += static_cast<uint64>(TouchedSlots.Num());
	if (!FailedLayers.IsEmpty())
	{
		++Data->FailedFlushCount;
		RecoverFailedLayers(FailedLayers);
		return false;
	}
	return true;
}

void UPresentationWorldSubsystem::RecordMeshPoolStats() const
{
	const FMeshPoolStats Stats = GetMeshPoolStats();
	CSV_CUSTOM_STAT(ElementSandboxPresentation, Sample, 1, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, LastFlushBatches, Stats.LastFlushBatchCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, TreeBuildRetries,
	                static_cast<int32>(Stats.HierarchicalTreeBuildRetries), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, ResidentInstances, Stats.ResidentInstanceCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, PendingInstances, Stats.PendingInstanceCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, Clusters, Stats.ClusterCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, LastFlushClusters, Stats.LastFlushClusterCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, LastFlushInstances, Stats.LastFlushInstanceCount,
	                ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, InstanceApplyMilliseconds, Stats.LastInstanceApplyMilliseconds,
	                ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, TreeBuildRequests,
	                static_cast<int32>(FMath::Min<uint64>(Stats.HierarchicalTreeBuildRequests, MAX_int32)),
	                ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, VisitedDirtySlots, Stats.LastVisitedDirtySlotCount,
	                ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(ElementSandboxPresentation, DeferredTreeBuildRequests,
	                static_cast<int32>(FMath::Min<uint64>(Stats.HierarchicalTreeBuildDeferredRequests, MAX_int32)),
	                ECsvCustomStatOp::Set);
}
