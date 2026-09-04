#pragma once

#include "CoreMinimal.h"
#include "MeteorRuntimeTypes.h"

namespace UE::ElementSandbox::Meteor
{
enum class EMeteorClientDebrisState : uint8 { Missing, Prepared, Flying, Settled, Canceled };
struct ELEMENTSANDBOXMETEOR_API FMeteorClientDirectoryEntry final
{
	uint64 PageId = 0;
	uint16 PageLane = MAX_uint16;
	uint32 SegmentRevision = 0;
	FWorldEntityId WorldEntityId;
	EMeteorClientDebrisState State = EMeteorClientDebrisState::Missing;
	double AuthorityStartTimeSeconds = 0.0;
	bool bPresentationQueued = false;
};
/** 两级稠密目录只保存协议状态，不保存逐帧飞行位置。 */
class ELEMENTSANDBOXMETEOR_API FMeteorClientPagedDirectory final
{
public:
	FMeteorClientPagedDirectory() = default;
	FMeteorClientPagedDirectory(const FMeteorClientPagedDirectory&) = delete;
	FMeteorClientPagedDirectory& operator=(const FMeteorClientPagedDirectory&) = delete;
	FMeteorClientDirectoryEntry* Find(uint32 Ordinal);
	const FMeteorClientDirectoryEntry* Find(uint32 Ordinal) const;
	FMeteorClientDirectoryEntry* FindOrAdd(uint32 Ordinal);
	void Reset();
	int32 GetAllocatedPageCount() const { return Pages.Num(); }
	SIZE_T GetAllocatedBytes() const;
private:
	struct alignas(64) FPage { TStaticArray<FMeteorClientDirectoryEntry, ClientDirectoryPageCapacity> Entries; };
	TArray<TUniquePtr<FPage>> Pages;
};
/** 仅生命周期变化发布一次；适配层转换为相对最终实例的 WPO 参数。 */
struct ELEMENTSANDBOXMETEOR_API FMeteorClientPresentationLane final
{
	FMeteorBurstId BurstId;
	uint64 PageId = 0;
	uint32 Revision = 0;
	uint32 Ordinal = MAX_uint32;
	FWorldEntityId WorldEntityId;
	FName RenderArchetypeId;
	EMeteorClientDebrisState State = EMeteorClientDebrisState::Missing;
	FVector StartPosition = FVector::ZeroVector;
	FVector ImpactEndpoint = FVector::ZeroVector;
	FTransform RestTransform = FTransform::Identity;
	FVector3f InitialVelocity = FVector3f::ZeroVector;
	FVector3f Acceleration = FVector3f::ZeroVector;
	FVector3f AngularVelocityDegrees = FVector3f::ZeroVector;
	FQuat4f StartRotation = FQuat4f::Identity;
	float VisualRadius = 0.0f;
	double LocalStartTimeSeconds = 0.0;
	float ImpactDurationSeconds = 0.0f;
	float SettlingDurationSeconds = 0.0f;
	float SettlingLiftHeight = 0.0f;
};
struct ELEMENTSANDBOXMETEOR_API FMeteorClientRuntimeStats final
{
	int32 PreparedPageCount = 0;
	int32 PreparedLaneCount = 0;
	int32 PreparedBacklog = 0;
	int32 ActivationBacklog = 0;
	int32 FlyingLaneCount = 0;
	int32 SettledLaneCount = 0;
	int32 PendingPresentationCount = 0;
	int32 FirstActivationLaneCount = 0;
	int64 TotalActivatedLaneCount = 0;
	double PrepareMilliseconds = 0.0;
	double ActivationMilliseconds = 0.0;
	bool bGameThreadBudgetExhausted = false;
};
/** 处理重复、乱序和取消。Settlement 不依赖显示提交，已落地身份不再起飞。 */
class ELEMENTSANDBOXMETEOR_API FMeteorClientRuntime final
{
public:
	FMeteorClientRuntime() = default;
	FMeteorClientRuntime(const FMeteorClientRuntime&) = delete;
	FMeteorClientRuntime& operator=(const FMeteorClientRuntime&) = delete;
	bool Initialize(const FMeteorRuntimeConfig& Config);
	void Reset();
	bool PrepareTrajectoryPage(TSharedPtr<const FMeteorTrajectoryPage> Page);
	int32 ActivateTrajectoryLanes(uint64 PageId, uint32 Revision, TConstArrayView<uint32> Ordinals, double AuthorityStartTimeSeconds);
	void CancelTrajectoryLanes(uint64 PageId, uint32 Revision, TConstArrayView<uint32> Ordinals);
	void CancelTrajectoryPage(uint64 PageId, uint32 Revision);
	bool HasPreparedPage(uint64 PageId, uint32 Revision) const;
	uint32 GetPreparedPageRevision(uint64 PageId) const;
	bool MarkSettled(uint32 Ordinal, FWorldEntityId WorldEntityId);
	void ConsumePresentationChanges(TArray<FMeteorClientPresentationLane>& Out, double TimeOffsetSeconds);
	bool HasPendingPresentationChanges() const { return !PendingOrdinals.IsEmpty(); }
	void SetPipelineStats(int32 Prepared, int32 Activation, double PrepareMs, double ActivateMs, bool Exhausted);
	const FMeteorClientPagedDirectory& GetDirectory() const { return Directory; }
	const FMeteorClientRuntimeStats& GetStats() const { return Stats; }
private:
	void Queue(uint32 Ordinal);
	void SetState(FMeteorClientDirectoryEntry& Entry, EMeteorClientDebrisState State);
	FMeteorClientPagedDirectory Directory;
	TMap<uint64, TSharedPtr<const FMeteorTrajectoryPage>> PreparedPages;
	TMap<uint64, uint32> PageRevisions;
	TMap<uint64, uint32> CanceledPages;
	TArray<uint32> PendingOrdinals;
	FMeteorClientRuntimeStats Stats;
	bool bInitialized = false;
};
}
