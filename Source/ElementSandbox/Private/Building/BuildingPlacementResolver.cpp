#include "Building/BuildingPlacementResolver.h"

#include "BuildingWorldSubsystem.h"
#include "Definition/BuildingDefinition.h"
#include "Engine/World.h"

namespace
{
	constexpr double PlacementSupportTraceUp = 150.0;
	constexpr double PlacementSupportTraceDown = 300.0;
	constexpr double MaximumSlopeDegrees = 45.0;

	uint8 NormalizeQuarterTurns(const uint8 QuarterTurns)
	{
		return QuarterTurns % 4;
	}
}

bool FBuildingPlacementResolver::ResolveCandidateTransform(
	UWorld& World,
	UBuildingWorldSubsystem& BuildingSubsystem,
	const UBuildingDefinition& Definition,
	const FVector& SurfaceLocation,
	const FTransform& PlacementShapeTransform,
	const uint8 YawQuarterTurns,
	const AActor* IgnoredActor,
	FBuildPlacementEvaluation& OutEvaluation)
{
	OutEvaluation = {};
	const FVector ShapeScale = PlacementShapeTransform.GetScale3D();
	if (BuildingSubsystem.GetWorld() != &World
		|| SurfaceLocation.ContainsNaN()
		|| PlacementShapeTransform.ContainsNaN()
		|| !PlacementShapeTransform.GetLocation().IsNearlyZero()
		|| !PlacementShapeTransform.GetRotation().IsNormalized()
		|| !FMath::IsFinite(ShapeScale.X)
		|| !FMath::IsFinite(ShapeScale.Y)
		|| !FMath::IsFinite(ShapeScale.Z)
		|| ShapeScale.X <= UE_SMALL_NUMBER
		|| ShapeScale.Y <= UE_SMALL_NUMBER
		|| ShapeScale.Z <= UE_SMALL_NUMBER)
	{
		OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
		return false;
	}

	const FRotator PlacementYaw(
		0.0,
		static_cast<double>(NormalizeQuarterTurns(YawQuarterTurns)) * 90.0,
		0.0);
	// Shape 先保留原构件的倾角/缩放，再由玩家的离散 Yaw 与候选位置放入世界。
	FTransform CandidateTransform = PlacementShapeTransform * FTransform(
		PlacementYaw,
		FVector(SurfaceLocation.X, SurfaceLocation.Y, 0.0));
	FBox CandidateBounds(ForceInit);
	if (!Definition.TryCalculateWorldBounds(CandidateTransform, CandidateBounds))
	{
		OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
		return false;
	}

	const FVector TraceStart = SurfaceLocation
		+ FVector::UpVector * PlacementSupportTraceUp;
	const FVector TraceEnd = SurfaceLocation
		- FVector::UpVector * PlacementSupportTraceDown;
	FCollisionQueryParams QueryParams(TEXT("BuildingPlacementSupport"), false);
	if (IgnoredActor)
	{
		QueryParams.AddIgnoredActor(IgnoredActor);
	}
	FBuildPlacementSurfaceHit SupportHit;
	if (!BuildingSubsystem.QueryPlacementSurface(
		TraceStart,
		TraceEnd,
		QueryParams,
		SupportHit))
	{
		OutEvaluation.Failure = EBuildPlacementFailure::NoSurface;
		return true;
	}

	FVector CandidateLocation = CandidateTransform.GetLocation();
	CandidateLocation.Z = SupportHit.Location.Z - CandidateBounds.Min.Z;
	CandidateTransform.SetLocation(CandidateLocation);
	OutEvaluation.ResolvedTransform = CandidateTransform;
	OutEvaluation.bBuildingSupport = SupportHit.bBuildingSurface;

	const double MinimumSurfaceZ = FMath::Cos(
		FMath::DegreesToRadians(MaximumSlopeDegrees));
	if (SupportHit.Normal.GetSafeNormal().Z < MinimumSurfaceZ)
	{
		OutEvaluation.Failure = EBuildPlacementFailure::SurfaceTooSteep;
		return true;
	}
	OutEvaluation.Failure = EBuildPlacementFailure::None;
	return true;
}

bool FBuildingPlacementResolver::ResolveIntent(
	UWorld& World,
	UBuildingWorldSubsystem& BuildingSubsystem,
	const UBuildingDefinition& Definition,
	const FVector& SurfaceLocation,
	const FVector& ExpectedResolvedLocation,
	const FTransform& PlacementShapeTransform,
	const uint8 YawQuarterTurns,
	const FVector& BuilderLocation,
	const AActor* IgnoredActor,
	FBuildPlacementEvaluation& OutEvaluation)
{
	if (BuilderLocation.ContainsNaN() || ExpectedResolvedLocation.ContainsNaN())
	{
		OutEvaluation = {};
		OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
		return false;
	}
	if (!ResolveCandidateTransform(
		World,
		BuildingSubsystem,
		Definition,
		SurfaceLocation,
		PlacementShapeTransform,
		YawQuarterTurns,
		IgnoredActor,
		OutEvaluation)
		|| !OutEvaluation.IsAllowed())
	{
		return OutEvaluation.Failure != EBuildPlacementFailure::InvalidTransform;
	}
	constexpr double MaximumAuthorityLocationError = 1.0;
	if (!OutEvaluation.ResolvedTransform.GetLocation().Equals(
		ExpectedResolvedLocation,
		MaximumAuthorityLocationError))
	{
		OutEvaluation.Failure = OutEvaluation.bBuildingSupport
			? EBuildPlacementFailure::BlockedByBuilding
			: EBuildPlacementFailure::InvalidTransform;
		return true;
	}

	// EvaluatePlacement 会重置 OutEvaluation；输入若直接引用其中的
	// ResolvedTransform，就会在进入判定时被一并清成单位变换。
	const FTransform ResolvedTransform = OutEvaluation.ResolvedTransform;
	const bool bBuildingSupport = OutEvaluation.bBuildingSupport;
	const bool bEvaluated = BuildingSubsystem.EvaluatePlacement(
		Definition,
		ResolvedTransform,
		BuilderLocation,
		MaximumDistance,
		OutEvaluation);
	OutEvaluation.bBuildingSupport = bBuildingSupport;
	return bEvaluated;
}
