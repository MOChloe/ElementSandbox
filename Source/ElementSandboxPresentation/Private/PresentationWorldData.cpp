#include "PresentationWorldData.h"

namespace
{
	uint32 GNextPresentationWorldId = 0;
	uint32 GNextMeshPoolId = 0;
}

FPresentationWorldData::FPresentationWorldData()
{
	check(IsInGameThread());
	AdvanceGeneration(GNextPresentationWorldId);
	AdvanceGeneration(GNextMeshPoolId);
	WorldId = GNextPresentationWorldId;
	PoolId = GNextMeshPoolId;
}

void FPresentationWorldData::AdvanceGeneration(uint32& Generation)
{
	if (++Generation == 0)
		++Generation;
}

void FPresentationWorldData::LinkReadyCluster(const int32 ClusterIndex, const int32 Priority)
{
	FPendingCluster& Cluster = PendingClusters[ClusterIndex];
	Cluster.PreviousReady[Priority] = LastReadyCluster[Priority];
	Cluster.NextReady[Priority] = INDEX_NONE;
	if (LastReadyCluster[Priority] != INDEX_NONE)
	{
		PendingClusters[LastReadyCluster[Priority]].NextReady[Priority] = ClusterIndex;
	}
	else
	{
		FirstReadyCluster[Priority] = ClusterIndex;
	}
	LastReadyCluster[Priority] = ClusterIndex;
}

void FPresentationWorldData::UnlinkReadyCluster(const int32 ClusterIndex, const int32 Priority)
{
	FPendingCluster& Cluster = PendingClusters[ClusterIndex];
	if (Cluster.PreviousReady[Priority] != INDEX_NONE)
	{
		PendingClusters[Cluster.PreviousReady[Priority]].NextReady[Priority] = Cluster.NextReady[Priority];
	}
	else
	{
		FirstReadyCluster[Priority] = Cluster.NextReady[Priority];
	}
	if (Cluster.NextReady[Priority] != INDEX_NONE)
	{
		PendingClusters[Cluster.NextReady[Priority]].PreviousReady[Priority] = Cluster.PreviousReady[Priority];
	}
	else
	{
		LastReadyCluster[Priority] = Cluster.PreviousReady[Priority];
	}
	Cluster.PreviousReady[Priority] = INDEX_NONE;
	Cluster.NextReady[Priority] = INDEX_NONE;
}

void FPresentationWorldData::QueueInstance(const int32 Index, const bool bUrgent)
{
	FInstanceSlot& Slot = Instances[Index];
	const FMeshPoolClusterKey& Key = Slot.bRemoveQueued ? Slot.CurrentCluster : Slot.DesiredCluster;
	if (Slot.IsQueuedForFlush())
	{
		if (PendingClusters[Slot.PendingClusterIndex].Key == Key && Slot.bUrgent == bUrgent)
			return;
		RemovePendingInstance(Index);
	}
	int32 ClusterIndex;
	if (const int32* Existing = PendingClusterLookup.Find(Key))
	{
		ClusterIndex = *Existing;
	}
	else
	{
		FPendingCluster Cluster;
		Cluster.Key = Key;
		ClusterIndex = PendingClusters.Add(MoveTemp(Cluster));
		PendingClusterLookup.Add(Key, ClusterIndex);
	}
	FPendingCluster& Cluster = PendingClusters[ClusterIndex];
	const int32 Priority = bUrgent ? 1 : 0;
	Slot.PendingClusterIndex = ClusterIndex;
	Slot.bUrgent = bUrgent;
	Slot.PreviousPendingInstance = Cluster.LastInstance[Priority];
	Slot.NextPendingInstance = INDEX_NONE;
	if (Cluster.LastInstance[Priority] != INDEX_NONE)
	{
		Instances[Cluster.LastInstance[Priority]].NextPendingInstance = Index;
	}
	else
	{
		Cluster.FirstInstance[Priority] = Index;
		LinkReadyCluster(ClusterIndex, Priority);
	}
	Cluster.LastInstance[Priority] = Index;
}

