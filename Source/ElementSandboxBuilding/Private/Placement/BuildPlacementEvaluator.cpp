#include "Placement/BuildPlacementEvaluator.h"

#include "Collision/BuildCollisionHost.h"
#include "Definition/BuildingDefinition.h"
#include "Engine/World.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Placement/BuildPlacementGeometry.h"
#include "Placement/BuildPlacementTypes.h"
#include "Spatial/BuildSpatialIndex.h"
#include "WorldObjectWorldSubsystem.h"

namespace
{
	FVector MakeToleranceScale(
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

	FBox MakeToleranceBounds(const FBox& Bounds, const double Tolerance)
	{
		const FVector Center = Bounds.GetCenter();
		const FVector OriginalExtent = Bounds.GetExtent();
		const FVector ShrunkExtent(
			FMath::Max(OriginalExtent.X - Tolerance, UE_SMALL_NUMBER),
			FMath::Max(OriginalExtent.Y - Tolerance, UE_SMALL_NUMBER),
			FMath::Max(OriginalExtent.Z - Tolerance, UE_SMALL_NUMBER));
		return FBox(Center - ShrunkExtent, Center + ShrunkExtent);
	}

	struct FPreparedPlacementPart final
	{
		const FBuildPlacementGeometry* Geometry = nullptr;
		FTransform RigidWorldTransform = FTransform::Identity;
		FBox WorldBounds = FBox(ForceInit);
	};

	bool TryPreparePart(
		FBuildPlacementGeometryCache& GeometryCache,
		const UBuildingDefinition& Definition,
		const int32 CollisionPartId,
		const FTransform& EntityTransform,
		const TConstArrayView<FTransform> PartLocalTransforms,
		const double Tolerance,
		FPreparedPlacementPart& OutPart)
	{
		OutPart = {};
		FTransform PartWorldTransform;
		if (!Definition.CollisionParts.IsValidIndex(CollisionPartId)
			|| !Definition.TryCalculateCollisionPartWorldTransform(
				CollisionPartId,
				EntityTransform,
				PartLocalTransforms,
				PartWorldTransform)
			|| !Definition.TryCalculateCollisionPartWorldBounds(
				CollisionPartId,
				EntityTransform,
				PartLocalTransforms,
				OutPart.WorldBounds))
		{
			return false;
		}

		UStaticMesh* Mesh =
			Definition.CollisionParts[CollisionPartId].CollisionMesh;
		if (!Mesh)
		{
			return false;
		}
		FVector GeometryScale = PartWorldTransform.GetScale3D();
		if (Tolerance > 0.0)
		{
			GeometryScale = MakeToleranceScale(
				OutPart.WorldBounds,
				GeometryScale,
				Tolerance);
		}
		OutPart.Geometry = GeometryCache.FindOrAdd(*Mesh, GeometryScale);
		OutPart.RigidWorldTransform = FTransform(
			PartWorldTransform.GetRotation(),
			PartWorldTransform.GetLocation());
		return OutPart.Geometry != nullptr;
	}
}

bool FBuildPlacementEvaluator::Evaluate(
	UWorld& World,
	const UBuildingDefinition& Definition,
	const FTransform& CandidateTransform,
	const FVector& BuilderLocation,
	const double MaximumDistance,
	const double PenetrationTolerance,
	const FBuildEntityRegistry& Registry,
	const FBuildSpatialIndex& SpatialIndex,
	FBuildPlacementGeometryCache& GeometryCache,
	const ABuildCollisionHost* CollisionHost,
	FBuildPlacementEvaluation& OutEvaluation)
{
	OutEvaluation = {};
	OutEvaluation.ResolvedTransform = CandidateTransform;
	OutEvaluation.SpatialRevision = SpatialIndex.GetQueryRevision();
	const FVector CandidateScale = CandidateTransform.GetScale3D();
	if (!Definition.HasValidDefinitionId()
		|| !Definition.HasValidCollisionDefinition()
		|| CandidateTransform.ContainsNaN()
		|| !CandidateTransform.GetRotation().IsNormalized()
		|| !FMath::IsFinite(CandidateScale.X)
		|| !FMath::IsFinite(CandidateScale.Y)
		|| !FMath::IsFinite(CandidateScale.Z)
		|| CandidateScale.X <= UE_SMALL_NUMBER
		|| CandidateScale.Y <= UE_SMALL_NUMBER
		|| CandidateScale.Z <= UE_SMALL_NUMBER)
	{
		OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
		return false;
	}
	// Decorative Definition 明确可以没有 Collision Part。它仍参与 ECS Bounds、
	// WorldObject 宽相和持久化，但不能被偷偷解释成 Solid，也不做不存在的 Body 查询。
	if (!FMath::IsFinite(MaximumDistance) || MaximumDistance < 0.0
		|| FVector::DistSquared(BuilderLocation, CandidateTransform.GetLocation())
			> FMath::Square(MaximumDistance))
	{
		OutEvaluation.Failure = EBuildPlacementFailure::OutOfRange;
		return true;
	}

	FBox CandidateBounds(ForceInit);
	if (!Definition.TryCalculateWorldBounds(CandidateTransform, CandidateBounds))
	{
		OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
		return false;
	}
	TArray<FPreparedPlacementPart, TInlineAllocator<4>> CandidateParts;
	CandidateParts.Reserve(Definition.CollisionParts.Num());
	for (int32 CandidatePartId = 0;
		CandidatePartId < Definition.CollisionParts.Num();
		++CandidatePartId)
	{
		FPreparedPlacementPart& CandidatePart = CandidateParts.AddDefaulted_GetRef();
		if (!TryPreparePart(
			GeometryCache,
			Definition,
			CandidatePartId,
			CandidateTransform,
			{},
			FMath::Max(0.0, PenetrationTolerance),
			CandidatePart))
		{
			OutEvaluation.Failure = EBuildPlacementFailure::InvalidTransform;
			return false;
		}
	}

	FBuildSpatialQueryScratch Scratch;
	TArray<FBuildEntityHandle> NearbyEntities;
	SpatialIndex.QueryOverlaps(CandidateBounds, Scratch, NearbyEntities);
	OutEvaluation.CandidateEntityCount = NearbyEntities.Num();
	for (const FBuildEntityHandle OtherEntity : NearbyEntities)
	{
		const FBuildDefinitionFragment* OtherDefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(OtherEntity);
		const FBuildTransformFragment* OtherTransform =
			Registry.FindFragment<FBuildTransformFragment>(OtherEntity);
		const FBuildPartTransformFragment* OtherParts =
			Registry.FindFragment<FBuildPartTransformFragment>(OtherEntity);
		const UBuildingDefinition* OtherDefinition = OtherDefinitionFragment
			? OtherDefinitionFragment->Definition.Get()
			: nullptr;
		if (!OtherDefinition || !OtherTransform)
		{
			continue;
		}
		const TConstArrayView<FTransform> OtherPartTransforms = OtherParts
			? TConstArrayView<FTransform>(OtherParts->LocalTransforms)
			: TConstArrayView<FTransform>();

		for (const FPreparedPlacementPart& CandidatePart : CandidateParts)
		{
			for (int32 OtherPartId = 0;
				OtherPartId < OtherDefinition->CollisionParts.Num();
				++OtherPartId)
			{
				++OutEvaluation.TestedPartPairCount;
				FPreparedPlacementPart OtherPart;
				if (!TryPreparePart(
					GeometryCache,
					*OtherDefinition,
					OtherPartId,
					OtherTransform->WorldTransform,
					OtherPartTransforms,
					0.0,
					OtherPart))
				{
					continue;
				}
				if (!CandidatePart.WorldBounds.Intersect(OtherPart.WorldBounds))
				{
					continue;
				}
				OutEvaluation.TestedShapePairCount +=
					CandidatePart.Geometry->GetShapeCount()
					* OtherPart.Geometry->GetShapeCount();
				if (CandidatePart.Geometry->Overlaps(
					CandidatePart.RigidWorldTransform,
					*OtherPart.Geometry,
					OtherPart.RigidWorldTransform))
				{
					OutEvaluation.Failure = EBuildPlacementFailure::BlockedByBuilding;
					return true;
				}
			}
		}
	}

	// PermanentStatic WorldObject 可能没有 Actor/PrimitiveComponent（例如树木），
	// 因此不能只依赖下面的 UE Scene Overlap。它们用自己的双空间索引做占用
	// 宽相；当前 WorldObject 没有通用 Collision Shape 契约，精确边界就是其
	// Definition/实例 Bounds。候选边界收缩同一容差，使相切仍然合法。
	if (const UWorldObjectWorldSubsystem* WorldObjects =
		World.GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		FWorldObjectSpatialQueryScratch WorldObjectScratch;
		TArray<FWorldObjectEntityHandle> WorldObjectCandidates;
		WorldObjects->QueryOverlap(
			MakeToleranceBounds(
				CandidateBounds,
				FMath::Max(0.0, PenetrationTolerance)),
			WorldObjectScratch,
			WorldObjectCandidates);
		OutEvaluation.WorldObjectCandidateCount = WorldObjectCandidates.Num();
		if (!WorldObjectCandidates.IsEmpty())
		{
			OutEvaluation.Failure = EBuildPlacementFailure::BlockedByWorld;
			return true;
		}
	}

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjectParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjectParams.AddObjectTypesToQuery(ECC_Vehicle);
	ObjectParams.AddObjectTypesToQuery(ECC_Destructible);
	FCollisionQueryParams QueryParams(TEXT("BuildingPlacementWorldOverlap"), false);
	if (IsValid(CollisionHost))
	{
		QueryParams.AddIgnoredActor(CollisionHost);
	}
	for (const FPreparedPlacementPart& CandidatePart : CandidateParts)
	{
		if (CandidatePart.Geometry->OverlapsWorld(
			World,
			CandidatePart.RigidWorldTransform,
			QueryParams,
			ObjectParams))
		{
			OutEvaluation.Failure = EBuildPlacementFailure::BlockedByWorld;
			return true;
		}
	}

	OutEvaluation.Failure = EBuildPlacementFailure::None;
	return true;
}
