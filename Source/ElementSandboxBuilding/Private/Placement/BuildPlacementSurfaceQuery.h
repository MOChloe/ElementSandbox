#pragma once

#include "CoreMinimal.h"

class FBuildEntityRegistry;
class FBuildPlacementGeometryCache;
class FBuildSpatialIndex;
struct FBuildPlacementSurfaceHit;

/** Building ECS 空间树上的确定性 Simple Collision 射线查询。 */
class FBuildPlacementSurfaceQuery final
{
public:
	static bool QueryBuilding(
		const FVector& Start,
		const FVector& End,
		const FBuildEntityRegistry& Registry,
		const FBuildSpatialIndex& SpatialIndex,
		FBuildPlacementGeometryCache& GeometryCache,
		FBuildPlacementSurfaceHit& OutHit);
};
