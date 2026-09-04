#pragma once

#include "Collision/BuildCollisionTypes.h"
#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Templates/UniquePtr.h"

class ABuildCollisionHost;
class FBuildCollisionProcessorData;
class FBuildEntityRegistry;

/**
 * 按 Source 的精确局部 Bounds 把 Collision Part 投影到隐藏 ISM Host。
 * RequiredNow 负责补齐，ResidentCache 负责滞回；空间 Chunk 从不等同于碰撞工作集。
 */
class ELEMENTSANDBOXBUILDING_API FBuildCollisionProcessor final
{
	public:
	explicit FBuildCollisionProcessor(
			const FBuildCollisionActivationConfig& InActivationConfig =
				FBuildCollisionActivationConfig());
	~FBuildCollisionProcessor();

	FBuildCollisionProcessor(const FBuildCollisionProcessor&) = delete;
	FBuildCollisionProcessor& operator=(const FBuildCollisionProcessor&) = delete;

	FBuildCollisionSourceHandle RegisterSource(const FBuildCollisionSource& Source);
	bool UpdateSource(
		FBuildCollisionSourceHandle Source,
		const FBuildCollisionSource& SourceData);
	bool UnregisterSource(FBuildCollisionSourceHandle Source);
	bool IsValidSource(FBuildCollisionSourceHandle Source) const;

	/** Create/Commit 只在当前活跃工作集相关时留下 Dirty。 */
	void NotifyEntityCreated(
			FBuildEntityHandle Entity,
			const FBuildSpatialIndex& SpatialIndex);
	void NotifyEntityDestroyed(FBuildEntityHandle Entity);
	void NotifyEntityTransformChanged(
		FBuildEntityHandle Entity,
		const FBuildSpatialIndex& SpatialIndex);
	void NotifyPartTransformsChanged(
		FBuildEntityHandle Entity,
		TConstArrayView<int32> MeshPartIds,
		const FBuildEntityRegistry& Registry,
		const FBuildSpatialIndex& SpatialIndex);

	/** 只在有 Source 差量、Dirty、预取或到期淘汰工作时执行。 */
	bool Execute(
		const FBuildEntityRegistry& Registry,
		const FBuildSpatialIndex& SpatialIndex,
		ABuildCollisionHost& CollisionHost,
		double CurrentTimeSeconds);

	bool HasPendingWork() const;
	int32 GetSourceCount() const;
	int32 GetProjectedEntityCount() const;
	int32 GetProjectedPartCount(FBuildEntityHandle Entity) const;
	int32 GetRequiredPartCount() const;
	int32 GetCachedOnlyPartCount() const;
	int32 GetPendingPrefetchAddCount() const;
	int32 GetEvictionCandidateCount() const;
	int32 GetLastQueriedEntityCount() const;
	int32 GetLastInspectedPartCount() const;
	int32 GetLastChangedPartCount() const;
	FBuildCollisionActivationConfig GetActivationConfig() const;
	SIZE_T GetEstimatedAllocatedSize() const;
		bool TryGetInstanceHandle(
			FBuildEntityHandle Entity,
			int32 CollisionPartId,
			FBuildCollisionInstanceHandle& OutInstance) const;
		bool GetPartProjectionState(
			FBuildEntityHandle Entity,
			int32 CollisionPartId,
			FBuildCollisionPartProjectionState& OutState) const;

private:
	void RequestFullReprojection(ABuildCollisionHost& CollisionHost);

	TUniquePtr<FBuildCollisionProcessorData> Data;
};
