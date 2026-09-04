#include "Observation/ElementViewCoverage.h"

namespace
{
	float HorizontalAngleDegrees(const FVector& Left, const FVector& Right)
	{
		const FVector2D A(Left.X, Left.Y);
		const FVector2D B(Right.X, Right.Y);
		if (A.IsNearlyZero() || B.IsNearlyZero())
		{
			return 0.0f;
		}
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
			FVector2D::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	}

	float PitchDegrees(const FVector& Direction)
	{
		const FVector Unit = Direction.GetSafeNormal();
		return FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Unit.Z, -1.0, 1.0)));
	}

	double SquaredDistanceToBox(const FVector& Point, const FBox& Box)
	{
		return FVector::DistSquared(Point, Box.GetClosestPointTo(Point));
	}

	bool PassesAngularCoverage(
		const FVector& CellCenter,
		const FPresentationViewSource& View,
		const FElementPresentationConfig& Config)
	{
		const FVector ToCell = CellCenter - View.ViewLocation;
		if (ToCell.IsNearlyZero())
		{
			return true;
		}
		const FVector Unit = ToCell.GetSafeNormal();
		const float Horizontal = FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(
			FVector::DotProduct(Unit, View.Right), FVector::DotProduct(Unit, View.Forward))));
		const float Vertical = FMath::Abs(FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(
			FVector::DotProduct(Unit, View.Up), -1.0, 1.0))));
		return Horizontal <= Config.HorizontalCoverageAngleDegrees * 0.5f + UE_KINDA_SMALL_NUMBER
			&& Vertical <= Config.VerticalCoverageAngleDegrees * 0.5f + UE_KINDA_SMALL_NUMBER;
	}
}

bool ElementViewCoverage::CrossesInvalidationThreshold(
	const FPresentationViewSource& Anchor,
	const FPresentationViewSource& Latest,
	const FElementPresentationConfig& Config)
{
	if (FVector::DistSquared(Anchor.SubjectLocation, Latest.SubjectLocation)
		> FMath::Square(Config.SubjectRecenterDistance)
		|| FVector::DistSquared(Anchor.ViewLocation, Latest.ViewLocation)
		> FMath::Square(Config.ViewRecenterDistance))
	{
		return true;
	}
	if (HorizontalAngleDegrees(Anchor.Forward, Latest.Forward)
		> Config.CalculateHorizontalRecenterAngleDegrees(Latest.HorizontalFOVDegrees)
		|| FMath::Abs(PitchDegrees(Anchor.Forward) - PitchDegrees(Latest.Forward))
		> Config.CalculateVerticalRecenterAngleDegrees(Latest.GetVerticalFOVDegrees()))
	{
		return true;
	}
	return FMath::Abs(Anchor.HorizontalFOVDegrees - Latest.HorizontalFOVDegrees)
			> Config.FieldOfViewChangeThresholdDegrees
		|| FMath::Abs(Anchor.GetVerticalFOVDegrees() - Latest.GetVerticalFOVDegrees())
			> Config.FieldOfViewChangeThresholdDegrees
		|| FMath::Abs(Anchor.AspectRatio - Latest.AspectRatio) > Config.AspectRatioChangeThreshold
		|| FMath::Abs(Anchor.ViewportSize.X - Latest.ViewportSize.X)
			> Config.ViewportDimensionChangeThreshold
		|| FMath::Abs(Anchor.ViewportSize.Y - Latest.ViewportSize.Y)
			> Config.ViewportDimensionChangeThreshold;
}

bool ElementViewCoverage::BuildCoverage(
	const FPresentationViewSource& View,
	const FElementPresentationConfig& Config,
	TSet<FElementVisualShardKey>& OutCoverage)
{
	OutCoverage.Reset();
	if (!View.IsValid() || !Config.IsValid())
	{
		return false;
	}
	const FVector Extent(Config.CoverageRadius);
	const FElementVisualShardKey Min = FElementVisualShardKey::FromWorldLocation(
		View.SubjectLocation - Extent, Config.ShardSize);
	const FElementVisualShardKey Max = FElementVisualShardKey::FromWorldLocation(
		View.SubjectLocation + Extent, Config.ShardSize);
	const double RadiusSquared = FMath::Square(Config.CoverageRadius);
	for (int32 X = Min.Coordinates.X; X <= Max.Coordinates.X; ++X)
	{
		for (int32 Y = Min.Coordinates.Y; Y <= Max.Coordinates.Y; ++Y)
		{
			for (int32 Z = Min.Coordinates.Z; Z <= Max.Coordinates.Z; ++Z)
			{
				const FVector CellMin(
					static_cast<double>(X) * Config.ShardSize,
					static_cast<double>(Y) * Config.ShardSize,
					static_cast<double>(Z) * Config.ShardSize);
				const FBox Bounds(CellMin, CellMin + FVector(Config.ShardSize));
				if (SquaredDistanceToBox(View.SubjectLocation, Bounds) > RadiusSquared)
				{
					continue;
				}
				if (!PassesAngularCoverage(Bounds.GetCenter(), View, Config))
				{
					continue;
				}
				OutCoverage.Add(FElementVisualShardKey{{X, Y, Z}});
				if (OutCoverage.Num() > Config.MaxCoverageShardsPerSource)
				{
					OutCoverage.Reset();
					return false;
				}
			}
		}
	}
	return true;
}
