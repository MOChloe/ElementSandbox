#include "Wood/WoodBuildingDefinition.h"

#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/StaticMesh.h"
#include "Entity/BuildEntityRegistry.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

UWoodBuildingDefinition::UWoodBuildingDefinition()
{
	Destruction.MaxDurability = 100.0f;
	Destruction.ProductClass = UWoodBlockWorldObjectDefinition::StaticClass();
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Game/Building/Meshes/SM_BuildingCube_CPU.SM_BuildingCube_CPU"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BurnableMaterial(
		TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
	PrimitiveMesh = CubeMesh.Object;
	PrimitiveMaterial = BurnableMaterial.Object;
}

bool UWoodBuildingDefinition::Initialize(
	const FName InDefinitionId,
	const FVector& SizeCentimeters)
{
	if (InDefinitionId.IsNone() || !IsValid(PrimitiveMesh)
		|| !IsValid(PrimitiveMaterial) || SizeCentimeters.ContainsNaN()
		|| SizeCentimeters.GetMin() <= UE_SMALL_NUMBER)
	{
		return false;
	}

	DefinitionId = InDefinitionId;
	ConfiguredSizeCentimeters = SizeCentimeters;
	MeshParts.Reset(1);
	FBuildMeshPartDefinition& MeshPart = MeshParts.AddDefaulted_GetRef();
	MeshPart.Mesh = PrimitiveMesh;
	MeshPart.MaterialOverride = PrimitiveMaterial;
	MeshPart.LocalTransform = FTransform(
		FQuat::Identity,
		FVector(0.0, 0.0, SizeCentimeters.Z * 0.5),
		SizeCentimeters / 100.0);
	MeshPart.PresentationPolicy = EBuildMeshPartPresentationPolicy::Static;
	MeshPart.CustomDataFloatCount = 1;

	CollisionParts.Reset(1);
	FBuildCollisionPartDefinition& Collision =
		CollisionParts.AddDefaulted_GetRef();
	Collision.CollisionMesh = PrimitiveMesh;
	Collision.DrivenMeshPartId = 0;
	Collision.Mobility = EBuildCollisionMobility::Static;
	return HasValidCollisionDefinition();
}

bool UWoodBuildingDefinition::ConfigureEntity(
	FBuildEntityRegistry& Registry,
	const FBuildEntityHandle Entity) const
{
	if (!Registry.IsAlive(Entity)
		|| ConfiguredSizeCentimeters.GetMin() <= UE_SMALL_NUMBER)
	{
		return false;
	}

	return true;
}
