#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

/** 空间树内部的最小叶数据；Bounds 始终是精确包围盒，不包含 broadphase padding。 */
struct FBuildSpatialEntry final
{
	FBuildEntityHandle Entity;
	FBox Bounds;
};

namespace UE::ElementSandbox::Building::Private
{
	inline bool IsFiniteSpatialVector(const FVector& Value)
	{
		return !Value.ContainsNaN()
			&& FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	inline bool IsValidSpatialBounds(const FBox& Bounds)
	{
		return Bounds.IsValid != 0
			&& !Bounds.ContainsNaN()
			&& FMath::IsFinite(Bounds.Min.X)
			&& FMath::IsFinite(Bounds.Min.Y)
			&& FMath::IsFinite(Bounds.Min.Z)
			&& FMath::IsFinite(Bounds.Max.X)
			&& FMath::IsFinite(Bounds.Max.Y)
			&& FMath::IsFinite(Bounds.Max.Z)
			&& Bounds.Min.X <= Bounds.Max.X
			&& Bounds.Min.Y <= Bounds.Max.Y
			&& Bounds.Min.Z <= Bounds.Max.Z;
	}

	inline FBox MergeBounds(const FBox& Left, const FBox& Right)
	{
		return FBox(
			FVector(
				FMath::Min(Left.Min.X, Right.Min.X),
				FMath::Min(Left.Min.Y, Right.Min.Y),
				FMath::Min(Left.Min.Z, Right.Min.Z)),
			FVector(
				FMath::Max(Left.Max.X, Right.Max.X),
				FMath::Max(Left.Max.Y, Right.Max.Y),
				FMath::Max(Left.Max.Z, Right.Max.Z)));
	}

	inline double GetBoundsSurfaceArea(const FBox& Bounds)
	{
		const FVector Size = Bounds.GetSize();
		return 2.0 * (Size.X * Size.Y + Size.X * Size.Z + Size.Y * Size.Z);
	}

	/** 对有限线段执行 slab Ray-AABB 检测，返回从 Origin 起算的世界距离。 */
	inline bool RaycastBounds(
		const FBox& Bounds,
		const FVector& Origin,
		const FVector& UnitDirection,
		const double MaxDistance,
		double& OutDistance)
	{
		OutDistance = 0.0;
		if (!IsValidSpatialBounds(Bounds)
			|| !IsFiniteSpatialVector(Origin)
			|| !IsFiniteSpatialVector(UnitDirection)
			|| UnitDirection.IsNearlyZero()
			|| !FMath::IsFinite(MaxDistance)
			|| MaxDistance < 0.0)
		{
			return false;
		}

		double EntryDistance = 0.0;
		double ExitDistance = MaxDistance;
		constexpr double ParallelTolerance = 1.0e-12;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double AxisOrigin = Origin[Axis];
			const double AxisDirection = UnitDirection[Axis];
			if (FMath::Abs(AxisDirection) <= ParallelTolerance)
			{
				if (AxisOrigin < Bounds.Min[Axis] || AxisOrigin > Bounds.Max[Axis])
				{
					return false;
				}
				continue;
			}

			double NearDistance = (Bounds.Min[Axis] - AxisOrigin) / AxisDirection;
			double FarDistance = (Bounds.Max[Axis] - AxisOrigin) / AxisDirection;
			if (NearDistance > FarDistance)
			{
				Swap(NearDistance, FarDistance);
			}

			EntryDistance = FMath::Max(EntryDistance, NearDistance);
			ExitDistance = FMath::Min(ExitDistance, FarDistance);
			if (EntryDistance > ExitDistance)
			{
				return false;
			}
		}

		OutDistance = EntryDistance;
		return true;
	}
}
