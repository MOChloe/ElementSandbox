#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPoolTypes.h"
#include "Templates/PimplPtr.h"

#include "MeshPoolRenderHost.generated.h"

class FMeshPoolRenderHostData;
class UInstancedStaticMeshComponent;

/** MeshPool 私有物理提交端；Entity 与 Gameplay 身份永远不会进入这里。 */
UCLASS(Transient, NotBlueprintable)
class ELEMENTSANDBOXPRESENTATION_API AMeshPoolRenderHost final : public AActor
{
	GENERATED_BODY()

  public:
	AMeshPoolRenderHost();
	virtual ~AMeshPoolRenderHost() override;

	void BeginBulkEdit();
	void EndBulkEdit(double CurrentTimeSeconds, int32 SynchronousTreeBuildMaxInstances);
	void ProcessDeferredTreeBuilds(double CurrentTimeSeconds, double QuietSeconds, double MaxDeferralSeconds,
								   bool bForce);
	bool HasDeferredTreeBuilds() const;
	bool AddInstances(const FMeshPoolClusterKey& Cluster, TConstArrayView<FMeshPoolInstanceHandle> Instances,
					  TConstArrayView<FTransform> WorldTransforms, TConstArrayView<float> FlattenedCustomData = {});
	bool UpdateInstances(const FMeshPoolClusterKey& Cluster, TConstArrayView<FMeshPoolInstanceUpdate> Updates);
	bool UpdateCustomData(const FMeshPoolClusterKey& Cluster, TConstArrayView<FMeshPoolCustomDataUpdate> Updates);
	bool MigrateInstances(const FMeshPoolClusterKey& Source, const FMeshPoolClusterKey& Target,
							  TConstArrayView<FMeshPoolInstanceUpdate> Updates);
	bool RemoveInstances(TConstArrayView<FMeshPoolInstanceHandle> Instances);
	void ClearLayer(FMeshPoolLayerHandle Layer);
	void ClearAll();

	bool IsValidInstance(FMeshPoolInstanceHandle Instance) const;
	bool TryGetInstanceTransform(FMeshPoolInstanceHandle Instance, FTransform& OutTransform) const;
	int32 GetInstanceCount() const;
	int32 GetClusterCount() const;
	uint64 GetHierarchicalTreeBuildRequestCount() const;
	uint64 GetHierarchicalTreeBuildRetryCount() const;
	uint64 GetHierarchicalTreeBuildDeferredRequestCount() const;
	uint64 GetHierarchicalTreeBuildCoalescedRequestCount() const;
	SIZE_T GetEstimatedCPUAllocatedSize() const;
	UInstancedStaticMeshComponent* GetClusterComponent(const FMeshPoolClusterKey& Cluster) const;

  private:
	void MarkClusterDirty(const FMeshPoolClusterKey& Cluster);
	void DestroyClusterIfEmpty(const FMeshPoolClusterKey& Cluster);
	TPimplPtr<FMeshPoolRenderHostData> Data;
};
