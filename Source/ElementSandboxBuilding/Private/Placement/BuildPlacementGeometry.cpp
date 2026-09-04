#include "Placement/BuildPlacementGeometry.h"

#include "Chaos/CastingUtilities.h"
#include "Chaos/GeometryQueries.h"
#include "Chaos/ImplicitObject.h"
#include "Chaos/ShapeInstance.h"
#include "Definition/BuildingDefinition.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Physics/Experimental/ChaosInterfaceUtils.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Physics/PhysicsInterfaceTypes.h"
#include "PhysicsEngine/BodySetup.h"

namespace
{
	FTransform MakeRigidTransform(const FTransform& Transform)
	{
		return FTransform(Transform.GetRotation(), Transform.GetLocation());
	}

	Chaos::FRigidTransform3 MakeChaosRigidTransform(const FTransform& Transform)
	{
		return Chaos::FRigidTransform3(
			Transform.GetLocation(),
			Transform.GetRotation());
	}

	FVector MakeCachedToleranceScale(
		const FBox& WorldBounds,
		const FVector& OriginalScale,
		const double Tolerance)
	{
		const FVector Extent = WorldBounds.GetExtent();
		FVector Result = OriginalScale;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Factor = Extent[Axis] > UE_SMALL_NUMBER
				? FMath::Clamp(1.0 - Tolerance / Extent[Axis], 0.01, 1.0)
				: 1.0;
			Result[Axis] *= Factor;
		}
		return Result;
	}
}

bool FBuildPlacementGeometry::Raycast(
	const FTransform& RigidWorldTransform,
	const FVector& Start,
	const FVector& End,
	FVector& OutLocation,
	FVector& OutNormal,
	double& OutDistance) const
{
	OutLocation = FVector::ZeroVector;
	OutNormal = FVector::ZeroVector;
	OutDistance = TNumericLimits<double>::Max();
	if (!IsValid()
		|| RigidWorldTransform.ContainsNaN()
		|| Start.ContainsNaN()
		|| End.ContainsNaN())
	{
		return false;
	}

	const FTransform RigidTransform = MakeRigidTransform(RigidWorldTransform);
	const FVector Delta = End - Start;
	const double Length = Delta.Length();
	if (!FMath::IsFinite(Length) || Length <= UE_SMALL_NUMBER)
	{
		return false;
	}
	const FVector Direction = Delta / Length;
	const FVector LocalStart = RigidTransform.InverseTransformPosition(Start);
	const FVector LocalDirection = RigidTransform.InverseTransformVectorNoScale(Direction);

	bool bHit = false;
	for (const Chaos::FImplicitObjectPtr& Shape : Shapes)
	{
		if (!Shape)
		{
			continue;
		}
		Chaos::FReal HitTime = 0.0;
		Chaos::FVec3 LocalLocation(0.0);
		Chaos::FVec3 LocalNormal(0.0);
		int32 FaceIndex = INDEX_NONE;
		if (Shape->Raycast(
				LocalStart,
				LocalDirection,
				Length,
				0.0,
				HitTime,
				LocalLocation,
				LocalNormal,
				FaceIndex)
			&& HitTime >= 0.0
			&& HitTime < OutDistance)
		{
			bHit = true;
			OutDistance = HitTime;
			OutLocation = RigidTransform.TransformPosition(LocalLocation);
			OutNormal = RigidTransform.TransformVectorNoScale(LocalNormal).GetSafeNormal();
		}
	}
	return bHit;
}

bool FBuildPlacementGeometry::Overlaps(
	const FTransform& RigidWorldTransform,
	const FBuildPlacementGeometry& Other,
	const FTransform& OtherRigidWorldTransform) const
{
	if (!IsValid() || !Other.IsValid()
		|| RigidWorldTransform.ContainsNaN()
		|| OtherRigidWorldTransform.ContainsNaN())
	{
		return false;
	}
	const Chaos::FRigidTransform3 TransformA = MakeChaosRigidTransform(
		MakeRigidTransform(RigidWorldTransform));
	const Chaos::FRigidTransform3 TransformB = MakeChaosRigidTransform(
		MakeRigidTransform(OtherRigidWorldTransform));
	for (const Chaos::FImplicitObjectPtr& ShapeA : Shapes)
	{
		if (!ShapeA)
		{
			continue;
		}
		for (const Chaos::FImplicitObjectPtr& ShapeB : Other.Shapes)
		{
			if (ShapeB && Chaos::Utilities::CastHelper(
				*ShapeB,
				TransformB,
				[&ShapeA, &TransformA](const auto& ConcreteB, const auto& FullTransformB)
				{
					return Chaos::OverlapQuery(
						*ShapeA,
						TransformA,
						ConcreteB,
						FullTransformB);
				}))
			{
				return true;
			}
		}
	}
	return false;
}

