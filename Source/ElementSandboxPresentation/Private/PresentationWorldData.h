#pragma once

#include "PresentationWorldSubsystem.h"
#include "Containers/SparseArray.h"

/** World 私有数据；观察源与 MeshPool 的稳定 Slot、待提交索引均由所属 Subsystem 持有。 */
class FPresentationWorldData final
{
public:
	struct FSourceSlot final
	{
		FPresentationViewSource View;
		uint32 Generation = 1;
		int32 NextFree = INDEX_NONE;
		bool bAlive = false;
	};

	struct FProjectorSlot final
	{
		FName Name;
		FPresentationProjectorDelegate Delegate;
		uint32 Generation = 1;
		int32 NextFree = INDEX_NONE;
		bool bAlive = false;
	};

	struct FLayerSlot final
	{
		FName Name;
		uint32 Generation = 1;
		int32 NextFree = INDEX_NONE;
		bool bAlive = false;
		bool bReprojectionRequested = false;
	};

	struct FInstanceSlot final
	{
		FMeshPoolClusterKey CurrentCluster;
		FMeshPoolClusterKey DesiredCluster;
		FTransform DesiredTransform = FTransform::Identity;
		TArray<float, TInlineAllocator<4>> DesiredCustomData;
		uint32 Generation = 1;
		int32 NextFree = INDEX_NONE;
		bool bAllocated = false;
		bool bLogicallyAlive = false;
		bool bCommitted = false;
		bool bRemoveQueued = false;
		bool bTransformDirty = false;
		bool bCustomDataDirty = false;
		int32 PendingClusterIndex = INDEX_NONE;
		int32 PreviousPendingInstance = INDEX_NONE;
		int32 NextPendingInstance = INDEX_NONE;
		bool bUrgent = false;
		bool IsQueuedForFlush() const
		{
			return PendingClusterIndex != INDEX_NONE;
		}
		bool bCountedPending = false;
	};

	/**
	 * Instance handle 的 Index 必须长期稳定。单块 TArray 在百万级装填时会周期性搬迁全部
	 * FInstanceSlot，并把一次普通 QueueAdd 放大成几十毫秒尖峰；固定页只分配新页，不搬旧页。
	 */
	class FInstanceSlotPages final
	{
	public:
		static constexpr int32 SlotsPerPage = 1024;

		int32 AddDefaulted()
		{
			check(NumSlots < MAX_int32);
			if ((NumSlots % SlotsPerPage) == 0)
			{
				Pages.Add(MakeUnique<FPage>());
			}
			return NumSlots++;
		}

		bool IsValidIndex(const int32 Index) const
		{
			return Index >= 0 && Index < NumSlots;
		}

		int32 Num() const
		{
			return NumSlots;
		}

		FInstanceSlot& operator[](const int32 Index)
		{
			check(IsValidIndex(Index));
			return Pages[Index / SlotsPerPage]->Slots[Index % SlotsPerPage];
		}

		const FInstanceSlot& operator[](const int32 Index) const
		{
			return const_cast<FInstanceSlotPages&>(*this)[Index];
		}

		SIZE_T GetAllocatedSize() const
		{
			return Pages.GetAllocatedSize() + static_cast<SIZE_T>(Pages.Num()) * sizeof(FPage);
		}

	private:
		struct FPage final
		{
			FInstanceSlot Slots[SlotsPerPage];
		};

		TArray<TUniquePtr<FPage>> Pages;
		int32 NumSlots = 0;
	};

	FPresentationWorldData();
	static void AdvanceGeneration(uint32& Generation);

	FSourceSlot* FindSource(const FPresentationSourceHandle Handle)
	{
		if (!Handle.IsSet() || Handle.GetWorldId() != WorldId || !Sources.IsValidIndex(Handle.GetIndex()))
		{
			return nullptr;
		}
		FSourceSlot& Slot = Sources[Handle.GetIndex()];
		return Slot.bAlive && Slot.Generation == Handle.GetGeneration() ? &Slot : nullptr;
	}

	FProjectorSlot* FindProjector(const FPresentationProjectorHandle Handle)
	{
		if (!Handle.IsSet() || Handle.GetWorldId() != WorldId || !Projectors.IsValidIndex(Handle.GetIndex()))
		{
			return nullptr;
		}
		FProjectorSlot& Slot = Projectors[Handle.GetIndex()];
		return Slot.bAlive && Slot.Generation == Handle.GetGeneration() ? &Slot : nullptr;
	}

	FLayerSlot* FindLayer(const FMeshPoolLayerHandle Handle)
	{
		if (!Handle.IsSet() || Handle.GetWorldId() != WorldId || !Layers.IsValidIndex(Handle.GetIndex()))
		{
			return nullptr;
		}
		FLayerSlot& Slot = Layers[Handle.GetIndex()];
		return Slot.bAlive && Slot.Generation == Handle.GetGeneration() ? &Slot : nullptr;
	}

	const FLayerSlot* FindLayer(const FMeshPoolLayerHandle Handle) const
	{
		return const_cast<FPresentationWorldData*>(this)->FindLayer(Handle);
	}

	FInstanceSlot* FindInstance(const FMeshPoolInstanceHandle Handle, const bool bLogicalOnly = true)
	{
		if (!Handle.IsSet() || Handle.GetPoolId() != PoolId || !Instances.IsValidIndex(Handle.GetIndex()))
		{
			return nullptr;
		}
		FInstanceSlot& Slot = Instances[Handle.GetIndex()];
		return Slot.bAllocated && Slot.Generation == Handle.GetGeneration() && (!bLogicalOnly || Slot.bLogicallyAlive)
		           ? &Slot
		           : nullptr;
	}

