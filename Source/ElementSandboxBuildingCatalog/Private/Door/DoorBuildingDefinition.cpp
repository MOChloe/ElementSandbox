#include "Door/DoorBuildingDefinition.h"

#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Door/DoorStateFragment.h"
#include "Engine/StaticMesh.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

namespace
{
	void AddDoorPart(
		UDoorBuildingDefinition& Definition,
		UStaticMesh& Mesh,
		const FVector& LocalLocation,
		const FVector& LocalScale,
		const EBuildMeshPartPresentationPolicy PresentationPolicy =
			EBuildMeshPartPresentationPolicy::Static)
	{
		FBuildMeshPartDefinition& Part = Definition.MeshParts.AddDefaulted_GetRef();
		Part.Mesh = &Mesh;
		Part.LocalTransform = FTransform(
			FRotator::ZeroRotator,
			LocalLocation,
			LocalScale);
		Part.PresentationPolicy = PresentationPolicy;
	}

	void AddDoorCollisionPart(
		UDoorBuildingDefinition& Definition,
		UStaticMesh& Mesh,
		const int32 DrivenMeshPartId,
		const EBuildCollisionMobility Mobility)
	{
		FBuildCollisionPartDefinition& Part =
			Definition.CollisionParts.AddDefaulted_GetRef();
		Part.CollisionMesh = &Mesh;
		Part.DrivenMeshPartId = DrivenMeshPartId;
		Part.Mobility = Mobility;
	}

	constexpr int32 DefinitionMovingDoorPartIds[] = {0, 4, 5, 6};
	const FVector DefinitionDoorHingeLocalLocation(0.0, -45.0, 0.0);

	bool IsMovingDoorPart(const int32 PartId)
	{
		for (const int32 MovingPartId : DefinitionMovingDoorPartIds)
		{
			if (PartId == MovingPartId)
			{
				return true;
			}
		}
		return false;
	}

	bool IsValidDoorBounds(const FBox& Bounds)
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
}

