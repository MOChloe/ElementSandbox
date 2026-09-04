#pragma once

#include "Collision/BuildCollisionProcessor.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildEntityWorldIndex.h"
#include "Placement/BuildPlacementGeometry.h"
#include "Snapshot/BuildQuerySnapshotStream.h"
#include "PresentationWorldSubsystem.h"
#include "Processing/BuildProcessorScheduler.h"
#include "Rendering/BuildRenderDirtySet.h"
#include "Rendering/BuildRenderProcessor.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Storage/BuildingPersistenceExtension.h"
#include "WorldStorageSubsystem.h"

class UBuildingDefinition;

/** Building ECS 真值、空间索引、中性查询快照流与 Gameplay Processor。 */
struct FBuildingCoreState final
{
	FBuildingCoreState() : SpatialIndex(SpatialConfig) {}

	FBuildSpatialIndexConfig SpatialConfig;
	FBuildEntityRegistry Registry;
	FBuildSpatialIndex SpatialIndex;
	mutable FBuildPlacementGeometryCache PlacementGeometry;
	FBuildQuerySnapshotStream QuerySnapshots;
	FBuildProcessorScheduler ProcessorScheduler;
	FBuildEntityWorldIndex EntityByWorldEntityId;
	TMap<FName, TWeakObjectPtr<UBuildingDefinition>> DefinitionById;
	TMap<FName, FBuildDefinitionEntityUpsertedEvent> DefinitionEntityUpsertedEvents;
};

/** Collision 与 Render 都是 ECS 的可丢弃投影，共享观察源但不拥有 Gameplay 真值。 */
struct FBuildingPresentationState final
{
	FBuildingPresentationState(const FBuildPresentationResidencyConfig& ResidencyConfig,
							   const FBuildRenderClusterConfig& ClusterConfig)
		: RenderProcessor(ResidencyConfig, ClusterConfig)
	{
	}

	FBuildCollisionProcessor CollisionProcessor;
	FBuildRenderDirtySet RenderDirtySet;
	FBuildRenderProcessor RenderProcessor;
	FMeshPoolLayerHandle Layer;
	FPresentationProjectorHandle Projector;
	FDelegateHandle MeshPoolInstanceRetiredHandle;
	FPresentationViewSnapshot LastViews;
	TArray<FBuildEntityHandle> RenderCustomDataDirtyEntities;
	TSet<FBuildEntityHandle> RenderCustomDataDirtySet;
	double LastRenderFlushMilliseconds = 0.0;
	bool bLastProjectionSucceeded = true;
	bool bRenderFlushFailureLogged = false;
	bool bCollisionFlushFailureLogged = false;
};

/** Building Chunk Section 的注册表及 WorldStorage Adapter 生命周期。 */
struct FBuildingPersistenceState final
{
	TWeakObjectPtr<UWorldStorageSubsystem> WorldStorage;
	TMap<FName, TSharedRef<IBuildingPersistenceExtension>> Extensions;
	TArray<FName> ExtensionOrder;
	TSharedPtr<IWorldStorageDomainAdapter> Adapter;
};

struct FBuildingLoopState final
{
	FDelegateHandle PostActorTickHandle;
	double AuthorityTickAccumulator = 0.0;
};

/** 薄协调对象；Gameplay、表现、持久化与跨域转换以组合表达。 */
class FBuildingWorldRuntime final
{
public:
	FBuildingWorldRuntime();

	FBuildingCoreState Core;
	FBuildingPresentationState Presentation;
	FBuildingPersistenceState Persistence;
	FBuildingLoopState Loop;
};
