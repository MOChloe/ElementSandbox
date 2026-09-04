#include "Shape/ElementCompoundShape.h"

namespace
{
	bool IsFinitePositive(const double Value)
	{
		return FMath::IsFinite(Value) && Value > UE_DOUBLE_SMALL_NUMBER;
	}

	FVector TransformExtent(const FTransform& Transform, const FVector& Extent)
	{
		const FMatrix Matrix = Transform.ToMatrixWithScale();
		return FVector(
			FMath::Abs(Matrix.M[0][0]) * Extent.X + FMath::Abs(Matrix.M[1][0]) * Extent.Y + FMath::Abs(Matrix.M[2][0]) * Extent.Z,
			FMath::Abs(Matrix.M[0][1]) * Extent.X + FMath::Abs(Matrix.M[1][1]) * Extent.Y + FMath::Abs(Matrix.M[2][1]) * Extent.Z,
			FMath::Abs(Matrix.M[0][2]) * Extent.X + FMath::Abs(Matrix.M[1][2]) * Extent.Y + FMath::Abs(Matrix.M[2][2]) * Extent.Z);
	}
}

FElementShape FElementShape::MakeSphere(const FVector& Center, const double Radius)
{
	FElementShape Shape;
	Shape.Kind = EElementPrimitiveKind::Sphere;
	Shape.Center = Center;
	Shape.Radius = Radius;
	return Shape;
}

FElementShape FElementShape::MakeBox(
	const FVector& Center,
	const FQuat& Rotation,
	const FVector& HalfExtents)
{
	FElementShape Shape;
	Shape.Kind = EElementPrimitiveKind::Box;
	Shape.Center = Center;
	Shape.Rotation = Rotation.GetNormalized();
	Shape.HalfExtents = HalfExtents;
	return Shape;
}

FElementShape FElementShape::MakeCapsule(
	const FVector& Center,
	const FVector& Axis,
	const double Radius,
	const double SegmentHalfLength)
{
	FElementShape Shape;
	Shape.Kind = EElementPrimitiveKind::Capsule;
	Shape.Center = Center;
	Shape.CapsuleAxis = Axis.GetSafeNormal();
	Shape.Radius = Radius;
	Shape.CapsuleSegmentHalfLength = SegmentHalfLength;
	return Shape;
}

bool FElementShape::IsValid() const
{
	if (Center.ContainsNaN()) return false;
	switch (Kind)
	{
	case EElementPrimitiveKind::Sphere:
		return IsFinitePositive(Radius);
	case EElementPrimitiveKind::Box:
		return !Rotation.ContainsNaN() && Rotation.IsNormalized()
			&& !HalfExtents.ContainsNaN()
			&& HalfExtents.GetMin() > UE_DOUBLE_SMALL_NUMBER;
	case EElementPrimitiveKind::Capsule:
		return !CapsuleAxis.ContainsNaN() && CapsuleAxis.IsNormalized()
			&& IsFinitePositive(Radius)
			&& FMath::IsFinite(CapsuleSegmentHalfLength)
			&& CapsuleSegmentHalfLength >= 0.0;
	default:
		return false;
	}
}

FBox FElementShape::CalculateWorldBounds(const FTransform& OwnerTransform) const
{
	if (!IsValid() || OwnerTransform.ContainsNaN()) return FBox(ForceInit);
	const FVector WorldCenter = OwnerTransform.TransformPosition(Center);
	const FVector Scale = OwnerTransform.GetScale3D().GetAbs();
	switch (Kind)
	{
	case EElementPrimitiveKind::Sphere:
	{
		const double WorldRadius = Radius * Scale.GetMax();
		return FBox::BuildAABB(WorldCenter, FVector(WorldRadius));
	}
	case EElementPrimitiveKind::Box:
	{
		const FTransform Local(Rotation, Center);
		const FTransform World = Local * OwnerTransform;
		return FBox::BuildAABB(WorldCenter, TransformExtent(World, HalfExtents));
	}
	case EElementPrimitiveKind::Capsule:
	{
		const FVector WorldAxis = OwnerTransform.TransformVectorNoScale(CapsuleAxis).GetSafeNormal();
		const double WorldRadius = Radius * Scale.GetMax();
		const double WorldHalfLength = CapsuleSegmentHalfLength * Scale.GetMax();
		const FVector Segment = WorldAxis * WorldHalfLength;
		return FBox(WorldCenter - Segment - FVector(WorldRadius), WorldCenter + Segment + FVector(WorldRadius));
	}
	default:
		return FBox(ForceInit);
	}
}

bool FElementCompoundShape::IsValid() const
{
	if (WorldTransform.ContainsNaN() || Shapes.IsEmpty()) return false;
	for (const FElementShape& Shape : Shapes)
	{
		if (!Shape.IsValid()) return false;
	}
	return true;
}

FBox FElementCompoundShape::CalculateWorldBounds() const
{
	FBox Bounds(ForceInit);
	if (!IsValid()) return Bounds;
	for (const FElementShape& Shape : Shapes)
	{
		Bounds += Shape.CalculateWorldBounds(WorldTransform);
	}
	return Bounds;
}

