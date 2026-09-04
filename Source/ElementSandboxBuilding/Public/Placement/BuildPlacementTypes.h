#pragma once

#include "CoreMinimal.h"

#include "BuildPlacementTypes.generated.h"

/** 客户端提示和服务器拒绝共用的摆放结果。 */
UENUM()
enum class EBuildPlacementFailure : uint8
{
	None,
	NoBuildItem,
	MissingDefinition,
	NoSurface,
	SurfaceTooSteep,
	InvalidTransform,
	OutOfRange,
	BlockedByBuilding,
	BlockedByWorld,
	StreamingNotReady,
	InventoryChanged,
	PlayerUnavailable,
	InventoryOpen,
	RateLimited,
	CreateFailed
};

struct ELEMENTSANDBOXBUILDING_API FBuildPlacementEvaluation final
{
	EBuildPlacementFailure Failure = EBuildPlacementFailure::InvalidTransform;
	FTransform ResolvedTransform = FTransform::Identity;
	uint64 SpatialRevision = 0;
	int32 CandidateEntityCount = 0;
	int32 WorldObjectCandidateCount = 0;
	int32 TestedPartPairCount = 0;
	int32 TestedShapePairCount = 0;
	bool bBuildingSupport = false;

	bool IsAllowed() const { return Failure == EBuildPlacementFailure::None; }
};

/** 共享支撑面查询的最近命中；Building 几何和 UE World 使用同一距离语义。 */
struct ELEMENTSANDBOXBUILDING_API FBuildPlacementSurfaceHit final
{
	FVector Location = FVector::ZeroVector;
	FVector Normal = FVector::ZeroVector;
	double Distance = TNumericLimits<double>::Max();
	bool bBuildingSurface = false;

	bool IsValid() const
	{
		return !Location.ContainsNaN()
			&& !Normal.ContainsNaN()
			&& Normal.SizeSquared() > UE_SMALL_NUMBER
			&& FMath::IsFinite(Distance)
			&& Distance >= 0.0;
	}
};
