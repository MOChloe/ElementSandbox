#pragma once

#include "Collision/BuildCollisionTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Templates/PimplPtr.h"

#include "BuildCollisionHost.generated.h"

class FBuildCollisionHostCluster;
class FBuildCollisionHostData;
class UInstancedStaticMeshComponent;

/** 被动承载近场 Chaos Body 的隐藏 ISM Host，不读取 Registry 或空间索引。 */
UCLASS(Transient, NotBlueprintable)
class ELEMENTSANDBOXBUILDING_API ABuildCollisionHost final : public AActor
{
	GENERATED_BODY()

public:
	ABuildCollisionHost();
	virtual ~ABuildCollisionHost() override;

	bool AddInstances(
		const FBuildCollisionClusterKey& ClusterKey,
		TConstArrayView<FTransform> WorldTransforms,
		TArray<FBuildCollisionInstanceHandle>& OutInstances);
	bool UpdateInstances(
		const FBuildCollisionClusterKey& ClusterKey,
		TConstArrayView<FBuildCollisionInstanceUpdate> Updates);
	bool RemoveInstances(TConstArrayView<FBuildCollisionInstanceHandle> Instances);

	bool IsValidInstance(FBuildCollisionInstanceHandle Instance) const;
	bool TryGetInstanceTransform(
		FBuildCollisionInstanceHandle Instance,
		FTransform& OutWorldTransform) const;
	UInstancedStaticMeshComponent* GetClusterComponent(
		const FBuildCollisionClusterKey& ClusterKey) const;
	int32 GetInstanceCount() const;
	int32 GetClusterCount() const;
	SIZE_T GetEstimatedCPUAllocatedSize() const;
	void ClearInstances();

private:
	FBuildCollisionHostCluster* FindOrAddCluster(
		const FBuildCollisionClusterKey& ClusterKey);
	FBuildCollisionInstanceHandle AllocateInstanceHandle();
	void ReleaseInstanceHandle(int32 SlotIndex);
	void DestroyClusterIfEmpty(const FBuildCollisionClusterKey& ClusterKey);

	TPimplPtr<FBuildCollisionHostData> Data;
};
