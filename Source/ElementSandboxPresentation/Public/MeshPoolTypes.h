#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class FPresentationWorldData;
class UPresentationWorldSubsystem;
class UStaticMesh;

enum class EMeshPoolBackend : uint8
{
	HierarchicalStatic,
	ImmediateMovable
};

struct ELEMENTSANDBOXPRESENTATION_API FMeshPoolLayerHandle final
{
  public:
	FMeshPoolLayerHandle() = default;

	bool IsSet() const
	{
		return WorldId != 0 && Index != INDEX_NONE && Generation != 0;
	}
	int32 GetIndex() const
	{
		return Index;
	}
	uint32 GetGeneration() const
	{
		return Generation;
	}
	uint32 GetWorldId() const
	{
		return WorldId;
	}

	friend bool operator==(const FMeshPoolLayerHandle& Left, const FMeshPoolLayerHandle& Right)
	{
		return Left.WorldId == Right.WorldId && Left.Index == Right.Index && Left.Generation == Right.Generation;
	}
	friend bool operator!=(const FMeshPoolLayerHandle& Left, const FMeshPoolLayerHandle& Right)
	{
		return !(Left == Right);
	}
	friend uint32 GetTypeHash(const FMeshPoolLayerHandle& Handle)
	{
		return HashCombineFast(HashCombineFast(Handle.WorldId, GetTypeHash(Handle.Index)), Handle.Generation);
	}

  private:
	FMeshPoolLayerHandle(uint32 InWorldId, int32 InIndex, uint32 InGeneration)
		: WorldId(InWorldId), Index(InIndex), Generation(InGeneration)
	{
	}

	uint32 WorldId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;
	friend FPresentationWorldData;
	friend UPresentationWorldSubsystem;
};

/** Pool 逻辑句柄；Pending Add 阶段即有效，绝不暴露引擎 InstanceIndex。 */
struct ELEMENTSANDBOXPRESENTATION_API FMeshPoolInstanceHandle final
{
  public:
	FMeshPoolInstanceHandle() = default;

	bool IsSet() const
	{
		return PoolId != 0 && Index != INDEX_NONE && Generation != 0;
	}
	int32 GetIndex() const
	{
		return Index;
	}
	uint32 GetGeneration() const
	{
		return Generation;
	}
	uint32 GetPoolId() const
	{
		return PoolId;
	}

	friend bool operator==(const FMeshPoolInstanceHandle& Left, const FMeshPoolInstanceHandle& Right)
	{
		return Left.PoolId == Right.PoolId && Left.Index == Right.Index && Left.Generation == Right.Generation;
	}
	friend bool operator!=(const FMeshPoolInstanceHandle& Left, const FMeshPoolInstanceHandle& Right)
	{
		return !(Left == Right);
	}
	friend uint32 GetTypeHash(const FMeshPoolInstanceHandle& Handle)
	{
		return HashCombineFast(HashCombineFast(Handle.PoolId, GetTypeHash(Handle.Index)), Handle.Generation);
	}

  private:
	FMeshPoolInstanceHandle(uint32 InPoolId, int32 InIndex, uint32 InGeneration)
		: PoolId(InPoolId), Index(InIndex), Generation(InGeneration)
	{
	}

	uint32 PoolId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;
	friend FPresentationWorldData;
	friend UPresentationWorldSubsystem;
};

/** Cluster 的完整物理身份；阴影不进入 Key，因为本轮所有 Cluster 固定无阴影。 */
struct ELEMENTSANDBOXPRESENTATION_API FMeshPoolClusterKey final
{
	FMeshPoolLayerHandle Layer;
	FIntVector Cell = FIntVector::ZeroValue;
	UStaticMesh* Mesh = nullptr;
	UMaterialInterface* MaterialOverride = nullptr;
	EMeshPoolBackend Backend = EMeshPoolBackend::HierarchicalStatic;
	int32 CustomDataFloatCount = 0;

	bool IsSet() const
	{
		return Layer.IsSet() && Mesh && CustomDataFloatCount >= 0 && CustomDataFloatCount <= 8;
	}
	friend bool operator==(const FMeshPoolClusterKey& Left, const FMeshPoolClusterKey& Right)
	{
		return Left.Layer == Right.Layer && Left.Cell == Right.Cell && Left.Mesh == Right.Mesh &&
			   Left.MaterialOverride == Right.MaterialOverride && Left.Backend == Right.Backend &&
			   Left.CustomDataFloatCount == Right.CustomDataFloatCount;
	}
	friend uint32 GetTypeHash(const FMeshPoolClusterKey& Key)
	{
		uint32 Hash = HashCombineFast(GetTypeHash(Key.Layer), GetTypeHash(Key.Cell));
		Hash = HashCombineFast(Hash, PointerHash(Key.Mesh));
		Hash = HashCombineFast(Hash, PointerHash(Key.MaterialOverride));
		Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(Key.Backend)));
		return HashCombineFast(Hash, GetTypeHash(Key.CustomDataFloatCount));
	}
};

struct ELEMENTSANDBOXPRESENTATION_API FMeshPoolInstanceUpdate final
{
	FMeshPoolInstanceHandle Instance;
	FTransform WorldTransform = FTransform::Identity;
	TArray<float, TInlineAllocator<4>> CustomData;
};

struct ELEMENTSANDBOXPRESENTATION_API FMeshPoolCustomDataUpdate final
{
	FMeshPoolInstanceHandle Instance;
	TArray<float, TInlineAllocator<4>> CustomData;
};

struct ELEMENTSANDBOXPRESENTATION_API FMeshPoolStats final
{
	int32 ResidentInstanceCount = 0;
	int32 PendingInstanceCount = 0;
	int32 ClusterCount = 0;
	int32 LastFlushClusterCount = 0;
	int32 LastFlushBatchCount = 0;
	int32 LastFlushInstanceCount = 0;
	int32 MaxFlushClusterCount = 0;
	int32 MaxFlushInstanceCount = 0;
	/** 最近一次非空 Flush 的实例 Remove/Migrate/Add/Update 应用耗时；不包含 HISM Tree 调度。 */
	double LastInstanceApplyMilliseconds = 0.0;
	uint64 FlushCount = 0;
	uint64 TotalFlushedInstanceCount = 0;
	uint64 ScheduledCycleCount = 0;
	uint64 EmptyFlushCount = 0;
	int32 LastVisitedDirtySlotCount = 0;
	uint64 TotalVisitedDirtySlotCount = 0;
	uint64 HierarchicalTreeBuildRequests = 0;
	/** 已启动构建因并发修改而发生的 UE 内部重试，不含外部调度启动。 */
	uint64 HierarchicalTreeBuildRetries = 0;
	uint64 HierarchicalTreeBuildDeferredRequests = 0;
	uint64 HierarchicalTreeBuildCoalescedRequests = 0;
	uint64 FailedFlushCount = 0;
	SIZE_T EstimatedCPUAllocatedSize = 0;
};