	const FInstanceSlot* FindInstance(const FMeshPoolInstanceHandle Handle, const bool bLogicalOnly = true) const
	{
		return const_cast<FPresentationWorldData*>(this)->FindInstance(Handle, bLogicalOnly);
	}

	FMeshPoolInstanceHandle AllocateInstance()
	{
		int32 Index = INDEX_NONE;
		if (FirstFreeInstance != INDEX_NONE)
		{
			Index = FirstFreeInstance;
			FirstFreeInstance = Instances[Index].NextFree;
			Instances[Index].NextFree = INDEX_NONE;
		}
		else
		{
			Index = Instances.AddDefaulted();
		}
		FInstanceSlot& Slot = Instances[Index];
		check(!Slot.bAllocated && Slot.Generation != 0);
		Slot.bAllocated = true;
		Slot.bLogicallyAlive = true;
		return FMeshPoolInstanceHandle(PoolId, Index, Slot.Generation);
	}

	static bool IsPending(const FInstanceSlot& Slot)
	{
		return Slot.bAllocated &&
		       (!Slot.bCommitted || Slot.bRemoveQueued || Slot.CurrentCluster != Slot.DesiredCluster ||
		        Slot.bTransformDirty || Slot.bCustomDataDirty);
	}

	void RefreshPendingState(FInstanceSlot& Slot)
	{
		const bool bShouldCount = IsPending(Slot);
		if (bShouldCount == Slot.bCountedPending)
		{
			return;
		}
		PendingInstanceCount += bShouldCount ? 1 : -1;
		check(PendingInstanceCount >= 0);
		Slot.bCountedPending = bShouldCount;
	}

	/** Dirty Slot 按目标 Cluster 排队；迁移、提级和取消都原地摘链，不留下失效队列项。 */
	void MarkInstanceDirty(int32 Index);
	void MarkInstanceUrgent(int32 Index);
	void RemovePendingInstance(int32 Index);
	/** 每批只取一个 Cluster 的连续待办，Cluster 之间轮转，Urgent 始终先于普通装填。 */
	void SelectPendingBatch(int32 MaximumInstances, TArray<int32>& OutIndices);

	void UpdateCustomDataAllocation(const SIZE_T PreviousSize, const FInstanceSlot& Slot)
	{
		const SIZE_T NewSize = Slot.DesiredCustomData.GetAllocatedSize();
		if (NewSize >= PreviousSize)
		{
			CustomDataAllocatedSize += NewSize - PreviousSize;
		}
		else
		{
			check(CustomDataAllocatedSize >= PreviousSize - NewSize);
			CustomDataAllocatedSize -= PreviousSize - NewSize;
		}
	}

	void ReleaseInstance(int32 Index);

	struct FPendingCluster final
	{
		FMeshPoolClusterKey Key;
		int32 FirstInstance[2] = {INDEX_NONE, INDEX_NONE};
		int32 LastInstance[2] = {INDEX_NONE, INDEX_NONE};
		int32 PreviousReady[2] = {INDEX_NONE, INDEX_NONE};
		int32 NextReady[2] = {INDEX_NONE, INDEX_NONE};
	};

	uint32 WorldId = 0;
	uint32 PoolId = 0;
	bool bEnabled = false;
	bool bProjectionDirty = false;
	bool bDeferredTreeBuildPending = false;
	uint64 SourceRegistryRevision = 1;
	TArray<FSourceSlot> Sources;
	TArray<FProjectorSlot> Projectors;
	TArray<FLayerSlot> Layers;
	FInstanceSlotPages Instances;
	TSparseArray<FPendingCluster> PendingClusters;
	TMap<FMeshPoolClusterKey, int32> PendingClusterLookup;
	int32 FirstReadyCluster[2] = {INDEX_NONE, INDEX_NONE};
	int32 LastReadyCluster[2] = {INDEX_NONE, INDEX_NONE};
	int32 FirstFreeSource = INDEX_NONE;
	int32 FirstFreeProjector = INDEX_NONE;
	int32 FirstFreeLayer = INDEX_NONE;
	int32 FirstFreeInstance = INDEX_NONE;
	int32 SourceCount = 0;
	uint64 FlushCount = 0;
	uint64 ScheduledCycleCount = 0;
	uint64 EmptyFlushCount = 0;
	int32 LastVisitedDirtySlotCount = 0;
	uint64 TotalVisitedDirtySlotCount = 0;
	uint64 FailedFlushCount = 0;
	int32 LastFlushClusterCount = 0;
	int32 LastFlushBatchCount = 0;
	int32 LastFlushInstanceCount = 0;
	int32 MaxFlushClusterCount = 0;
	int32 MaxFlushInstanceCount = 0;
	double LastInstanceApplyMilliseconds = 0.0;
	uint64 TotalFlushedInstanceCount = 0;
	int32 PendingInstanceCount = 0;
	SIZE_T CustomDataAllocatedSize = 0;

private:
	void QueueInstance(int32 Index, bool bUrgent);
	void LinkReadyCluster(int32 ClusterIndex, int32 Priority);
	void UnlinkReadyCluster(int32 ClusterIndex, int32 Priority);
};
