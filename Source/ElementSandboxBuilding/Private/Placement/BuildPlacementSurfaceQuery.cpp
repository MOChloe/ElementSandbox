#include "Placement/BuildPlacementSurfaceQuery.h"

#include "Definition/BuildingDefinition.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Placement/BuildPlacementGeometry.h"
#include "Placement/BuildPlacementTypes.h"
#include "Spatial/BuildSpatialIndex.h"

bool FBuildPlacementSurfaceQuery::QueryBuilding(
	const FVector& Start,
	const FVector& End,
	const FBuildEntityRegistry& Registry,
	const FBuildSpatialIndex& SpatialIndex,
	FBuildPlacementGeometryCache& GeometryCache,
	FBuildPlacementSurfaceHit& OutHit)
{
	OutHit = {};
	if (Start.ContainsNaN() || End.ContainsNaN()
		|| FVector::DistSquared(Start, End) <= UE_SMALL_NUMBER)
	{
		return false;
	}

	FBox RayBounds(ForceInit);
	RayBounds += Start;
	RayBounds += End;
	RayBounds = RayBounds.ExpandBy(0.1);
	FBuildSpatialQueryScratch Scratch;
	TArray<FBuildEntityHandle> Candidates;
	SpatialIndex.QueryOverlaps(RayBounds, Scratch, Candidates);
	for (const FBuildEntityHandle Entity : Candidates)
	{
		const FBuildDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(Entity);
		const FBuildTransformFragment* Transform =
			Registry.FindFragment<FBuildTransformFragment>(Entity);
		const FBuildPartTransformFragment* PartTransforms =
			Registry.FindFragment<FBuildPartTransformFragment>(Entity);
		const UBuildingDefinition* Definition = DefinitionFragment
			? DefinitionFragment->Definition.Get()
			: nullptr;
		if (!Definition || !Transform)
		{
			continue;
		}
		const TConstArrayView<FTransform> LocalTransforms = PartTransforms
			? TConstArrayView<FTransform>(PartTransforms->LocalTransforms)
			: TConstArrayView<FTransform>();
		for (int32 PartId = 0; PartId < Definition->CollisionParts.Num(); ++PartId)
		{
			FTransform PartWorldTransform;
			if (!Definition->TryCalculateCollisionPartWorldTransform(
					PartId,
					Transform->WorldTransform,
					LocalTransforms,
					PartWorldTransform))
			{
				continue;
			}
			UStaticMesh* Mesh = Definition->CollisionParts[PartId].CollisionMesh;
			const FBuildPlacementGeometry* Geometry = Mesh
				? GeometryCache.FindOrAdd(*Mesh, PartWorldTransform.GetScale3D())
				: nullptr;
			FVector Location;
			FVector Normal;
			double Distance = TNumericLimits<double>::Max();
			if (Geometry
				&& Geometry->Raycast(
					PartWorldTransform,
					Start,
					End,
					Location,
					Normal,
					Distance)
				&& Distance < OutHit.Distance)
			{
				OutHit.Location = Location;
				OutHit.Normal = Normal;
				OutHit.Distance = Distance;
				OutHit.bBuildingSurface = true;
			}
		}
	}
	return OutHit.IsValid();
}
