#include "Torch/TorchFixtureBuildingDefinition.h"

#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/StaticMesh.h"
#include "Entity/BuildEntityRegistry.h"
#include "Materials/MaterialInterface.h"
#include "Torch/TorchDefinition.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

UTorchFixtureBuildingDefinition::UTorchFixtureBuildingDefinition()
{
	Destruction.MaxDurability = 100.0f;
	Destruction.ProductClass = UWoodBlockWorldObjectDefinition::StaticClass();
	DefinitionId = GetMountedTorchBuildingDefinitionId();
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ShaftMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> FlameMesh(
		TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ShaftMaterial(
		TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FlameMaterial(
		TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	if (!ShaftMesh.Succeeded() || !FlameMesh.Succeeded()
		|| !ShaftMaterial.Succeeded() || !FlameMaterial.Succeeded())
	{
		return;
	}

	// Entity 原点就是墙面安装点。杆体从原点向外上方伸出，火焰位于顶端；
	// 这样离线配方能直接验证安装点确实落在承载构件表面。
	// 即使正式种子包含两百万盏，也不为单盏火把创建 Actor、Component 或灯光。
	MeshParts.Reset(2);
	FBuildMeshPartDefinition& Shaft = MeshParts.AddDefaulted_GetRef();
	Shaft.Mesh = ShaftMesh.Object;
	Shaft.MaterialOverride = ShaftMaterial.Object;
	Shaft.LocalTransform = FTransform(
		FRotator(18.0, 0.0, 0.0),
		FVector(-12.0, 0.0, 36.0),
		FVector(0.08, 0.08, 0.75));
	Shaft.SurfaceProfileId = TEXT("Surface.Torch.Wood");
	Shaft.PresentationPolicy = EBuildMeshPartPresentationPolicy::Static;
	Shaft.CustomDataFloatCount = 1;

	FBuildMeshPartDefinition& Flame = MeshParts.AddDefaulted_GetRef();
	Flame.Mesh = FlameMesh.Object;
	Flame.MaterialOverride = FlameMaterial.Object;
	Flame.LocalTransform = FTransform(
		FRotator::ZeroRotator,
		FVector(-24.0, 0.0, 92.0),
		FVector(0.22, 0.22, 0.62));
	Flame.SurfaceProfileId = TEXT("Surface.Torch.Flame");
	Flame.PresentationPolicy = EBuildMeshPartPresentationPolicy::Static;

	// 只让木杆参与近场 Chaos；火焰锥体仍是纯表现。百万火炬不会常驻 Body，
	// Building Collision Processor 只为当前碰撞观察范围创建实例。
	CollisionParts.Reset(1);
	FBuildCollisionPartDefinition& Collision = CollisionParts.AddDefaulted_GetRef();
	Collision.CollisionMesh = ShaftMesh.Object;
	Collision.DrivenMeshPartId = 0;
	Collision.Mobility = EBuildCollisionMobility::Static;
}

bool UTorchFixtureBuildingDefinition::ConfigureEntity(
	FBuildEntityRegistry& Registry,
	const FBuildEntityHandle Entity) const
{
	return Registry.IsAlive(Entity);
}