bool FBuildPlacementGeometry::OverlapsWorld(
	UWorld& World,
	const FTransform& RigidWorldTransform,
	const FCollisionQueryParams& QueryParams,
	const FCollisionObjectQueryParams& ObjectParams) const
{
	if (!IsValid() || RigidWorldTransform.ContainsNaN())
	{
		return false;
	}
	TArray<FOverlapResult> Overlaps;
	for (const Chaos::FImplicitObjectPtr& Shape : Shapes)
	{
		Overlaps.Reset();
		if (Shape && FPhysicsInterface::GeomOverlapMulti(
			&World,
			*Shape,
			RigidWorldTransform.GetLocation(),
			RigidWorldTransform.GetRotation(),
			Overlaps,
			ECC_Visibility,
			QueryParams,
			FCollisionResponseParams::DefaultResponseParam,
			ObjectParams))
		{
			return true;
		}
	}
	return false;
}

bool FBuildPlacementGeometryCache::CacheDefinition(
	const UBuildingDefinition& Definition,
	const double PenetrationTolerance)
{
	if (!Definition.HasValidCollisionDefinition()
		|| !FMath::IsFinite(PenetrationTolerance)
		|| PenetrationTolerance < 0.0)
	{
		return false;
	}
	for (int32 PartId = 0; PartId < Definition.CollisionParts.Num(); ++PartId)
	{
		FTransform PartWorldTransform;
		FBox PartWorldBounds(ForceInit);
		if (!Definition.TryCalculateCollisionPartWorldTransform(
				PartId,
				FTransform::Identity,
				{},
				PartWorldTransform)
			|| !Definition.TryCalculateCollisionPartWorldBounds(
				PartId,
				FTransform::Identity,
				{},
				PartWorldBounds))
		{
			return false;
		}
		UStaticMesh* Mesh = Definition.CollisionParts[PartId].CollisionMesh;
		const FVector TargetScale = PartWorldTransform.GetScale3D();
		const FVector CandidateScale = MakeCachedToleranceScale(
			PartWorldBounds,
			TargetScale,
			PenetrationTolerance);
		if (!Mesh
			|| !FindOrAdd(*Mesh, TargetScale)
			|| !FindOrAdd(*Mesh, CandidateScale))
		{
			return false;
		}
	}
	return true;
}

const FBuildPlacementGeometry* FBuildPlacementGeometryCache::FindOrAdd(
	UStaticMesh& Mesh,
	const FVector& Scale)
{
	check(IsInGameThread());
	if (Scale.ContainsNaN()
		|| !FMath::IsFinite(Scale.X)
		|| !FMath::IsFinite(Scale.Y)
		|| !FMath::IsFinite(Scale.Z)
		|| Scale.GetAbsMin() <= UE_SMALL_NUMBER)
	{
		return nullptr;
	}
	const FKey Key{&Mesh, Scale};
	if (const TUniquePtr<FBuildPlacementGeometry>* Existing = Entries.Find(Key))
	{
		return Existing->Get();
	}

	UBodySetup* BodySetup = Mesh.GetBodySetup();
	if (!BodySetup || BodySetup->AggGeom.GetElementCount() == 0)
	{
		return nullptr;
	}
	FGeometryAddParams Params{};
	Params.bDoubleSided = false;
	Params.CollisionData.CollisionFlags.bEnableQueryCollision = true;
	Params.CollisionTraceType = CTF_UseSimpleAsComplex;
	Params.Scale = Scale;
	Params.SimpleMaterial = nullptr;
	Params.LocalTransform = FTransform::Identity;
	Params.WorldTransform = FTransform::Identity;
	Params.Geometry = &BodySetup->AggGeom;

	TArray<Chaos::FImplicitObjectPtr> Shapes;
	Chaos::FShapesArray ShapeData;
	ChaosInterface::CreateGeometry(Params, Shapes, ShapeData);
	if (Shapes.Num() != ShapeData.Num())
	{
		return nullptr;
	}
	for (int32 Index = Shapes.Num() - 1; Index >= 0; --Index)
	{
		if (!Shapes[Index] || !ShapeData[Index] || !ShapeData[Index]->GetQueryEnabled())
		{
			Shapes.RemoveAt(Index, 1, EAllowShrinking::No);
		}
	}
	if (Shapes.IsEmpty())
	{
		return nullptr;
	}

	TUniquePtr<FBuildPlacementGeometry> Geometry = MakeUnique<FBuildPlacementGeometry>();
	Geometry->Shapes = MoveTemp(Shapes);
	const FBuildPlacementGeometry* Result = Geometry.Get();
	Entries.Add(Key, MoveTemp(Geometry));
	return Result;
}
