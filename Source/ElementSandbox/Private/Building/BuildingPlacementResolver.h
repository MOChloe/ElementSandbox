#pragma once

#include "CoreMinimal.h"
#include "Placement/BuildPlacementTypes.h"

class AActor;
class UBuildingDefinition;
class UBuildingWorldSubsystem;
class UWorld;

/**
 * 建造意图的共享规则入口。客户端预览与服务器裁决都从原始期望位置重新
 * 查询 Building ECS + UE World 支撑面并执行同一套占用判定；它不持有玩家或预览生命周期。
 */
struct FBuildingPlacementResolver final
{
	static constexpr double MaximumDistance = 500.0;
	static bool ResolveCandidateTransform(
		UWorld& World,
		UBuildingWorldSubsystem& BuildingSubsystem,
		const UBuildingDefinition& Definition,
		const FVector& SurfaceLocation,
		const FTransform& PlacementShapeTransform,
		uint8 YawQuarterTurns,
		const AActor* IgnoredActor,
		FBuildPlacementEvaluation& OutEvaluation);

	static bool ResolveIntent(
		UWorld& World,
		UBuildingWorldSubsystem& BuildingSubsystem,
		const UBuildingDefinition& Definition,
		const FVector& SurfaceLocation,
		const FVector& ExpectedResolvedLocation,
		const FTransform& PlacementShapeTransform,
		uint8 YawQuarterTurns,
		const FVector& BuilderLocation,
		const AActor* IgnoredActor,
		FBuildPlacementEvaluation& OutEvaluation);
};
