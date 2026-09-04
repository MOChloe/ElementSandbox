#pragma once

#include "CoreMinimal.h"
#include "PresentationViewSource.h"

inline double ComputeSettlementTreeBoundsDistanceSquared2D(const FBox& Bounds, const FVector& Point)
{
	if (Bounds.IsValid == 0 || Bounds.ContainsNaN() || Point.ContainsNaN())
	{
		return TNumericLimits<double>::Max();
	}
	const double DeltaX = Point.X < Bounds.Min.X
		? Bounds.Min.X - Point.X
		: (Point.X > Bounds.Max.X ? Point.X - Bounds.Max.X : 0.0);
	const double DeltaY = Point.Y < Bounds.Min.Y
		? Bounds.Min.Y - Point.Y
		: (Point.Y > Bounds.Max.Y ? Point.Y - Bounds.Max.Y : 0.0);
	return DeltaX * DeltaX + DeltaY * DeltaY;
}

inline double ComputeSettlementTreeRecenterThresholdDegrees(
	const double CoverageAngleDegrees,
	const double HorizontalFOVDegrees,
	const double FOVSafetyAngleDegrees,
	const double MinimumRecenterAngleDegrees)
{
	return FMath::Max(
		FMath::Max(0.0, MinimumRecenterAngleDegrees),
		(FMath::Clamp(CoverageAngleDegrees, 1.0, 360.0) - FMath::Clamp(HorizontalFOVDegrees, 1.0, 179.0)) * 0.5
			- FMath::Max(0.0, FOVSafetyAngleDegrees));
}

inline double ComputeSettlementTreeLocalCoverageRadius(
	const double LocalRadius,
	const double SourceMovementThreshold,
	const double FOVSafetyAngleDegrees)
{
	const double SafetyRadians = FMath::DegreesToRadians(FMath::Clamp(FOVSafetyAngleDegrees, 0.1, 89.0));
	return FMath::Max(
		FMath::Max(0.0, LocalRadius) + FMath::Max(0.0, SourceMovementThreshold),
		FMath::Max(0.0, SourceMovementThreshold) / FMath::Sin(SafetyRadians));
}

inline bool IsSettlementTreePositionCoverageReusable(
	const FVector& AnchorSubjectLocation,
	const FVector& AnchorViewLocation,
	const FVector& CurrentSubjectLocation,
	const FVector& CurrentViewLocation,
	const double MovementThreshold)
{
	const double ThresholdSquared = FMath::Square(FMath::Max(0.0, MovementThreshold));
	return FVector::DistSquared2D(AnchorSubjectLocation, CurrentSubjectLocation) <= ThresholdSquared
		&& FVector::DistSquared2D(AnchorViewLocation, CurrentViewLocation) <= ThresholdSquared;
}

inline bool IsSettlementTreeFarCoverageReusable(
	const FVector& AnchorSubjectLocation,
	const FVector& AnchorViewLocation,
	const FVector& AnchorHorizontalForward,
	const float AnchorHorizontalFOVDegrees,
	const float AnchorAspectRatio,
	const FIntPoint AnchorViewportSize,
	const FVector& CurrentSubjectLocation,
	const FVector& CurrentViewLocation,
	const FVector& CurrentHorizontalForward,
	const float CurrentHorizontalFOVDegrees,
	const float CurrentAspectRatio,
	const FIntPoint CurrentViewportSize,
	const double MovementThreshold,
	const double CoverageAngleDegrees,
	const double FOVSafetyAngleDegrees,
	const double MinimumRecenterAngleDegrees)
{
	// 360° Far 集合就是完整 Resident Catalog；它的成员只由 Catalog 的增量
	// Upsert/Remove 改变，与观察源的位置、朝向、FOV 和窗口尺寸都无关。
	if (CoverageAngleDegrees >= 360.0 - UE_DOUBLE_SMALL_NUMBER)
	{
		return true;
	}
	if (!IsSettlementTreePositionCoverageReusable(
			AnchorSubjectLocation,
			AnchorViewLocation,
			CurrentSubjectLocation,
			CurrentViewLocation,
			MovementThreshold)
		|| !FMath::IsNearlyEqual(AnchorHorizontalFOVDegrees, CurrentHorizontalFOVDegrees, 0.01f)
		|| !FMath::IsNearlyEqual(AnchorAspectRatio, CurrentAspectRatio, 0.0001f)
		|| AnchorViewportSize != CurrentViewportSize)
	{
		return false;
	}
	const FVector Anchor2D(AnchorHorizontalForward.X, AnchorHorizontalForward.Y, 0.0);
	const FVector Current2D(CurrentHorizontalForward.X, CurrentHorizontalForward.Y, 0.0);
	if (Anchor2D.IsNearlyZero() || Current2D.IsNearlyZero())
	{
		return false;
	}
	const double Dot = FMath::Clamp(
		static_cast<double>(FVector::DotProduct(Anchor2D.GetSafeNormal(), Current2D.GetSafeNormal())),
		-1.0,
		1.0);
	const double DeltaDegrees = FMath::RadiansToDegrees(FMath::Acos(Dot));
	return DeltaDegrees <= ComputeSettlementTreeRecenterThresholdDegrees(
		CoverageAngleDegrees,
		CurrentHorizontalFOVDegrees,
		FOVSafetyAngleDegrees,
		MinimumRecenterAngleDegrees);
}

