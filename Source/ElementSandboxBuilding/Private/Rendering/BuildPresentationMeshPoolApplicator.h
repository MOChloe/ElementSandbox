#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "MeshPoolTypes.h"
#include "Rendering/BuildRenderTypes.h"

class FBuildEntityRegistry;
class UBuildingDefinition;
class UPresentationWorldSubsystem;

struct FBuildPresentationAppliedPart final
{
	int32 PartId = INDEX_NONE;
	FMeshPoolInstanceHandle Instance;
	FMeshPoolClusterKey Cluster;
	EBuildRenderStorageClass StorageClass = EBuildRenderStorageClass::StaticHISM;
};

/** Game Thread 上唯一负责把已解析 Building Part 差量写入 MeshPool 的私有边界。 */
class FBuildPresentationMeshPoolApplicator final
{
public:
	static int32 CountMeshParts(const UBuildingDefinition& Definition);
	static int32 CountPromotableMeshParts(const UBuildingDefinition& Definition);

	/**
	 * 从 NextPartId 开始只装填有限数量的有效 Mesh Part。调用方跨帧保存 Cursor 与已应用 Part；
	 * 单次失败只回滚本 Slice，不能让一个数百 Part 的 Entity 穿透 GameThread 时间预算。
	 */
	static bool QueueAddEntitySlice(
		const FBuildEntityRegistry& Registry,
		UPresentationWorldSubsystem& Presentation,
		FMeshPoolLayerHandle Layer,
		FBuildEntityHandle Entity,
		bool bHot,
		const FBuildPresentationResidencyConfig& ResidencyConfig,
		const FBuildRenderClusterConfig& ClusterConfig,
		int32 NextPartId,
		int32 MaximumMeshParts,
		double DeadlineSeconds,
		int32& OutNextPartId,
		bool& OutComplete,
		TArray<FBuildPresentationAppliedPart>& OutParts);

	static bool QueueRemoveEntity(
		UPresentationWorldSubsystem& Presentation,
		TConstArrayView<FBuildPresentationAppliedPart> Parts);

	static bool QueueUpdateEntity(
		const FBuildEntityRegistry& Registry,
		UPresentationWorldSubsystem& Presentation,
		FMeshPoolLayerHandle Layer,
		FBuildEntityHandle Entity,
		bool bHot,
		const FBuildPresentationResidencyConfig& ResidencyConfig,
		const FBuildRenderClusterConfig& ClusterConfig,
		TConstArrayView<int32> RequestedParts,
		TArrayView<FBuildPresentationAppliedPart> AppliedParts);

	static bool QueueStorageMigration(
		const FBuildEntityRegistry& Registry,
		UPresentationWorldSubsystem& Presentation,
		FMeshPoolLayerHandle Layer,
		FBuildEntityHandle Entity,
		bool bHot,
		const FBuildPresentationResidencyConfig& ResidencyConfig,
		const FBuildRenderClusterConfig& ClusterConfig,
		TArrayView<FBuildPresentationAppliedPart> AppliedParts);

	static bool QueueCustomData(
		const FBuildEntityRegistry& Registry,
		UPresentationWorldSubsystem& Presentation,
		FBuildEntityHandle Entity,
		TConstArrayView<FBuildPresentationAppliedPart> AppliedParts);
};
