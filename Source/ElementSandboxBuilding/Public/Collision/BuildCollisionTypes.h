#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildCollisionPartDefinition.h"

class ABuildCollisionHost;
class FBuildCollisionProcessor;
class UStaticMesh;

/** Chaos ISM Cluster 的完整身份；Chunk 不是 Cluster Key 的一部分。 */
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionClusterKey final
{
	UStaticMesh* Mesh = nullptr;
	EBuildCollisionMobility Mobility = EBuildCollisionMobility::Static;
	FName CollisionProfileName = NAME_None;

	bool IsSet() const
	{
		return Mesh != nullptr && !CollisionProfileName.IsNone();
	}

	friend bool operator==(
		const FBuildCollisionClusterKey& Left,
		const FBuildCollisionClusterKey& Right)
	{
		return Left.Mesh == Right.Mesh
			&& Left.Mobility == Right.Mobility
			&& Left.CollisionProfileName == Right.CollisionProfileName;
	}

	friend uint32 GetTypeHash(const FBuildCollisionClusterKey& Key)
	{
		return HashCombineFast(
			HashCombineFast(
				PointerHash(Key.Mesh),
				GetTypeHash(static_cast<uint8>(Key.Mobility))),
			GetTypeHash(Key.CollisionProfileName));
	}
};

/** Host 内部 InstanceIndex 被 Swap-Remove 改写时仍保持有效。 */
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionInstanceHandle final
{
public:
	FBuildCollisionInstanceHandle() = default;

	bool IsSet() const
	{
		return HostId != 0 && Index != INDEX_NONE && Generation != 0;
	}

	int32 GetIndex() const { return Index; }
	uint32 GetGeneration() const { return Generation; }
	uint32 GetHostId() const { return HostId; }

	friend bool operator==(
		const FBuildCollisionInstanceHandle& Left,
		const FBuildCollisionInstanceHandle& Right)
	{
		return Left.HostId == Right.HostId
			&& Left.Index == Right.Index
			&& Left.Generation == Right.Generation;
	}

	friend uint32 GetTypeHash(const FBuildCollisionInstanceHandle& Handle)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Handle.HostId), GetTypeHash(Handle.Index)),
			GetTypeHash(Handle.Generation));
	}

private:
	FBuildCollisionInstanceHandle(
		const uint32 InHostId,
		const int32 InIndex,
		const uint32 InGeneration)
		: HostId(InHostId)
		, Index(InIndex)
		, Generation(InGeneration)
	{
	}

	uint32 HostId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;

	friend ABuildCollisionHost;
};

struct ELEMENTSANDBOXBUILDING_API FBuildCollisionInstanceUpdate final
{
	FBuildCollisionInstanceHandle Instance;
	FTransform WorldTransform = FTransform::Identity;
};

/** 最近一次 Collision Projection 在哪一层失败；仅用于诊断，不参与资格判断。 */
enum class EBuildCollisionProjectionFailure : uint8
{
	None,
	MissingDefinitionOrTransform,
	InvalidCollisionPart,
	InvalidClusterConfiguration,
	InvalidCollisionGeometry,
	HostRemoveFailed,
	HostAddFailed,
	HostUpdateFailed,
	ProcessorStateInvalid
};

/** Processor 对单个稳定 Collision Part 的只读诊断快照。 */
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionPartProjectionState final
{
	bool bRequired = false;
	bool bRetained = false;
	bool bSelectionPending = false;
	bool bPendingPrefetch = false;
	FBuildCollisionClusterKey ClusterKey;
	FBuildCollisionInstanceHandle Instance;
	EBuildCollisionProjectionFailure LastFailure =
		EBuildCollisionProjectionFailure::None;
};

