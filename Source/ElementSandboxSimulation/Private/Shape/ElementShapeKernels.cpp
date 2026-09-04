#include "Shape/ElementShapeKernels.h"

namespace
{
	struct FSpherePrimitive final
	{
		FVector Center = FVector::ZeroVector;
		double Radius = 0.0;
	};

	struct FBoxPrimitive final
	{
		FVector Center = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector Extent = FVector::ZeroVector;
	};

	struct FCapsulePrimitive final
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		double Radius = 0.0;
	};

	double MaxScale(const FTransform& Transform)
	{
		return Transform.GetScale3D().GetAbs().GetMax();
	}

	FSpherePrimitive MakeSphere(const FElementShape& Shape, const FTransform& Owner)
	{
		return {Owner.TransformPosition(Shape.Center), Shape.Radius * MaxScale(Owner)};
	}

	FBoxPrimitive MakeBox(const FElementShape& Shape, const FTransform& Owner)
	{
		const FVector OwnerScale = Owner.GetScale3D().GetAbs();
		FBoxPrimitive Result;
		Result.Center = Owner.TransformPosition(Shape.Center);
		Result.Rotation = (Owner.GetRotation() * Shape.Rotation).GetNormalized();
		Result.Extent = Shape.HalfExtents * OwnerScale;
		return Result;
	}

	FCapsulePrimitive MakeCapsule(const FElementShape& Shape, const FTransform& Owner)
	{
		const FVector Center = Owner.TransformPosition(Shape.Center);
		const FVector Axis = Owner.TransformVectorNoScale(Shape.CapsuleAxis).GetSafeNormal();
		const double Scale = MaxScale(Owner);
		const FVector HalfSegment = Axis * Shape.CapsuleSegmentHalfLength * Scale;
		return {Center - HalfSegment, Center + HalfSegment, Shape.Radius * Scale};
	}

	double PointSegmentDistanceSquared(const FVector& Point, const FVector& Start, const FVector& End)
	{
		const FVector Segment = End - Start;
		const double Denominator = Segment.SizeSquared();
		const double Alpha = Denominator > UE_DOUBLE_SMALL_NUMBER
			? FMath::Clamp(FVector::DotProduct(Point - Start, Segment) / Denominator, 0.0, 1.0)
			: 0.0;
		return FVector::DistSquared(Point, Start + Segment * Alpha);
	}

	double SegmentSegmentDistanceSquared(
		const FVector& P1,
		const FVector& Q1,
		const FVector& P2,
		const FVector& Q2)
	{
		const FVector D1 = Q1 - P1;
		const FVector D2 = Q2 - P2;
		const FVector R = P1 - P2;
		const double A = FVector::DotProduct(D1, D1);
		const double E = FVector::DotProduct(D2, D2);
		const double F = FVector::DotProduct(D2, R);
		double S = 0.0;
		double T = 0.0;
		if (A <= UE_DOUBLE_SMALL_NUMBER && E <= UE_DOUBLE_SMALL_NUMBER) return R.SizeSquared();
		if (A <= UE_DOUBLE_SMALL_NUMBER)
		{
			T = FMath::Clamp(F / E, 0.0, 1.0);
		}
		else
		{
			const double C = FVector::DotProduct(D1, R);
			if (E <= UE_DOUBLE_SMALL_NUMBER)
			{
				S = FMath::Clamp(-C / A, 0.0, 1.0);
			}
			else
			{
				const double B = FVector::DotProduct(D1, D2);
				const double Denominator = A * E - B * B;
				if (Denominator > UE_DOUBLE_SMALL_NUMBER)
				{
					S = FMath::Clamp((B * F - C * E) / Denominator, 0.0, 1.0);
				}
				T = (B * S + F) / E;
				if (T < 0.0)
				{
					T = 0.0;
					S = FMath::Clamp(-C / A, 0.0, 1.0);
				}
				else if (T > 1.0)
				{
					T = 1.0;
					S = FMath::Clamp((B - C) / A, 0.0, 1.0);
				}
			}
		}
		return FVector::DistSquared(P1 + D1 * S, P2 + D2 * T);
	}

	double PointBoxDistanceSquared(const FVector& Point, const FBoxPrimitive& Box)
	{
		const FVector Local = Box.Rotation.UnrotateVector(Point - Box.Center);
		double Result = 0.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Excess = FMath::Max(FMath::Abs(Local[Axis]) - Box.Extent[Axis], 0.0);
			Result += Excess * Excess;
		}
		return Result;
	}

	double SegmentBoxDistanceSquared(const FVector& Start, const FVector& End, const FBoxPrimitive& Box)
	{
		const FVector LocalStart = Box.Rotation.UnrotateVector(Start - Box.Center);
		const FVector LocalEnd = Box.Rotation.UnrotateVector(End - Box.Center);
		const FVector Direction = LocalEnd - LocalStart;
		auto DistanceAt = [&Box, &LocalStart, &Direction](const double Alpha)
		{
			const FVector Point = LocalStart + Direction * Alpha;
			double Result = 0.0;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const double Excess = FMath::Max(FMath::Abs(Point[Axis]) - Box.Extent[Axis], 0.0);
				Result += Excess * Excess;
			}
			return Result;
		};
		double Low = 0.0;
		double High = 1.0;
		for (int32 Iteration = 0; Iteration < 18; ++Iteration)
		{
			const double Third = (High - Low) / 3.0;
			const double Left = Low + Third;
			const double Right = High - Third;
			if (DistanceAt(Left) <= DistanceAt(Right)) High = Right;
			else Low = Left;
		}
		return DistanceAt((Low + High) * 0.5);
	}

	bool IntersectsBoxBox(const FBoxPrimitive& A, const FBoxPrimitive& B)
	{
		FVector AxesA[3] = {
			A.Rotation.GetAxisX(), A.Rotation.GetAxisY(), A.Rotation.GetAxisZ()};
		FVector AxesB[3] = {
			B.Rotation.GetAxisX(), B.Rotation.GetAxisY(), B.Rotation.GetAxisZ()};
		double Rotation[3][3];
		double AbsRotation[3][3];
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = 0; J < 3; ++J)
			{
				Rotation[I][J] = FVector::DotProduct(AxesA[I], AxesB[J]);
				AbsRotation[I][J] = FMath::Abs(Rotation[I][J]) + 1.e-8;
			}
		}
		const FVector Delta = B.Center - A.Center;
		double Translation[3] = {
			FVector::DotProduct(Delta, AxesA[0]),
			FVector::DotProduct(Delta, AxesA[1]),
			FVector::DotProduct(Delta, AxesA[2])};
		for (int32 I = 0; I < 3; ++I)
		{
			const double RadiusB = B.Extent.X * AbsRotation[I][0]
				+ B.Extent.Y * AbsRotation[I][1] + B.Extent.Z * AbsRotation[I][2];
			if (FMath::Abs(Translation[I]) > A.Extent[I] + RadiusB) return false;
		}
		for (int32 J = 0; J < 3; ++J)
		{
			const double Projection = FMath::Abs(Translation[0] * Rotation[0][J]
				+ Translation[1] * Rotation[1][J] + Translation[2] * Rotation[2][J]);
			const double RadiusA = A.Extent.X * AbsRotation[0][J]
				+ A.Extent.Y * AbsRotation[1][J] + A.Extent.Z * AbsRotation[2][J];
			if (Projection > RadiusA + B.Extent[J]) return false;
		}
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = 0; J < 3; ++J)
			{
				const int32 I1 = (I + 1) % 3;
				const int32 I2 = (I + 2) % 3;
				const int32 J1 = (J + 1) % 3;
				const int32 J2 = (J + 2) % 3;
				const double RadiusA = A.Extent[I1] * AbsRotation[I2][J]
					+ A.Extent[I2] * AbsRotation[I1][J];
				const double RadiusB = B.Extent[J1] * AbsRotation[I][J2]
					+ B.Extent[J2] * AbsRotation[I][J1];
				const double Projection = FMath::Abs(Translation[I2] * Rotation[I1][J]
					- Translation[I1] * Rotation[I2][J]);
				if (Projection > RadiusA + RadiusB) return false;
			}
		}
		return true;
	}

	bool IntersectsShapes(
		const FElementShape& Left,
		const FTransform& LeftOwner,
		const FElementShape& Right,
		const FTransform& RightOwner)
	{
		if (Left.Kind == EElementPrimitiveKind::Sphere)
		{
			const FSpherePrimitive Sphere = MakeSphere(Left, LeftOwner);
			if (Right.Kind == EElementPrimitiveKind::Sphere)
			{
				const FSpherePrimitive Other = MakeSphere(Right, RightOwner);
				return FVector::DistSquared(Sphere.Center, Other.Center)
					<= FMath::Square(Sphere.Radius + Other.Radius);
			}
			if (Right.Kind == EElementPrimitiveKind::Box)
			{
				return PointBoxDistanceSquared(Sphere.Center, MakeBox(Right, RightOwner))
					<= FMath::Square(Sphere.Radius);
			}
			const FCapsulePrimitive Capsule = MakeCapsule(Right, RightOwner);
			return PointSegmentDistanceSquared(Sphere.Center, Capsule.Start, Capsule.End)
				<= FMath::Square(Sphere.Radius + Capsule.Radius);
		}
		if (Left.Kind == EElementPrimitiveKind::Box)
		{
			const FBoxPrimitive Box = MakeBox(Left, LeftOwner);
			if (Right.Kind == EElementPrimitiveKind::Sphere)
			{
				const FSpherePrimitive Sphere = MakeSphere(Right, RightOwner);
				return PointBoxDistanceSquared(Sphere.Center, Box) <= FMath::Square(Sphere.Radius);
			}
			if (Right.Kind == EElementPrimitiveKind::Box)
			{
				return IntersectsBoxBox(Box, MakeBox(Right, RightOwner));
			}
			const FCapsulePrimitive Capsule = MakeCapsule(Right, RightOwner);
			return SegmentBoxDistanceSquared(Capsule.Start, Capsule.End, Box)
				<= FMath::Square(Capsule.Radius);
		}
		const FCapsulePrimitive Capsule = MakeCapsule(Left, LeftOwner);
		if (Right.Kind == EElementPrimitiveKind::Sphere)
		{
			const FSpherePrimitive Sphere = MakeSphere(Right, RightOwner);
			return PointSegmentDistanceSquared(Sphere.Center, Capsule.Start, Capsule.End)
				<= FMath::Square(Sphere.Radius + Capsule.Radius);
		}
		if (Right.Kind == EElementPrimitiveKind::Box)
		{
			return SegmentBoxDistanceSquared(Capsule.Start, Capsule.End, MakeBox(Right, RightOwner))
				<= FMath::Square(Capsule.Radius);
		}
		const FCapsulePrimitive Other = MakeCapsule(Right, RightOwner);
		return SegmentSegmentDistanceSquared(Capsule.Start, Capsule.End, Other.Start, Other.End)
			<= FMath::Square(Capsule.Radius + Other.Radius);
	}

	double CalculateLinearWeight(const double Distance, const double FalloffExtent)
	{
		return FMath::Clamp(1.0 - Distance / FalloffExtent, 0.0, 1.0);
	}

	double PointShapeDistance(
		const FVector& Point,
		const FElementShape& Target,
		const FTransform& TargetOwner)
	{
		switch (Target.Kind)
		{
		case EElementPrimitiveKind::Sphere:
		{
			const FSpherePrimitive Sphere = MakeSphere(Target, TargetOwner);
			return FMath::Max(FVector::Distance(Point, Sphere.Center) - Sphere.Radius, 0.0);
		}
		case EElementPrimitiveKind::Box:
			return FMath::Sqrt(PointBoxDistanceSquared(Point, MakeBox(Target, TargetOwner)));
		case EElementPrimitiveKind::Capsule:
		{
			const FCapsulePrimitive Capsule = MakeCapsule(Target, TargetOwner);
			return FMath::Max(FMath::Sqrt(PointSegmentDistanceSquared(
				Point, Capsule.Start, Capsule.End)) - Capsule.Radius, 0.0);
		}
		default:
			return 0.0;
		}
	}

	double SegmentShapeDistance(
		const FVector& Start,
		const FVector& End,
		const FElementShape& Target,
		const FTransform& TargetOwner)
	{
		switch (Target.Kind)
		{
		case EElementPrimitiveKind::Sphere:
		{
			const FSpherePrimitive Sphere = MakeSphere(Target, TargetOwner);
			return FMath::Max(FMath::Sqrt(PointSegmentDistanceSquared(
				Sphere.Center, Start, End)) - Sphere.Radius, 0.0);
		}
		case EElementPrimitiveKind::Box:
			return FMath::Sqrt(SegmentBoxDistanceSquared(
				Start, End, MakeBox(Target, TargetOwner)));
		case EElementPrimitiveKind::Capsule:
		{
			const FCapsulePrimitive Capsule = MakeCapsule(Target, TargetOwner);
			return FMath::Max(FMath::Sqrt(SegmentSegmentDistanceSquared(
				Start, End, Capsule.Start, Capsule.End)) - Capsule.Radius, 0.0);
		}
		default:
			return 0.0;
		}
	}

	template <typename PredicateType>
	double CalculateScaledBoxWeight(
		const FBoxPrimitive& Influence,
		const PredicateType& IntersectsAtScale)
	{
		FBoxPrimitive LevelSet = Influence;
		LevelSet.Extent = FVector::ZeroVector;
		if (IntersectsAtScale(LevelSet)) return 1.0;
		LevelSet.Extent = Influence.Extent;
		if (!IntersectsAtScale(LevelSet)) return 0.0;

		// Box 的等权重面是同中心、同朝向的缩放 OBB。二分寻找第一个与目标
		// 相交的等权重面，避免用目标中心或 AABB 角点近似实际接触位置。
		double Low = 0.0;
		double High = 1.0;
		for (int32 Iteration = 0; Iteration < 18; ++Iteration)
		{
			const double Middle = (Low + High) * 0.5;
			LevelSet.Extent = Influence.Extent * Middle;
			if (IntersectsAtScale(LevelSet)) High = Middle;
			else Low = Middle;
		}
		return FMath::Clamp(1.0 - High, 0.0, 1.0);
	}

	double BoxShapeWeight(
		const FBoxPrimitive& Influence,
		const FElementShape& Target,
		const FTransform& TargetOwner)
	{
		switch (Target.Kind)
		{
		case EElementPrimitiveKind::Sphere:
		{
			const FSpherePrimitive Sphere = MakeSphere(Target, TargetOwner);
			return CalculateScaledBoxWeight(Influence,
				[&Sphere](const FBoxPrimitive& LevelSet)
				{
					return PointBoxDistanceSquared(Sphere.Center, LevelSet)
						<= FMath::Square(Sphere.Radius);
				});
		}
		case EElementPrimitiveKind::Box:
		{
			const FBoxPrimitive Box = MakeBox(Target, TargetOwner);
			return CalculateScaledBoxWeight(Influence,
				[&Box](const FBoxPrimitive& LevelSet)
				{
					return IntersectsBoxBox(LevelSet, Box);
				});
		}
		case EElementPrimitiveKind::Capsule:
		{
			const FCapsulePrimitive Capsule = MakeCapsule(Target, TargetOwner);
			return CalculateScaledBoxWeight(Influence,
				[&Capsule](const FBoxPrimitive& LevelSet)
				{
					return SegmentBoxDistanceSquared(Capsule.Start, Capsule.End, LevelSet)
						<= FMath::Square(Capsule.Radius);
				});
		}
		default:
			return 0.0;
		}
	}

	double IntersectingShapePairWeight(
		const FElementShape& Influence,
		const FTransform& InfluenceOwner,
		const FElementShape& Target,
		const FTransform& TargetOwner)
	{
		switch (Influence.Kind)
		{
		case EElementPrimitiveKind::Sphere:
		{
			const FSpherePrimitive Sphere = MakeSphere(Influence, InfluenceOwner);
			return CalculateLinearWeight(
				PointShapeDistance(Sphere.Center, Target, TargetOwner), Sphere.Radius);
		}
		case EElementPrimitiveKind::Box:
			return BoxShapeWeight(MakeBox(Influence, InfluenceOwner), Target, TargetOwner);
		case EElementPrimitiveKind::Capsule:
		{
			const FCapsulePrimitive Capsule = MakeCapsule(Influence, InfluenceOwner);
			return CalculateLinearWeight(SegmentShapeDistance(
				Capsule.Start, Capsule.End, Target, TargetOwner), Capsule.Radius);
		}
		default:
			return 0.0;
		}
	}

	void BuildBoxVertices(const FBoxPrimitive& Box, FVector (&OutVertices)[8])
	{
		const FVector AxisX = Box.Rotation.GetAxisX() * Box.Extent.X;
		const FVector AxisY = Box.Rotation.GetAxisY() * Box.Extent.Y;
		const FVector AxisZ = Box.Rotation.GetAxisZ() * Box.Extent.Z;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			OutVertices[Index] = Box.Center
				+ AxisX * ((Index & 1) != 0 ? 1.0 : -1.0)
				+ AxisY * ((Index & 2) != 0 ? 1.0 : -1.0)
				+ AxisZ * ((Index & 4) != 0 ? 1.0 : -1.0);
		}
	}

	double BoxBoxSurfaceDistance(const FBoxPrimitive& Left, const FBoxPrimitive& Right)
	{
		FVector LeftVertices[8];
		FVector RightVertices[8];
		BuildBoxVertices(Left, LeftVertices);
		BuildBoxVertices(Right, RightVertices);
		double MinimumSquared = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			MinimumSquared = FMath::Min(
				MinimumSquared, PointBoxDistanceSquared(LeftVertices[Index], Right));
			MinimumSquared = FMath::Min(
				MinimumSquared, PointBoxDistanceSquared(RightVertices[Index], Left));
		}

		// 两个分离 OBB 的最近点只可能是 Vertex-Face 或 Edge-Edge。
		// 固定 12 条边逐对比较，保持 Box 内核专用且不引入通用凸体/GJK 路径。
		constexpr int32 Edges[12][2] = {
			{0, 1}, {2, 3}, {4, 5}, {6, 7},
			{0, 2}, {1, 3}, {4, 6}, {5, 7},
			{0, 4}, {1, 5}, {2, 6}, {3, 7}};
		for (const int32* LeftEdge : Edges)
		{
			for (const int32* RightEdge : Edges)
			{
				MinimumSquared = FMath::Min(MinimumSquared, SegmentSegmentDistanceSquared(
					LeftVertices[LeftEdge[0]], LeftVertices[LeftEdge[1]],
					RightVertices[RightEdge[0]], RightVertices[RightEdge[1]]));
			}
		}
		return FMath::Sqrt(FMath::Max(MinimumSquared, 0.0));
	}

	double ShapePairSurfaceDistance(
		const FElementShape& Left,
		const FTransform& LeftOwner,
		const FElementShape& Right,
		const FTransform& RightOwner)
	{
		if (IntersectsShapes(Left, LeftOwner, Right, RightOwner)) return 0.0;
		if (Left.Kind == EElementPrimitiveKind::Sphere)
		{
			const FSpherePrimitive Sphere = MakeSphere(Left, LeftOwner);
			return FMath::Max(PointShapeDistance(Sphere.Center, Right, RightOwner) - Sphere.Radius, 0.0);
		}
		if (Right.Kind == EElementPrimitiveKind::Sphere)
		{
			const FSpherePrimitive Sphere = MakeSphere(Right, RightOwner);
			return FMath::Max(PointShapeDistance(Sphere.Center, Left, LeftOwner) - Sphere.Radius, 0.0);
		}
		if (Left.Kind == EElementPrimitiveKind::Capsule)
		{
			const FCapsulePrimitive Capsule = MakeCapsule(Left, LeftOwner);
			return FMath::Max(SegmentShapeDistance(
				Capsule.Start, Capsule.End, Right, RightOwner) - Capsule.Radius, 0.0);
		}
		if (Right.Kind == EElementPrimitiveKind::Capsule)
		{
			const FCapsulePrimitive Capsule = MakeCapsule(Right, RightOwner);
			return FMath::Max(SegmentShapeDistance(
				Capsule.Start, Capsule.End, Left, LeftOwner) - Capsule.Radius, 0.0);
		}
		return BoxBoxSurfaceDistance(MakeBox(Left, LeftOwner), MakeBox(Right, RightOwner));
	}

	double CalculateSmoothstepFalloff(const double Distance, const double FalloffDistance)
	{
		if (!FMath::IsFinite(Distance) || !FMath::IsFinite(FalloffDistance)
			|| FalloffDistance <= 0.0 || Distance >= FalloffDistance)
		{
			return 0.0;
		}
		const double X = FMath::Clamp(Distance / FalloffDistance, 0.0, 1.0);
		return 1.0 - X * X * (3.0 - 2.0 * X);
	}

	FTransform InterpolateTransform(const FTransform& Start, const FTransform& End, const double Alpha)
	{
		FTransform Result;
		Result.Blend(Start, End, static_cast<float>(Alpha));
		return Result;
	}

	bool ClipSegmentToBox(
		const FVector& Start,
		const FVector& End,
		const FBox& Box,
		double& OutEntry,
		double& OutExit)
	{
		OutEntry = 0.0;
		OutExit = 1.0;
		const FVector Direction = End - Start;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::IsNearlyZero(Direction[Axis]))
			{
				if (Start[Axis] < Box.Min[Axis] || Start[Axis] > Box.Max[Axis]) return false;
				continue;
			}
			double Near = (Box.Min[Axis] - Start[Axis]) / Direction[Axis];
			double Far = (Box.Max[Axis] - Start[Axis]) / Direction[Axis];
			if (Near > Far) Swap(Near, Far);
			OutEntry = FMath::Max(OutEntry, Near);
			OutExit = FMath::Min(OutExit, Far);
			if (OutEntry > OutExit) return false;
		}
		return OutExit >= 0.0 && OutEntry <= 1.0;
	}
}

