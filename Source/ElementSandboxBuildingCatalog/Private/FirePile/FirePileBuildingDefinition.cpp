#include "FirePile/FirePileBuildingDefinition.h"

#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/StaticMesh.h"
#include "Entity/BuildEntityRegistry.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

namespace
{
	void AddFirePilePart(
		UFirePileBuildingDefinition& Definition,
		UStaticMesh& Mesh,
		UMaterialInterface& Material,
		const FTransform& LocalTransform)
	{
		FBuildMeshPartDefinition& Part = Definition.MeshParts.AddDefaulted_GetRef();
		Part.Mesh = &Mesh;
		Part.MaterialOverride = &Material;
		Part.LocalTransform = LocalTransform;
		Part.PresentationPolicy = EBuildMeshPartPresentationPolicy::Static;
	}
}

UFirePileBuildingDefinition::UFirePileBuildingDefinition()
{
	Destruction.MaxDurability = 100.0f;
	Destruction.ProductClass = UWoodBlockWorldObjectDefinition::StaticClass();
	DefinitionId = TEXT("FirePile");
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(
		TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> StoneMaterial(
		TEXT("/Game/Building/Materials/MI_FirePileStone.MI_FirePileStone"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WoodMaterial(
		TEXT("/Game/Building/Materials/MI_FirePileWood.MI_FirePileWood"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FlameMaterial(
		TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	if (!SphereMesh.Succeeded()
		|| !CylinderMesh.Succeeded()
		|| !ConeMesh.Succeeded()
		|| !StoneMaterial.Succeeded()
		|| !WoodMaterial.Succeeded()
		|| !FlameMaterial.Succeeded())
	{
		return;
	}

	MeshParts.Reset(14);
	constexpr int32 StoneCount = 8;
	constexpr double StoneRingRadius = 62.0;
	for (int32 StoneIndex = 0; StoneIndex < StoneCount; ++StoneIndex)
	{
		const double AngleDegrees = 360.0 * StoneIndex / StoneCount;
		const double AngleRadians = FMath::DegreesToRadians(AngleDegrees);
		const FVector Location(
			FMath::Cos(AngleRadians) * StoneRingRadius,
			FMath::Sin(AngleRadians) * StoneRingRadius,
			14.0);
		AddFirePilePart(
			*this,
			*SphereMesh.Object,
			*StoneMaterial.Object,
			FTransform(
				FRotator(0.0, AngleDegrees, 0.0),
				Location,
				FVector(0.28, 0.20, 0.16)));
	}

	// 三根横放的圆柱组成柴堆。
	for (const double YawDegrees : {0.0, 60.0, 120.0})
	{
		AddFirePilePart(
			*this,
			*CylinderMesh.Object,
			*WoodMaterial.Object,
			FTransform(
				FRotator(0.0, YawDegrees, 90.0),
				FVector(0.0, 0.0, 34.0),
				FVector(0.16, 0.16, 0.90)));
	}

	// 先用三个橙色 Cone 给永续火源一个无需 Niagara 的清晰占位表现。
	AddFirePilePart(
		*this,
		*ConeMesh.Object,
		*FlameMaterial.Object,
		FTransform(
			FRotator::ZeroRotator,
			FVector(0.0, 0.0, 92.0),
			FVector(0.42, 0.42, 1.12)));
	AddFirePilePart(
		*this,
		*ConeMesh.Object,
		*FlameMaterial.Object,
		FTransform(
			FRotator(0.0, 25.0, 0.0),
			FVector(24.0, 4.0, 70.0),
			FVector(0.26, 0.26, 0.72)));
	AddFirePilePart(
		*this,
		*ConeMesh.Object,
		*FlameMaterial.Object,
		FTransform(
			FRotator(0.0, -20.0, 0.0),
			FVector(-22.0, 10.0, 66.0),
			FVector(0.24, 0.24, 0.64)));

	FBuildCollisionPartDefinition& Collision = CollisionParts.AddDefaulted_GetRef();
	Collision.CollisionMesh = CylinderMesh.Object;
	Collision.LocalTransform = FTransform(
		FRotator::ZeroRotator,
		FVector(0.0, 0.0, 25.0),
		FVector(0.80, 0.80, 0.35));
	Collision.Mobility = EBuildCollisionMobility::Static;
}

bool UFirePileBuildingDefinition::ConfigureEntity(
	FBuildEntityRegistry& Registry,
	const FBuildEntityHandle Entity) const
{
	return Registry.IsAlive(Entity);
}
