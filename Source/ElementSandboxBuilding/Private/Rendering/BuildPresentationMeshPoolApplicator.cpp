#include "Rendering/BuildPresentationMeshPoolApplicator.h"

#include "Definition/BuildingDefinition.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildRenderCustomDataFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "PresentationWorldSubsystem.h"
#include "Rendering/BuildPresentationIndex.h"

namespace
{
	bool TryResolveEntity(
		const FBuildEntityRegistry& Registry,
		const FBuildEntityHandle Entity,
		const FBuildTransformFragment*& OutTransform,
		const UBuildingDefinition*& OutDefinition,
		const FBuildPartTransformFragment*& OutPartTransforms)
	{
		OutTransform = Registry.FindFragment<FBuildTransformFragment>(Entity);
		const FBuildDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(Entity);
		OutDefinition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
		OutPartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Entity);
		return OutTransform && OutDefinition
			&& (!OutPartTransforms
				|| OutPartTransforms->LocalTransforms.Num() == OutDefinition->MeshParts.Num());
	}

	bool TryResolvePartTransform(
		const FBuildTransformFragment& Transform,
		const UBuildingDefinition& Definition,
		const FBuildPartTransformFragment* PartTransforms,
		const int32 PartId,
		FTransform& OutTransform)
	{
		if (!Definition.MeshParts.IsValidIndex(PartId))
		{
			return false;
		}
		const FTransform& Local = PartTransforms
			? PartTransforms->LocalTransforms[PartId]
			: Definition.MeshParts[PartId].LocalTransform;
		OutTransform = Local * Transform.WorldTransform;
		return !OutTransform.ContainsNaN();
	}

	bool TryResolveCustomData(
		const FBuildEntityRegistry& Registry,
		const FBuildEntityHandle Entity,
		const int32 Count,
		TArray<float, TInlineAllocator<4>>& OutData)
	{
		if (Count < 0 || Count > 8)
		{
			return false;
		}
		OutData.Init(0.0f, Count);
		const FBuildRenderCustomDataFragment* Fragment =
			Registry.FindFragment<FBuildRenderCustomDataFragment>(Entity);
		if (!Fragment)
		{
			return true;
		}
		for (int32 Index = 0; Index < Count && Index < Fragment->Values.Num(); ++Index)
		{
			if (!FMath::IsFinite(Fragment->Values[Index]))
			{
				return false;
			}
			OutData[Index] = Fragment->Values[Index];
		}
		return true;
	}

	EBuildRenderStorageClass ResolveStorageClass(
		const FBuildMeshPartDefinition& Part,
		const bool bHot)
	{
		return Part.PresentationPolicy == EBuildMeshPartPresentationPolicy::ProximityPromotable
			? (bHot ? EBuildRenderStorageClass::HotISM : EBuildRenderStorageClass::ColdPromotableHISM)
			: EBuildRenderStorageClass::StaticHISM;
	}

	FMeshPoolClusterKey MakeClusterKey(
		const FMeshPoolLayerHandle Layer,
		const FBuildMeshPartDefinition& Part,
		const FTransform& WorldTransform,
		const EBuildRenderStorageClass StorageClass,
		const FBuildPresentationResidencyConfig& ResidencyConfig,
		const FBuildRenderClusterConfig& ClusterConfig)
	{
		FMeshPoolClusterKey Key;
		Key.Layer = Layer;
		Key.Mesh = Part.Mesh;
		Key.MaterialOverride = Part.MaterialOverride;
		Key.CustomDataFloatCount = Part.CustomDataFloatCount;
		Key.Backend = StorageClass == EBuildRenderStorageClass::HotISM
			? EMeshPoolBackend::ImmediateMovable
			: EMeshPoolBackend::HierarchicalStatic;
		const double CellSize = Key.Backend == EMeshPoolBackend::ImmediateMovable
			? ResidencyConfig.GameplayChunkSize
			: ClusterConfig.StaticCellSize;
		if (!TryGetPresentationGridCoordinate(WorldTransform.GetLocation(), CellSize, Key.Cell))
		{
			Key.Mesh = nullptr;
		}
		return Key;
	}

	FBuildPresentationAppliedPart* FindAppliedPart(
			const TArrayView<FBuildPresentationAppliedPart> Parts,
			const int32 PartId)
	{
		for (FBuildPresentationAppliedPart& Part : Parts)
		{
			if (Part.PartId == PartId)
			{
				return &Part;
			}
		}
		return nullptr;
	}
}

int32 FBuildPresentationMeshPoolApplicator::CountMeshParts(const UBuildingDefinition& Definition)
{
	int32 Count = 0;
	for (const FBuildMeshPartDefinition& Part : Definition.MeshParts)
	{
		Count += Part.Mesh ? 1 : 0;
	}
	return Count;
}