bool FElementShapeKernels::Intersects(
	const FElementCompoundShape& Left,
	const FElementCompoundShape& Right)
{
	if (!Left.IsValid() || !Right.IsValid()
		|| !Left.CalculateWorldBounds().Intersect(Right.CalculateWorldBounds())) return false;
	for (const FElementShape& LeftShape : Left.Shapes)
	{
		for (const FElementShape& RightShape : Right.Shapes)
		{
			if (IntersectsShapes(LeftShape, Left.WorldTransform, RightShape, Right.WorldTransform)) return true;
		}
	}
	return false;
}

double FElementShapeKernels::CalculateWeight(
	const FElementCompoundShape& Influence,
	const FElementCompoundShape& Target,
	const EElementSpatialWeightMode Mode)
{
	if (!Influence.IsValid() || !Target.IsValid()
		|| Mode == EElementSpatialWeightMode::SurfaceDistanceFalloff
		|| !Influence.CalculateWorldBounds().Intersect(Target.CalculateWorldBounds())) return 0.0;
	double Weight = 0.0;
	for (const FElementShape& InfluenceShape : Influence.Shapes)
	{
		for (const FElementShape& TargetShape : Target.Shapes)
		{
			if (!IntersectsShapes(
				InfluenceShape, Influence.WorldTransform, TargetShape, Target.WorldTransform)) continue;
			if (Mode == EElementSpatialWeightMode::Uniform) return 1.0;
			Weight = FMath::Max(Weight, IntersectingShapePairWeight(
				InfluenceShape, Influence.WorldTransform, TargetShape, Target.WorldTransform));
		}
	}
	return Weight;
}