/** 一个 World 中 Collision Source 的 Generation Handle。 */
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionSourceHandle final
{
public:
	FBuildCollisionSourceHandle() = default;

	bool IsSet() const
	{
		return RuntimeId != 0 && Index != INDEX_NONE && Generation != 0;
	}

	int32 GetIndex() const { return Index; }
	uint32 GetGeneration() const { return Generation; }
	uint32 GetRuntimeId() const { return RuntimeId; }

	friend bool operator==(
		const FBuildCollisionSourceHandle& Left,
		const FBuildCollisionSourceHandle& Right)
	{
		return Left.RuntimeId == Right.RuntimeId
			&& Left.Index == Right.Index
			&& Left.Generation == Right.Generation;
	}

private:
	FBuildCollisionSourceHandle(
		const uint32 InRuntimeId,
		const int32 InIndex,
		const uint32 InGeneration)
		: RuntimeId(InRuntimeId)
		, Index(InIndex)
		, Generation(InGeneration)
	{
	}

	uint32 RuntimeId = 0;
	int32 Index = INDEX_NONE;
	uint32 Generation = 0;

	friend FBuildCollisionProcessor;
};

/** 一个玩家/Pawn 提交给 Building Collision Projector 的纯观察数据。 */
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionSource final
{
	/** 角色或其他需要近场阻挡的主体位置；相机位置不能替代它。 */
	FVector SubjectLocation = FVector::ZeroVector;

	/** 当前世界速度，用于对 Prefetch 候选按预计接触时间排序。 */
	FVector Velocity = FVector::ZeroVector;

	/** 必须在下一次移动前具备碰撞的主体近场。 */
	FBox ImmediateBounds = FBox(ForceInit);

	/** 沿当前运动方向提前加载的区域。 */
	FBox PrefetchBounds = FBox(ForceInit);

	/** 第三人称镜头与 Subject 之间必须立即阻挡的区域。 */
	FBox CameraBounds = FBox(ForceInit);

	/** 离开 Required 后仍允许缓存 Body 的空间滞回区域。 */
	FBox RetentionBounds = FBox(ForceInit);

	/** 提交方的数据版本；0 保留为无效值。 */
	uint64 Revision = 0;

	bool IsValid() const;
};

/**
 * Building Collision 的局部驻留策略。Chunk 只作为 FBuildSpatialIndex 内部宽相，
 * 不再参与 Collision Source、Body 生命周期或预算。
 */
struct ELEMENTSANDBOXBUILDING_API FBuildCollisionActivationConfig final
{
	double MinimumBlockingRadius = 400.0;
	double PredictionHorizonSeconds = 1.0;
	double MovementSafetyPadding = 100.0;
	double RetentionPadding = 300.0;
	double EvictionGraceSeconds = 3.0;
	double SourceMoveThreshold = 50.0;
	double SourceDirectionThresholdDegrees = 15.0;
	double SourceSpeedThreshold = 100.0;
	int32 MaxPrefetchAddsPerFrame = 16;
	int32 MaxRemovesPerFrame = 32;

	bool IsValid() const
	{
		return FMath::IsFinite(MinimumBlockingRadius)
			&& MinimumBlockingRadius > 0.0
			&& FMath::IsFinite(PredictionHorizonSeconds)
			&& PredictionHorizonSeconds >= 0.0
			&& FMath::IsFinite(MovementSafetyPadding)
			&& MovementSafetyPadding >= 0.0
			&& FMath::IsFinite(RetentionPadding)
			&& RetentionPadding >= 0.0
			&& FMath::IsFinite(EvictionGraceSeconds)
			&& EvictionGraceSeconds >= 0.0
			&& FMath::IsFinite(SourceMoveThreshold)
			&& SourceMoveThreshold >= 0.0
			&& FMath::IsFinite(SourceDirectionThresholdDegrees)
			&& SourceDirectionThresholdDegrees >= 0.0
			&& SourceDirectionThresholdDegrees <= 180.0
			&& FMath::IsFinite(SourceSpeedThreshold)
			&& SourceSpeedThreshold >= 0.0
			&& MaxPrefetchAddsPerFrame > 0
			&& MaxRemovesPerFrame > 0;
	}
};
