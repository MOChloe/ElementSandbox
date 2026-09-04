#include "Collision/BuildCollisionTypes.h"

namespace
{
	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	bool IsValidBounds(const FBox& Bounds)
	{
		return Bounds.IsValid != 0
			&& IsFiniteVector(Bounds.Min)
			&& IsFiniteVector(Bounds.Max)
			&& Bounds.Min.X <= Bounds.Max.X
			&& Bounds.Min.Y <= Bounds.Max.Y
			&& Bounds.Min.Z <= Bounds.Max.Z;
	}

	bool ContainsBounds(const FBox& Outer, const FBox& Inner)
	{
		return Outer.IsInsideOrOn(Inner.Min) && Outer.IsInsideOrOn(Inner.Max);
	}
}

bool FBuildCollisionSource::IsValid() const
{
	return Revision != 0
		&& IsFiniteVector(SubjectLocation)
		&& IsFiniteVector(Velocity)
		&& IsValidBounds(ImmediateBounds)
		&& IsValidBounds(PrefetchBounds)
		&& IsValidBounds(CameraBounds)
		&& IsValidBounds(RetentionBounds)
		&& ContainsBounds(RetentionBounds, ImmediateBounds)
		&& ContainsBounds(RetentionBounds, PrefetchBounds)
		&& ContainsBounds(RetentionBounds, CameraBounds);
}