double FElementShapeKernels::CalculateSurfaceDistance(
	const FElementCompoundShape& Left,
	const FElementCompoundShape& Right)
{
	if (!Left.IsValid() || !Right.IsValid()) return TNumericLimits<double>::Max();
	double Minimum = TNumericLimits<double>::Max();
	for (const FElementShape& LeftShape : Left.Shapes)
	{
		for (const FElementShape& RightShape : Right.Shapes)
		{
			Minimum = FMath::Min(Minimum, ShapePairSurfaceDistance(
				LeftShape, Left.WorldTransform, RightShape, Right.WorldTransform));
			if (Minimum <= UE_DOUBLE_SMALL_NUMBER) return 0.0;
		}
	}
	return Minimum;
}

double FElementShapeKernels::CalculateSurfaceDistanceWeight(
	const FElementCompoundShape& Origin,
	const FElementCompoundShape& Target,
	const double FalloffDistanceCentimeters)
{
	return CalculateSmoothstepFalloff(
		CalculateSurfaceDistance(Origin, Target), FalloffDistanceCentimeters);
}

bool FElementShapeKernels::Sweep(
	const FElementCompoundShape& Influence,
	const FElementCompoundShape& TargetTemplate,
	const FElementMotionSegment& Segment,
	const EElementSpatialWeightMode Mode,
	FElementSweptShapeResult& OutResult,
	const FElementCompoundShape* FalloffOriginShape,
	const double FalloffDistanceCentimeters)
{
	OutResult = {};
	if (!Influence.IsValid() || !TargetTemplate.IsValid() || !Segment.IsValid()
		|| (Mode == EElementSpatialWeightMode::SurfaceDistanceFalloff
			&& (!FalloffOriginShape || !FalloffOriginShape->IsValid()
				|| !FMath::IsFinite(FalloffDistanceCentimeters)
				|| FalloffDistanceCentimeters <= 0.0))) return false;
	FElementCompoundShape StartTarget = TargetTemplate;
	StartTarget.WorldTransform = Segment.PreviousTransform;
	FElementCompoundShape EndTarget = TargetTemplate;
	EndTarget.WorldTransform = Segment.CurrentTransform;
	const FBox StartBounds = StartTarget.CalculateWorldBounds();
	const FBox EndBounds = EndTarget.CalculateWorldBounds();
	const FVector TargetExtent = StartBounds.GetExtent().ComponentMax(EndBounds.GetExtent());
	const FBox InfluenceBounds = Influence.CalculateWorldBounds().ExpandBy(TargetExtent);
	double BroadEntry = 0.0;
	double BroadExit = 1.0;
	if (!ClipSegmentToBox(StartBounds.GetCenter(), EndBounds.GetCenter(), InfluenceBounds, BroadEntry, BroadExit))
	{
		return false;
	}
	BroadEntry = FMath::Clamp(BroadEntry, 0.0, 1.0);
	BroadExit = FMath::Clamp(BroadExit, 0.0, 1.0);
	constexpr int32 Samples = 17;
	int32 FirstContact = INDEX_NONE;
	int32 LastContact = INDEX_NONE;
	double WeightSum = 0.0;
	double MaximumWeight = 0.0;
	double EndWeight = 0.0;
	for (int32 Sample = 0; Sample < Samples; ++Sample)
	{
		const double LocalAlpha = static_cast<double>(Sample) / static_cast<double>(Samples - 1);
		const double Alpha = FMath::Lerp(BroadEntry, BroadExit, LocalAlpha);
		FElementCompoundShape Target = TargetTemplate;
		Target.WorldTransform = InterpolateTransform(
			Segment.PreviousTransform, Segment.CurrentTransform, Alpha);
		const double Weight = Mode == EElementSpatialWeightMode::SurfaceDistanceFalloff
			? CalculateSurfaceDistanceWeight(*FalloffOriginShape, Target, FalloffDistanceCentimeters)
			: CalculateWeight(Influence, Target, Mode);
		if (Weight <= 0.0) continue;
		if (FirstContact == INDEX_NONE) FirstContact = Sample;
		LastContact = Sample;
		WeightSum += Weight;
		MaximumWeight = FMath::Max(MaximumWeight, Weight);
		if (Sample == Samples - 1 || FMath::IsNearlyEqual(Alpha, 1.0)) EndWeight = Weight;
	}
	if (FirstContact == INDEX_NONE) return false;
	const double SegmentSeconds = static_cast<double>(
		Segment.EndTimeMilliseconds - Segment.StartTimeMilliseconds) / 1000.0;
	const double BroadSpan = BroadExit - BroadEntry;
	const double SampleSpan = 1.0 / static_cast<double>(Samples - 1);
	const double ContactFraction = FMath::Clamp(
		(static_cast<double>(LastContact - FirstContact) + 1.0) * SampleSpan * BroadSpan, 0.0, 1.0);
	OutResult.ContactDurationSeconds = SegmentSeconds * ContactFraction;
	const int32 ContactSamples = LastContact - FirstContact + 1;
	OutResult.IntegratedWeightSeconds = OutResult.ContactDurationSeconds
		* WeightSum / static_cast<double>(ContactSamples);
	OutResult.MaximumWeight = MaximumWeight;
	OutResult.EndWeight = EndWeight;
	return true;
}
