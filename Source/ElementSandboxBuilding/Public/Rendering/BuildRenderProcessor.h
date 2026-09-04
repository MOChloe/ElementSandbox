#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "MeshPoolTypes.h"
#include "PresentationViewSource.h"
#include "Rendering/BuildRenderTypes.h"
#include "Templates/UniquePtr.h"

class FBuildEntityRegistry;
class FBuildRenderDirtySet;
class FBuildRenderProcessorData;
class UPresentationWorldSubsystem;

/** Building ECS 到 MeshPool 的差量 Projector；只为当前驻留 Part 保存句柄。 */
class ELEMENTSANDBOXBUILDING_API FBuildRenderProcessor final
{
public:
	explicit FBuildRenderProcessor(
		const FBuildPresentationResidencyConfig& InResidencyConfig = FBuildPresentationResidencyConfig(),
		const FBuildRenderClusterConfig& InClusterConfig = FBuildRenderClusterConfig());
	~FBuildRenderProcessor();

	FBuildRenderProcessor(const FBuildRenderProcessor&) = delete;
	FBuildRenderProcessor& operator=(const FBuildRenderProcessor&) = delete;

	bool Execute(
		const FBuildEntityRegistry& Registry,
		FBuildRenderDirtySet& DirtySet,
		UPresentationWorldSubsystem& Presentation,
		FMeshPoolLayerHandle Layer);

	bool Project(
		const FBuildEntityRegistry& Registry,
		const FPresentationViewSnapshot& Views,
		UPresentationWorldSubsystem& Presentation,
		FMeshPoolLayerHandle Layer);

	bool TryGetInstanceHandle(
		FBuildEntityHandle Entity,
		int32 PartId,
		FMeshPoolInstanceHandle& OutInstance) const;
	bool TryGetPartStorageClass(
		FBuildEntityHandle Entity,
		int32 PartId,
		EBuildRenderStorageClass& OutStorageClass) const;

	bool SetPresentationMotionActive(FBuildEntityHandle Entity, bool bActive);
	bool IsPresentationMotionActive(FBuildEntityHandle Entity) const;
	bool HasPendingProjectionWork() const;
	/** 测试/基准显式 Flush 使用；只让下一次 Project 的 Local 选择同行完成。 */
	void RequestSynchronousLocalSelectionForNextProjection();

	bool ApplyCustomDataChanges(
		const FBuildEntityRegistry& Registry,
		UPresentationWorldSubsystem& Presentation,
		TConstArrayView<FBuildEntityHandle> Entities);
	/** 仅预留轻量索引记录；驻留句柄 Map 仍只按实际 Resident 数增长。 */
	void ReserveEntityCapacity(int32 EntityCapacity);

	int32 GetRenderedEntityCount() const;
	int32 GetRenderedPartCount(FBuildEntityHandle Entity) const;
	bool HasRetiringInstances(FBuildEntityHandle Entity) const;
	void NotifyInstanceRetired(FMeshPoolInstanceHandle Instance);
	double GetStaticRenderCellSize() const;
	SIZE_T GetEstimatedAllocatedSize() const;
	FBuildPresentationSelectionStats GetSelectionStats() const;

#if WITH_DEV_AUTOMATION_TESTS
	/** 自动化测试使用的可注入单调时钟。 */
	void SetResidencyTimeSecondsForTesting(double TimeSeconds);
	/** 自动化测试可让 Local/Far 纯值选择器同行执行；生产构建始终走线程池。 */
	void SetResidencySelectionSynchronousForTesting(bool bSynchronous);
	#endif

private:
	TUniquePtr<FBuildRenderProcessorData> Data;
};