inline bool DoesSettlementTreeBoundsIntersectHorizontalSector(
	const FBox& Bounds,
	const FVector& Origin,
	const FVector& Forward,
	const double HalfAngleDegrees)
{
	if (Bounds.IsValid == 0 || Bounds.ContainsNaN() || Origin.ContainsNaN() || Forward.ContainsNaN())
	{
		return false;
	}
	if (ComputeSettlementTreeBoundsDistanceSquared2D(Bounds, Origin) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return true;
	}
	const FVector2D HorizontalForward(Forward.X, Forward.Y);
	const FVector Center = Bounds.GetCenter();
	const FVector Extent = Bounds.GetExtent();
	const FVector2D ToCenter(Center.X - Origin.X, Center.Y - Origin.Y);
	if (HorizontalForward.IsNearlyZero() || ToCenter.IsNearlyZero())
	{
		return true;
	}
	const double Distance = ToCenter.Size();
	const double Radius = FVector2D(Extent.X, Extent.Y).Size();
	const double AngularRadius = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Radius / Distance, 0.0, 1.0)));
	const double Dot = FMath::Clamp(
		static_cast<double>(FVector2D::DotProduct(HorizontalForward.GetSafeNormal(), ToCenter.GetSafeNormal())),
		-1.0,
		1.0);
	const double DeltaDegrees = FMath::RadiansToDegrees(FMath::Acos(Dot));
	return DeltaDegrees <= FMath::Clamp(HalfAngleDegrees, 0.0, 180.0) + AngularRadius;
}

inline bool IsSettlementTreeBoundsDesired(
	const FBox& WorldBounds,
	const FPresentationViewSource& Source,
	const double LocalRadiusSquared,
	const double ForwardHalfAngleDegrees)
{
	return ComputeSettlementTreeBoundsDistanceSquared2D(WorldBounds, Source.SubjectLocation) <= LocalRadiusSquared
		|| DoesSettlementTreeBoundsIntersectHorizontalSector(
			WorldBounds, Source.ViewLocation, Source.Forward, ForwardHalfAngleDegrees);
}

/** 纯数据方向门；Worker 与 Automation 共用，避免测试复制另一套选择规则。 */
inline bool IsSettlementTreeCandidateDesired(
	const FVector& TreeLocation,
	const TConstArrayView<FPresentationViewSource> Sources,
	const double LocalRadiusSquared,
	const double ForwardDotThreshold)
{
	for (const FPresentationViewSource& Source : Sources)
	{
		const double HalfAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(
			FMath::Clamp(ForwardDotThreshold, -1.0, 1.0)));
		if (IsSettlementTreeBoundsDesired(
			FBox(TreeLocation, TreeLocation), Source, LocalRadiusSquared, HalfAngleDegrees))
		{
			return true;
		}
	}
	return false;
}

inline double ComputeSettlementTreeHorizontalAngularSpeedDegreesPerSecond(
	const FVector& PreviousForward,
	const FVector& CurrentForward,
	const double DeltaSeconds)
{
	const FVector Previous2D(PreviousForward.X, PreviousForward.Y, 0.0);
	const FVector Current2D(CurrentForward.X, CurrentForward.Y, 0.0);
	if (DeltaSeconds <= UE_DOUBLE_SMALL_NUMBER || Previous2D.IsNearlyZero() || Current2D.IsNearlyZero())
	{
		return 0.0;
	}
	const double Dot = FMath::Clamp(
		static_cast<double>(FVector::DotProduct(Previous2D.GetSafeNormal(), Current2D.GetSafeNormal())),
		-1.0,
		1.0);
	return FMath::RadiansToDegrees(FMath::Acos(Dot)) / DeltaSeconds;
}

inline bool IsSettlementTreeRapidRotationSettling(
	const double NowSeconds,
	const double LastRapidRotationSeconds,
	const double SettleSeconds)
{
	return NowSeconds >= LastRapidRotationSeconds
		&& NowSeconds - LastRapidRotationSeconds < FMath::Max(0.0, SettleSeconds);
}
