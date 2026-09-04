#include "Shape/BuildShapeTypes.h"

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

FBuildShapeShardKey FBuildShapeShardKey::FromWorldLocation(
	const FVector& WorldLocation,
	const double ShardSize)
{
	FBuildShapeShardKey Result;
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

FBox FBuildShapeShardKey::CalculateWorldBounds(const double ShardSize) const
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

bool FBuildPartShapeDefinition::TryResolve(
	const FBox& MeshLocalBounds,
	FBuildPartShapeDefinition& OutResolved) const
{
	OutResolved = *this;
	if (TemplateRevision == 0)
	{
		return false;
	}
	if (Kind == EBuildPartShapeKind::MeshBoundsObb)
	{
		if (!IsFiniteBounds(MeshLocalBounds)
			|| MeshLocalBounds.GetExtent().GetMin() <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}
		OutResolved.Kind = EBuildPartShapeKind::Obb;
		OutResolved.Center = MeshLocalBounds.GetCenter();
		OutResolved.Rotation = FQuat::Identity;
		OutResolved.HalfExtents = MeshLocalBounds.GetExtent();
	}
	else if (Kind == EBuildPartShapeKind::Capsule)
	{
		OutResolved.CapsuleAxis = CapsuleAxis.GetSafeNormal();
	}
	else if (Kind == EBuildPartShapeKind::Obb)
	{
		OutResolved.Rotation = Rotation.GetNormalized();
	}
	return OutResolved.IsResolvedValid();
}

bool FBuildPartShapeDefinition::IsResolvedValid() const
{
	if (TemplateRevision == 0 || Center.ContainsNaN())
	{
		return false;
	}
	switch (Kind)
	{
	case EBuildPartShapeKind::Sphere:
		return FMath::IsFinite(Radius) && Radius > UE_DOUBLE_SMALL_NUMBER;
	case EBuildPartShapeKind::Capsule:
		return !CapsuleAxis.ContainsNaN() && CapsuleAxis.IsNormalized()
			&& FMath::IsFinite(Radius) && Radius > UE_DOUBLE_SMALL_NUMBER
			&& FMath::IsFinite(CapsuleSegmentHalfLength)
			&& CapsuleSegmentHalfLength >= 0.0;
	case EBuildPartShapeKind::Obb:
		return !Rotation.ContainsNaN() && Rotation.IsNormalized()
			&& !HalfExtents.ContainsNaN()
			&& HalfExtents.GetMin() > UE_DOUBLE_SMALL_NUMBER;
	default:
		return false;
	}
}

FBox FBuildPartShapeDefinition::CalculateBroadphaseBounds(
	const FTransform& WorldTransform) const
{
	FBox Result(ForceInit);
	if (!IsResolvedValid() || WorldTransform.ContainsNaN())
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
	case EBuildPartShapeKind::Sphere:
	{
		const FVector WorldCenter = WorldTransform.TransformPosition(Center);
		const FVector Extent(Radius * RadiusScale);
		return FBox(WorldCenter - Extent, WorldCenter + Extent);
	}
	case EBuildPartShapeKind::Capsule:
	{
		const FVector Offset = CapsuleAxis * CapsuleSegmentHalfLength;
		Result += WorldTransform.TransformPosition(Center - Offset);
		Result += WorldTransform.TransformPosition(Center + Offset);
		return Result.ExpandBy(Radius * RadiusScale);
	}
	case EBuildPartShapeKind::Obb:
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

bool FBuildShapeInstanceSnapshot::IsValid() const
{
	return ShapeRef.IsSet() && !DefinitionId.IsNone()
		&& TemplateRevision != 0 && EntityTransformRevision != 0
		&& PartTransformRevision != 0 && TransformRevision != 0 && StateRevision != 0
		&& LocalGeometry.IsResolvedValid() && !WorldTransform.ContainsNaN()
		&& IsFiniteBounds(WorldBounds)
		&& WorldBounds.Equals(LocalGeometry.CalculateBroadphaseBounds(WorldTransform), 0.01);
}

bool GetBuildShapeShardsForBounds(
	const FBox& WorldBounds,
	TArray<FBuildShapeShardKey>& OutShards,
	const double ShardSize)
{
	OutShards.Reset();
	if (!IsFiniteBounds(WorldBounds) || !FMath::IsFinite(ShardSize)
		|| ShardSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}
	const FBuildShapeShardKey Minimum = FBuildShapeShardKey::FromWorldLocation(WorldBounds.Min, ShardSize);
	const FBuildShapeShardKey Maximum = FBuildShapeShardKey::FromWorldLocation(WorldBounds.Max, ShardSize);
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
				FBuildShapeShardKey& Key = OutShards.AddDefaulted_GetRef();
				Key.Coordinates = FIntVector(X, Y, Z);
			}
		}
	}
	return true;
}
