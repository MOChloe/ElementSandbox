#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectEntityHandle.h"

#include "WorldObjectCollisionTypes.generated.h"

/** 普通 Dormant WorldObject 的角色近场碰撞参数；与服务器 Authority 8 Hz 无关。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectCollisionActivationConfig final
{
	double ImmediateCollisionRadius = 400.0;
	double CameraCorridorPadding = 100.0;
	double PredictionSeconds = 1.0;
	// 留出 Actor/Physics Record 复制时间；只唤醒，不在这个较长走廊中施力。
	double PawnContactPredictionSeconds = 0.4;
	double PawnContactPadding = 10.0;
	double RetentionPadding = 300.0;
	double GraceSeconds = 3.0;
	/** 新物件即使同帧落入 Pawn 近场也必须有硬上限，禁止生命周期洪峰无界创建 Physics Body。 */
	int32 ImmediateAddsPerFrame = 32;
	int32 PredictiveAddsPerFrame = 16;
	int32 RemovesPerFrame = 32;
	double SourceMoveThreshold = 50.0;
	double SourceDirectionThresholdDegrees = 15.0;
	double SourceSpeedThreshold = 100.0;

	bool IsValid() const
	{
		return FMath::IsFinite(ImmediateCollisionRadius) && ImmediateCollisionRadius >= 0.0
			&& FMath::IsFinite(CameraCorridorPadding) && CameraCorridorPadding >= 0.0
			&& FMath::IsFinite(PredictionSeconds) && PredictionSeconds >= 0.0
			&& FMath::IsFinite(PawnContactPredictionSeconds) && PawnContactPredictionSeconds >= 0.0
			&& FMath::IsFinite(PawnContactPadding) && PawnContactPadding >= 0.0
			&& FMath::IsFinite(RetentionPadding) && RetentionPadding >= 0.0
			&& FMath::IsFinite(GraceSeconds) && GraceSeconds >= 0.0
			&& ImmediateAddsPerFrame >= 0 && PredictiveAddsPerFrame >= 0 && RemovesPerFrame >= 0
			&& FMath::IsFinite(SourceMoveThreshold) && SourceMoveThreshold >= 0.0
			&& FMath::IsFinite(SourceDirectionThresholdDegrees) && SourceDirectionThresholdDegrees >= 0.0
			&& FMath::IsFinite(SourceSpeedThreshold) && SourceSpeedThreshold >= 0.0;
	}
};

/** 一个本地预测 Pawn 或 Authority Pawn 提交给 WorldObject 碰撞投影器的纯观察数据。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectCollisionSource final
{
	FVector SubjectLocation = FVector::ZeroVector;
	FVector ViewLocation = FVector::ZeroVector;
	FVector ViewDirection = FVector::ForwardVector;
	FVector Velocity = FVector::ZeroVector;
	FBox PawnContactBounds = FBox(ForceInit);
	FBox ImmediateBounds = FBox(ForceInit);
	FBox PrefetchBounds = FBox(ForceInit);
	FBox RetentionBounds = FBox(ForceInit);
	uint64 Revision = 0;

	bool IsValid() const
	{
		return !SubjectLocation.ContainsNaN() && !ViewLocation.ContainsNaN()
			&& !ViewDirection.ContainsNaN() && !ViewDirection.IsNearlyZero()
			&& !Velocity.ContainsNaN() && PawnContactBounds.IsValid != 0
			&& ImmediateBounds.IsValid != 0
			&& PrefetchBounds.IsValid != 0 && RetentionBounds.IsValid != 0
			&& Revision != 0;
	}
};

struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectCollisionSourceHandle final
{
	int32 Slot = INDEX_NONE;
	uint32 Generation = 0;

	bool IsSet() const { return Slot != INDEX_NONE && Generation != 0; }
};

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectCollisionStats final
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int32 SourceCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 CollisionInstanceCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 PendingAddCount = 0;
	UPROPERTY(BlueprintReadOnly) int32 PendingRemoveCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 SourceSubmitCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 SpatialQueryCount = 0;
	UPROPERTY(BlueprintReadOnly) int64 CandidateTestCount = 0;
};
