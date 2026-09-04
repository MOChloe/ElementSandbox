#pragma once

#include "CoreMinimal.h"

class ABuildCollisionHost;
class FBuildEntityRegistry;
class FBuildPlacementGeometryCache;
class FBuildSpatialIndex;
class UBuildingDefinition;
class UWorld;
struct FBuildPlacementEvaluation;

class FBuildPlacementEvaluator final
{
public:
	static bool Evaluate(
		UWorld& World,
		const UBuildingDefinition& Definition,
		const FTransform& CandidateTransform,
		const FVector& BuilderLocation,
		double MaximumDistance,
		double PenetrationTolerance,
		const FBuildEntityRegistry& Registry,
		const FBuildSpatialIndex& SpatialIndex,
		FBuildPlacementGeometryCache& GeometryCache,
		const ABuildCollisionHost* CollisionHost,
		FBuildPlacementEvaluation& OutEvaluation);
};