UDoorBuildingDefinition::UDoorBuildingDefinition()
{
	DefinitionId = TEXT("Door");
	Destruction.MaxDurability = 100.0f;
	Destruction.ProductClass = UWoodBlockWorldObjectDefinition::StaticClass();
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Game/Building/Meshes/SM_BuildingCube_CPU.SM_BuildingCube_CPU"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
		TEXT("/Game/Building/Meshes/SM_BuildingSphere_CPU.SM_BuildingSphere_CPU"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BurnableMaterial(
		TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
	if (!CubeMesh.Succeeded()
		|| !SphereMesh.Succeeded()
		|| !BurnableMaterial.Succeeded())
	{
		return;
	}

	MeshParts.Reset(7);
	// 90 x 210 cm 门扇，门的正面朝向本地 -X。
	AddDoorPart(
		*this,
		*CubeMesh.Object,
		FVector(0.0, 0.0, 105.0),
		FVector(0.04, 0.90, 2.10),
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	// 左右门框与上门框。
	AddDoorPart(*this, *CubeMesh.Object, FVector(0.0, -52.0, 112.5), FVector(0.12, 0.12, 2.25));
	AddDoorPart(*this, *CubeMesh.Object, FVector(0.0, 52.0, 112.5), FVector(0.12, 0.12, 2.25));
	AddDoorPart(*this, *CubeMesh.Object, FVector(0.0, 0.0, 225.0), FVector(0.12, 1.16, 0.12));
	// 门扇正面两块简单饰板，让白模轮廓能被立即识别为家用门。
	AddDoorPart(
		*this,
		*CubeMesh.Object,
		FVector(-2.75, 0.0, 155.0),
		FVector(0.015, 0.66, 0.72),
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	AddDoorPart(
		*this,
		*CubeMesh.Object,
		FVector(-2.75, 0.0, 58.0),
		FVector(0.015, 0.66, 0.56),
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	// 8 cm 球形门把手，位于门扇右侧。
	AddDoorPart(
		*this,
		*SphereMesh.Object,
		FVector(-6.0, 32.0, 100.0),
		FVector(0.08),
		EBuildMeshPartPresentationPolicy::ProximityPromotable);
	for (FBuildMeshPartDefinition& Part : MeshParts)
	{
		Part.MaterialOverride = BurnableMaterial.Object;
		Part.SurfaceProfileId = TEXT("Surface.City.Wood");
		Part.CustomDataFloatCount = 1;
	}
	CollisionParts.Reset(4);
	// 门扇跟随 Mesh Part 0；门框三个代理分别跟随 Part 1、2、3。
	AddDoorCollisionPart(
		*this,
		*CubeMesh.Object,
		0,
		EBuildCollisionMobility::Kinematic);
	for (const int32 FramePartId : {1, 2, 3})
	{
		AddDoorCollisionPart(
			*this,
			*CubeMesh.Object,
			FramePartId,
			EBuildCollisionMobility::Static);
		}
}

bool UDoorBuildingDefinition::InitializeAsSettlementCompanion()
{
	if (MeshParts.Num() != 7 || !HasValidCollisionDefinition())
	{
		return false;
	}

	DefinitionId = TEXT("Settlement.Door");
	return true;
}

bool UDoorBuildingDefinition::TryCalculateWorldBounds(
	const FTransform& WorldTransform,
	const TConstArrayView<FTransform> PartLocalTransforms,
	FBox& OutWorldBounds) const
{
	OutWorldBounds = FBox(ForceInit);
	if (WorldTransform.ContainsNaN()
		|| (!PartLocalTransforms.IsEmpty()
			&& PartLocalTransforms.Num() != MeshParts.Num()))
	{
		return false;
	}

	FBox StaticLocalBounds(ForceInit);
	double MaximumHorizontalRadius = 0.0;
	double MinimumLocalZ = TNumericLimits<double>::Max();
	double MaximumLocalZ = TNumericLimits<double>::Lowest();
	bool bHasMovingBounds = false;
	for (int32 PartId = 0; PartId < MeshParts.Num(); ++PartId)
	{
		const FBuildMeshPartDefinition& Part = MeshParts[PartId];
		if (!Part.Mesh || Part.LocalTransform.ContainsNaN())
		{
			continue;
		}

		const FBox MeshBounds = Part.Mesh->GetBoundingBox();
		const FBox PartLocalBounds = MeshBounds.TransformBy(Part.LocalTransform);
		if (!IsValidDoorBounds(MeshBounds) || !IsValidDoorBounds(PartLocalBounds))
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}

		if (!IsMovingDoorPart(PartId))
		{
			StaticLocalBounds += PartLocalBounds;
			continue;
		}

		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector MeshCorner(
				(CornerIndex & 1) ? MeshBounds.Max.X : MeshBounds.Min.X,
				(CornerIndex & 2) ? MeshBounds.Max.Y : MeshBounds.Min.Y,
				(CornerIndex & 4) ? MeshBounds.Max.Z : MeshBounds.Min.Z);
			const FVector LocalCorner = Part.LocalTransform.TransformPosition(MeshCorner);
			const FVector HingeOffset = LocalCorner - DefinitionDoorHingeLocalLocation;
			MaximumHorizontalRadius = FMath::Max(
				MaximumHorizontalRadius,
				FVector2D(HingeOffset.X, HingeOffset.Y).Length());
			MinimumLocalZ = FMath::Min(MinimumLocalZ, LocalCorner.Z);
			MaximumLocalZ = FMath::Max(MaximumLocalZ, LocalCorner.Z);
			bHasMovingBounds = true;
		}
	}

	FBox ConservativeLocalBounds = StaticLocalBounds;
	if (bHasMovingBounds)
	{
		ConservativeLocalBounds += FBox(
			FVector(
				DefinitionDoorHingeLocalLocation.X - MaximumHorizontalRadius,
				DefinitionDoorHingeLocalLocation.Y - MaximumHorizontalRadius,
				MinimumLocalZ),
			FVector(
				DefinitionDoorHingeLocalLocation.X + MaximumHorizontalRadius,
				DefinitionDoorHingeLocalLocation.Y + MaximumHorizontalRadius,
				MaximumLocalZ));
	}

	OutWorldBounds = ConservativeLocalBounds.TransformBy(WorldTransform);
	for (int32 CollisionPartId = 0;
		CollisionPartId < CollisionParts.Num();
		++CollisionPartId)
	{
		FBox CollisionWorldBounds(ForceInit);
		if (!TryCalculateCollisionPartWorldBounds(
			CollisionPartId,
			WorldTransform,
			PartLocalTransforms,
			CollisionWorldBounds))
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}
		OutWorldBounds += CollisionWorldBounds;
	}
	return IsValidDoorBounds(ConservativeLocalBounds)
		&& IsValidDoorBounds(OutWorldBounds);
}

bool UDoorBuildingDefinition::DoPartTransformChangesAffectSpatialBounds(
	const TConstArrayView<int32> PartIds) const
{
	if (PartIds.IsEmpty())
	{
		return false;
	}

	for (const int32 PartId : PartIds)
	{
		if (!IsMovingDoorPart(PartId))
		{
			return true;
		}
	}
	return false;
}

bool UDoorBuildingDefinition::ConfigureEntity(
	FBuildEntityRegistry& Registry,
	const FBuildEntityHandle Entity) const
{
	FBuildPartTransformFragment PartTransforms;
	PartTransforms.LocalTransforms.Reserve(MeshParts.Num());
	for (const FBuildMeshPartDefinition& Part : MeshParts)
	{
		PartTransforms.LocalTransforms.Add(Part.LocalTransform);
	}
	FBuildDoorStateFragment DoorState;
	DoorState.State = EBuildDoorState::Closed;
	DoorState.TransitionStartServerTimeSeconds = 0.0;

	const bool bConfigured = Registry.AddFragment(Entity, DoorState)
		&& Registry.AddFragment(Entity, PartTransforms);
	return bConfigured;
}
