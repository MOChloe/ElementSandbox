#include "Shape/WorldObjectShapeTypes.h"

namespace
{
	bool IsFiniteBounds(const FBox& Bounds)
	{
		return Bounds.IsValid != 0
			&& !Bounds.Min.ContainsNaN()
			&& !Bounds.Max.ContainsNaN()
			&& Bounds.Min.X <= Bounds.Max.X
			&& Bounds.Min.Y <= Bounds.Max.Y
			&& Bounds.Min.Z <= Bounds.Max.Z;
	}

	double MaxAbsScale(const FVector& Scale)
	{
		return FMath::Max3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
	}
}

FWorldObjectShapeShardKey FWorldObjectShapeShardKey::FromWorldLocation(
	const FVector& WorldLocation,
	const double ShardSize)
{
	FWorldObjectShapeShardKey Result;
	if (WorldLocation.ContainsNaN() || !FMath::IsFinite(ShardSize)
		|| ShardSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		return Result;
	}
	Result.Coordinates = FIntVector(
		FMath::FloorToInt(WorldLocation.X / ShardSize),
		FMath::FloorToInt(WorldLocation.Y / ShardSize),
		FMath::FloorToInt(WorldLocation.Z / ShardSize));
	return Result;
}

FBox FWorldObjectShapeShardKey::CalculateWorldBounds(const double ShardSize) const
{
	if (!FMath::IsFinite(ShardSize) || ShardSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		return FBox(ForceInit);
	}
	const FVector Minimum(
		static_cast<double>(Coordinates.X) * ShardSize,
		static_cast<double>(Coordinates.Y) * ShardSize,
		static_cast<double>(Coordinates.Z) * ShardSize);
	return FBox(Minimum, Minimum + FVector(ShardSize));
}

bool FWorldObjectShapeDefinition::IsValid() const
{
	if (TemplateRevision == 0 || Center.ContainsNaN())
	{
		return false;
	}
	switch (Kind)
	{
	case EWorldObjectShapeKind::Sphere:
		return FMath::IsFinite(Radius) && Radius > UE_DOUBLE_SMALL_NUMBER;
	case EWorldObjectShapeKind::Capsule:
		return !CapsuleAxis.ContainsNaN() && CapsuleAxis.IsNormalized()
			&& FMath::IsFinite(Radius) && Radius > UE_DOUBLE_SMALL_NUMBER
			&& FMath::IsFinite(CapsuleSegmentHalfLength)
			&& CapsuleSegmentHalfLength >= 0.0;
	case EWorldObjectShapeKind::Obb:
		return !Rotation.ContainsNaN() && Rotation.IsNormalized()
			&& !HalfExtents.ContainsNaN()
			&& HalfExtents.GetMin() > UE_DOUBLE_SMALL_NUMBER;
	default:
		return false;
	}
}

FBox FWorldObjectShapeDefinition::CalculateBroadphaseBounds(
	const FTransform& WorldTransform) const
{
	FBox Result(ForceInit);
	if (!IsValid() || WorldTransform.ContainsNaN())
	{
		return Result;
	}
	const double RadiusScale = MaxAbsScale(WorldTransform.GetScale3D());
	if (!FMath::IsFinite(RadiusScale) || RadiusScale <= UE_DOUBLE_SMALL_NUMBER)
	{
		return Result;
	}
	switch (Kind)
	{
	case EWorldObjectShapeKind::Sphere:
	{
		const FVector WorldCenter = WorldTransform.TransformPosition(Center);
		const FVector Extent(Radius * RadiusScale);
		return FBox(WorldCenter - Extent, WorldCenter + Extent);
	}
	case EWorldObjectShapeKind::Capsule:
	{
		const FVector Offset = CapsuleAxis * CapsuleSegmentHalfLength;
		Result += WorldTransform.TransformPosition(Center - Offset);
		Result += WorldTransform.TransformPosition(Center + Offset);
		return Result.ExpandBy(Radius * RadiusScale);
	}
	case EWorldObjectShapeKind::Obb:
	{
		const FVector AxisX = Rotation.GetAxisX();
		const FVector AxisY = Rotation.GetAxisY();
		const FVector AxisZ = Rotation.GetAxisZ();
		for (int32 X = -1; X <= 1; X += 2)
		{
			for (int32 Y = -1; Y <= 1; Y += 2)
			{
				for (int32 Z = -1; Z <= 1; Z += 2)
				{
					Result += WorldTransform.TransformPosition(
						Center + AxisX * (HalfExtents.X * X)
						+ AxisY * (HalfExtents.Y * Y)
						+ AxisZ * (HalfExtents.Z * Z));
				}
			}
		}
		return Result;
	}
	default:
		return Result;
	}
}

FWorldObjectShapeDefinition FWorldObjectShapeDefinition::MakeObbFromBounds(
	const FBox& LocalBounds,
	const uint64 InTemplateRevision)
{
	FWorldObjectShapeDefinition Result;
	Result.Kind = EWorldObjectShapeKind::Obb;
	Result.Center = LocalBounds.GetCenter();
	Result.Rotation = FQuat::Identity;
	Result.HalfExtents = LocalBounds.GetExtent();
	Result.TemplateRevision = InTemplateRevision;
	return Result;
}

bool FWorldObjectShapeInstanceSnapshot::IsValid() const
{
	return ShapeRef.IsSet() && !DefinitionId.IsNone()
		&& TemplateRevision != 0 && TransformRevision != 0
		&& ShapeRevision != 0 && StateRevision != 0
		&& LocalGeometry.IsValid() && !WorldTransform.ContainsNaN()
		&& IsFiniteBounds(InteractionWorldBounds) && IsFiniteBounds(WorldBounds)
		&& WorldBounds.Equals(LocalGeometry.CalculateBroadphaseBounds(WorldTransform), 0.01);
}

bool GetWorldObjectShapeShardsForBounds(
	const FBox& WorldBounds,
	TArray<FWorldObjectShapeShardKey>& OutShards,
	const double ShardSize)
{
	OutShards.Reset();
	if (!IsFiniteBounds(WorldBounds) || !FMath::IsFinite(ShardSize)
		|| ShardSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}
	const FWorldObjectShapeShardKey Minimum =
		FWorldObjectShapeShardKey::FromWorldLocation(WorldBounds.Min, ShardSize);
	const FWorldObjectShapeShardKey Maximum =
		FWorldObjectShapeShardKey::FromWorldLocation(WorldBounds.Max, ShardSize);
	const int64 CountX = static_cast<int64>(Maximum.Coordinates.X) - Minimum.Coordinates.X + 1;
	const int64 CountY = static_cast<int64>(Maximum.Coordinates.Y) - Minimum.Coordinates.Y + 1;
	const int64 CountZ = static_cast<int64>(Maximum.Coordinates.Z) - Minimum.Coordinates.Z + 1;
	const int64 Count = CountX * CountY * CountZ;
	if (Count <= 0 || Count > MAX_int32)
	{
		return false;
	}
	OutShards.Reserve(static_cast<int32>(Count));
	for (int32 X = Minimum.Coordinates.X; X <= Maximum.Coordinates.X; ++X)
	{
		for (int32 Y = Minimum.Coordinates.Y; Y <= Maximum.Coordinates.Y; ++Y)
		{
			for (int32 Z = Minimum.Coordinates.Z; Z <= Maximum.Coordinates.Z; ++Z)
			{
				FWorldObjectShapeShardKey& Key = OutShards.AddDefaulted_GetRef();
				Key.Coordinates = FIntVector(X, Y, Z);
			}
		}
	}
	return true;
}
