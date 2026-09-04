#pragma once

#include "CoreMinimal.h"

#include "WorldObjectRuntimeStats.generated.h"

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectRuntimeStats final
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere)
	int32 EntityCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 PermanentStaticCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 PortableCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 ActiveCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 ActorActiveCount = 0;

	/** 当前仍绑定 Actor/Chaos Proxy 的 Entity 数；Dormant 自动物理对象必须为零。 */
	UPROPERTY(VisibleAnywhere)
	int32 BoundProxyCount = 0;

	/** 当前由 WorldStorage Chunk 唯一持久化的 WorldObject 数量。 */
	UPROPERTY(VisibleAnywhere)
	int32 WorldStorageOwnedEntityCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 LastSampledActiveCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 LastChangedTransformCount = 0;

	UPROPERTY(VisibleAnywhere)
	int64 StaticBuildCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 StaticLinearChunkCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 StaticBVHChunkCount = 0;

	UPROPERTY(VisibleAnywhere)
	int64 DynamicReinsertCount = 0;

	UPROPERTY(VisibleAnywhere)
	int64 RegistryAllocatedBytes = 0;

	UPROPERTY(VisibleAnywhere)
	int64 SpatialAllocatedBytes = 0;

	UPROPERTY(VisibleAnywhere)
	int64 EstimatedAllocatedBytes = 0;

	UPROPERTY(VisibleAnywhere)
	int32 FragmentPoolCount = 0;

	UPROPERTY(VisibleAnywhere)
	int32 FragmentSparseIndexPageCount = 0;
};