void FPresentationWorldData::MarkInstanceDirty(const int32 Index)
{
	QueueInstance(Index, Instances[Index].bUrgent);
	RefreshPendingState(Instances[Index]);
}

void FPresentationWorldData::MarkInstanceUrgent(const int32 Index)
{
	check(Instances[Index].IsQueuedForFlush());
	QueueInstance(Index, true);
}

void FPresentationWorldData::RemovePendingInstance(const int32 Index)
{
	FInstanceSlot& Slot = Instances[Index];
	if (!Slot.IsQueuedForFlush())
		return;
	const int32 ClusterIndex = Slot.PendingClusterIndex;
	FPendingCluster& Cluster = PendingClusters[ClusterIndex];
	const int32 Priority = Slot.bUrgent ? 1 : 0;
	if (Slot.PreviousPendingInstance != INDEX_NONE)
	{
		Instances[Slot.PreviousPendingInstance].NextPendingInstance = Slot.NextPendingInstance;
	}
	else
	{
		Cluster.FirstInstance[Priority] = Slot.NextPendingInstance;
	}
	if (Slot.NextPendingInstance != INDEX_NONE)
	{
		Instances[Slot.NextPendingInstance].PreviousPendingInstance = Slot.PreviousPendingInstance;
	}
	else
	{
		Cluster.LastInstance[Priority] = Slot.PreviousPendingInstance;
	}
	if (Cluster.FirstInstance[Priority] == INDEX_NONE)
	{
		UnlinkReadyCluster(ClusterIndex, Priority);
	}
	Slot.PendingClusterIndex = INDEX_NONE;
	Slot.PreviousPendingInstance = INDEX_NONE;
	Slot.NextPendingInstance = INDEX_NONE;
	Slot.bUrgent = false;
	if (Cluster.FirstInstance[0] == INDEX_NONE && Cluster.FirstInstance[1] == INDEX_NONE)
	{
		PendingClusterLookup.Remove(Cluster.Key);
		PendingClusters.RemoveAt(ClusterIndex);
	}
}

void FPresentationWorldData::SelectPendingBatch(const int32 MaximumInstances, TArray<int32>& OutIndices)
{
	const int32 Priority = FirstReadyCluster[1] != INDEX_NONE ? 1 : 0;
	const int32 ClusterIndex = FirstReadyCluster[Priority];
	if (ClusterIndex == INDEX_NONE)
		return;
	const FPendingCluster& Cluster = PendingClusters[ClusterIndex];
	for (int32 Index = Cluster.FirstInstance[Priority]; Index != INDEX_NONE && OutIndices.Num() < MaximumInstances;
	     Index = Instances[Index].NextPendingInstance)
	{
		OutIndices.Add(Index);
	}
	// 大 Cluster 不长期占据队首；同一优先级的其他 Cluster 可以在下一批继续推进。
	if (Cluster.NextReady[Priority] != INDEX_NONE)
	{
		UnlinkReadyCluster(ClusterIndex, Priority);
		LinkReadyCluster(ClusterIndex, Priority);
	}
}

void FPresentationWorldData::ReleaseInstance(const int32 Index)
{
	check(Instances.IsValidIndex(Index) && Instances[Index].bAllocated);
	FInstanceSlot& Slot = Instances[Index];
	RemovePendingInstance(Index);
	if (Slot.bCountedPending)
	{
		check(PendingInstanceCount > 0);
		--PendingInstanceCount;
	}
	const SIZE_T CustomDataSize = Slot.DesiredCustomData.GetAllocatedSize();
	check(CustomDataAllocatedSize >= CustomDataSize);
	CustomDataAllocatedSize -= CustomDataSize;
	uint32 NextGeneration = Slot.Generation;
	AdvanceGeneration(NextGeneration);
	Slot = FInstanceSlot();
	Slot.Generation = NextGeneration;
	Slot.NextFree = FirstFreeInstance;
	FirstFreeInstance = Index;
}