int32 FBuildPresentationMeshPoolApplicator::CountPromotableMeshParts(const UBuildingDefinition& Definition)
{
	int32 Count = 0;
	for (const FBuildMeshPartDefinition& Part : Definition.MeshParts)
	{
		Count += Part.Mesh
			&& Part.PresentationPolicy == EBuildMeshPartPresentationPolicy::ProximityPromotable ? 1 : 0;
	}
	return Count;
}

bool FBuildPresentationMeshPoolApplicator::QueueAddEntitySlice(
	const FBuildEntityRegistry& Registry,
	UPresentationWorldSubsystem& Presentation,
	const FMeshPoolLayerHandle Layer,
	const FBuildEntityHandle Entity,
	const bool bHot,
	const FBuildPresentationResidencyConfig& ResidencyConfig,
	const FBuildRenderClusterConfig& ClusterConfig,
	const int32 NextPartId,
	const int32 MaximumMeshParts,
	const double DeadlineSeconds,
	int32& OutNextPartId,
	bool& OutComplete,
	TArray<FBuildPresentationAppliedPart>& OutParts)
{
	OutParts.Reset();
	OutNextPartId = NextPartId;
	OutComplete = false;
	const FBuildTransformFragment* Transform = nullptr;
	const UBuildingDefinition* Definition = nullptr;
	const FBuildPartTransformFragment* PartTransforms = nullptr;
	if (MaximumMeshParts <= 0 || !TryResolveEntity(Registry, Entity, Transform, Definition, PartTransforms)
		|| NextPartId < 0 || NextPartId > Definition->MeshParts.Num())
	{
		return false;
	}
	OutParts.Reserve(FMath::Min(MaximumMeshParts, Definition->MeshParts.Num() - NextPartId));
	int32 PartId = NextPartId;
	while (PartId < Definition->MeshParts.Num() && OutParts.Num() < MaximumMeshParts)
	{
		const int32 CurrentPartId = PartId++;
		const FBuildMeshPartDefinition& Part = Definition->MeshParts[CurrentPartId];
		if (!Part.Mesh)
		{
			continue;
		}
		FTransform WorldTransform;
		TArray<float, TInlineAllocator<4>> CustomData;
		if (!TryResolvePartTransform(*Transform, *Definition, PartTransforms, CurrentPartId, WorldTransform)
			|| !TryResolveCustomData(Registry, Entity, Part.CustomDataFloatCount, CustomData))
		{
			QueueRemoveEntity(Presentation, OutParts);
			OutParts.Reset();
			return false;
		}
		const EBuildRenderStorageClass Storage = ResolveStorageClass(Part, bHot);
		const FMeshPoolClusterKey Cluster = MakeClusterKey(
			Layer, Part, WorldTransform, Storage, ResidencyConfig, ClusterConfig);
		const FMeshPoolInstanceHandle Instance = Presentation.QueueAdd(Cluster, WorldTransform, CustomData);
		if (!Instance.IsSet())
		{
			QueueRemoveEntity(Presentation, OutParts);
			OutParts.Reset();
			return false;
		}
		OutParts.Add({CurrentPartId, Instance, Cluster, Storage});
		// 新 Cluster 首次创建可能远比普通 Add 昂贵；在 Part 边界检查墙钟，不能让 16 个冷 Cluster 串成 47ms。
		if (FMath::IsFinite(DeadlineSeconds) && FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			break;
		}
	}
	OutNextPartId = PartId;
	OutComplete = PartId >= Definition->MeshParts.Num();
	return true;
}

bool FBuildPresentationMeshPoolApplicator::QueueRemoveEntity(
	UPresentationWorldSubsystem& Presentation,
	const TConstArrayView<FBuildPresentationAppliedPart> Parts)
{
	for (const FBuildPresentationAppliedPart& Part : Parts)
	{
		if (Presentation.IsValidInstance(Part.Instance) && !Presentation.QueueRemove(Part.Instance))
		{
			return false;
		}
	}
	return true;
}

bool FBuildPresentationMeshPoolApplicator::QueueUpdateEntity(
	const FBuildEntityRegistry& Registry,
	UPresentationWorldSubsystem& Presentation,
	const FMeshPoolLayerHandle Layer,
	const FBuildEntityHandle Entity,
	const bool bHot,
	const FBuildPresentationResidencyConfig& ResidencyConfig,
	const FBuildRenderClusterConfig& ClusterConfig,
	const TConstArrayView<int32> RequestedParts,
	const TArrayView<FBuildPresentationAppliedPart> AppliedParts)
{
	const FBuildTransformFragment* Transform = nullptr;
	const UBuildingDefinition* Definition = nullptr;
	const FBuildPartTransformFragment* PartTransforms = nullptr;
	if (!TryResolveEntity(Registry, Entity, Transform, Definition, PartTransforms))
	{
		return false;
	}
	TArray<int32, TInlineAllocator<8>> PartIds;
	if (RequestedParts.IsEmpty())
	{
		for (const FBuildPresentationAppliedPart& Part : AppliedParts)
		{
			PartIds.Add(Part.PartId);
		}
	}
	else
	{
		PartIds.Append(RequestedParts);
	}
	for (const int32 PartId : PartIds)
	{
		FBuildPresentationAppliedPart* RecordPart = FindAppliedPart(AppliedParts, PartId);
		if (!RecordPart || !Definition->MeshParts.IsValidIndex(PartId))
		{
			return false;
		}
		const FBuildMeshPartDefinition& Part = Definition->MeshParts[PartId];
		FTransform WorldTransform;
		TArray<float, TInlineAllocator<4>> CustomData;
		if (Part.Mesh != RecordPart->Cluster.Mesh
			|| !TryResolvePartTransform(*Transform, *Definition, PartTransforms, PartId, WorldTransform)
			|| !TryResolveCustomData(Registry, Entity, Part.CustomDataFloatCount, CustomData))
		{
			return false;
		}
		const EBuildRenderStorageClass Storage = ResolveStorageClass(Part, bHot);
		const FMeshPoolClusterKey Cluster = MakeClusterKey(
			Layer, Part, WorldTransform, Storage, ResidencyConfig, ClusterConfig);
		const bool bQueued = Cluster == RecordPart->Cluster
			? Presentation.QueueUpdate(RecordPart->Instance, WorldTransform, CustomData)
			: Presentation.QueueMigrate(RecordPart->Instance, Cluster, WorldTransform, CustomData);
		if (!bQueued)
		{
			return false;
		}
		RecordPart->Cluster = Cluster;
		RecordPart->StorageClass = Storage;
	}
	return true;
}

bool FBuildPresentationMeshPoolApplicator::QueueStorageMigration(
	const FBuildEntityRegistry& Registry,
	UPresentationWorldSubsystem& Presentation,
	const FMeshPoolLayerHandle Layer,
	const FBuildEntityHandle Entity,
	const bool bHot,
	const FBuildPresentationResidencyConfig& ResidencyConfig,
	const FBuildRenderClusterConfig& ClusterConfig,
	const TArrayView<FBuildPresentationAppliedPart> AppliedParts)
{
	const FBuildTransformFragment* Transform = nullptr;
	const UBuildingDefinition* Definition = nullptr;
	const FBuildPartTransformFragment* PartTransforms = nullptr;
	if (!TryResolveEntity(Registry, Entity, Transform, Definition, PartTransforms))
	{
		return false;
	}
	for (FBuildPresentationAppliedPart& RecordPart : AppliedParts)
	{
		if (!Definition->MeshParts.IsValidIndex(RecordPart.PartId))
		{
			return false;
		}
		const FBuildMeshPartDefinition& Part = Definition->MeshParts[RecordPart.PartId];
		const EBuildRenderStorageClass DesiredStorage = ResolveStorageClass(Part, bHot);
		if (DesiredStorage == RecordPart.StorageClass)
		{
			continue;
		}
		FTransform WorldTransform;
		TArray<float, TInlineAllocator<4>> CustomData;
		if (!TryResolvePartTransform(
				*Transform, *Definition, PartTransforms, RecordPart.PartId, WorldTransform)
			|| !TryResolveCustomData(Registry, Entity, Part.CustomDataFloatCount, CustomData))
		{
			return false;
		}
		const FMeshPoolClusterKey DesiredCluster = MakeClusterKey(
			Layer, Part, WorldTransform, DesiredStorage, ResidencyConfig, ClusterConfig);
		if (!Presentation.QueueMigrate(
			RecordPart.Instance, DesiredCluster, WorldTransform, CustomData))
		{
			return false;
		}
		RecordPart.Cluster = DesiredCluster;
		RecordPart.StorageClass = DesiredStorage;
	}
	return true;
}

bool FBuildPresentationMeshPoolApplicator::QueueCustomData(
	const FBuildEntityRegistry& Registry,
	UPresentationWorldSubsystem& Presentation,
	const FBuildEntityHandle Entity,
	const TConstArrayView<FBuildPresentationAppliedPart> AppliedParts)
{
	for (const FBuildPresentationAppliedPart& Part : AppliedParts)
	{
		if (Part.Cluster.CustomDataFloatCount <= 0)
		{
			continue;
		}
		TArray<float, TInlineAllocator<4>> CustomData;
		if (!TryResolveCustomData(Registry, Entity, Part.Cluster.CustomDataFloatCount, CustomData)
			|| !Presentation.QueueCustomData(Part.Instance, CustomData))
		{
			return false;
		}
	}
	return true;
}
