#include "City/CityBuildingPieceDefinition.h"

#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/StaticMesh.h"
#include "Entity/BuildEntityRegistry.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

TConstArrayView<ECityBuildingPieceKind> GetDefaultCityPrimitivePieceKinds()
{
	static const ECityBuildingPieceKind Kinds[]
		= { ECityBuildingPieceKind::SolidBox, ECityBuildingPieceKind::DecorativeBox,
			  ECityBuildingPieceKind::SolidSphere, ECityBuildingPieceKind::DecorativeSphere };
	return Kinds;
}

TConstArrayView<FName> GetDefaultCityPieceSurfaceProfileIds()
{
	static const FName SurfaceProfileIds[] = {
		TEXT("Surface.City.Wall"),
		TEXT("Surface.City.Stone"),
		TEXT("Surface.City.Wood")};
	return SurfaceProfileIds;
}

FName GetCityBuildingPieceDefinitionId(
	const ECityBuildingPieceKind Kind,
	const FName SurfaceProfileId)
{
	const TCHAR* KindSuffix = nullptr;
	switch (Kind)
	{
	case ECityBuildingPieceKind::SolidBox:
		KindSuffix = TEXT("SolidBox");
		break;
	case ECityBuildingPieceKind::DecorativeBox:
		KindSuffix = TEXT("DecorativeBox");
		break;
	case ECityBuildingPieceKind::SolidSphere:
		KindSuffix = TEXT("SolidSphere");
		break;
	case ECityBuildingPieceKind::DecorativeSphere:
		KindSuffix = TEXT("DecorativeSphere");
		break;
	default:
		return NAME_None;
	}

	const TCHAR* SurfaceSuffix = nullptr;
	if (SurfaceProfileId == TEXT("Surface.City.Wall"))
	{
		SurfaceSuffix = TEXT("Wall");
	}
	else if (SurfaceProfileId == TEXT("Surface.City.Stone"))
	{
		SurfaceSuffix = TEXT("Stone");
	}
	else if (SurfaceProfileId == TEXT("Surface.City.Wood"))
	{
		SurfaceSuffix = TEXT("Wood");
	}
	if (!SurfaceSuffix)
	{
		return NAME_None;
	}
	return FName(*FString::Printf(TEXT("Settlement.Piece.%s.%s"), KindSuffix, SurfaceSuffix));
}

UCityBuildingPieceDefinition::UCityBuildingPieceDefinition()
{
	Destruction.MaxDurability = 100.0f;
	Destruction.ProductClass = UWoodBlockWorldObjectDefinition::StaticClass();
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(
		TEXT("/Game/Building/Meshes/SM_BuildingCube_CPU.SM_BuildingCube_CPU"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(
		TEXT("/Game/Building/Meshes/SM_BuildingSphere_CPU.SM_BuildingSphere_CPU"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Burnable(
		TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
	CubeMesh = Cube.Object;
	SphereMesh = Sphere.Object;
	BurnableMaterial = Burnable.Object;
}

bool UCityBuildingPieceDefinition::Initialize(
	const ECityBuildingPieceKind InKind,
	const FName InSurfaceProfileId)
{
	const FName StableId = GetCityBuildingPieceDefinitionId(InKind, InSurfaceProfileId);
	const bool bSphere
		= InKind == ECityBuildingPieceKind::SolidSphere || InKind == ECityBuildingPieceKind::DecorativeSphere;
	const bool bSolid = InKind == ECityBuildingPieceKind::SolidBox || InKind == ECityBuildingPieceKind::SolidSphere;
	UStaticMesh* Mesh = bSphere ? SphereMesh.Get() : CubeMesh.Get();
	if (StableId.IsNone() || !IsValid(Mesh) || !IsValid(BurnableMaterial))
	{
		return false;
	}

	DefinitionId = StableId;
	PieceKind = InKind;
	SurfaceProfileId = InSurfaceProfileId;
	if (InKind != ECityBuildingPieceKind::SolidBox
		&& InKind != ECityBuildingPieceKind::DecorativeBox
		&& InKind != ECityBuildingPieceKind::SolidSphere
		&& InKind != ECityBuildingPieceKind::DecorativeSphere)
	{
		return false;
	}
	MeshParts.Reset(1);
	FBuildMeshPartDefinition& MeshPart = MeshParts.AddDefaulted_GetRef();
	MeshPart.Mesh = Mesh;
	MeshPart.MaterialOverride = BurnableMaterial;
	MeshPart.LocalTransform = FTransform::Identity;
	MeshPart.SurfaceProfileId = SurfaceProfileId;
	MeshPart.PresentationPolicy = EBuildMeshPartPresentationPolicy::Static;
	MeshPart.CustomDataFloatCount = CustomDataFloatCount;

	CollisionParts.Reset(bSolid ? 1 : 0);
	if (bSolid)
	{
		FBuildCollisionPartDefinition& Collision = CollisionParts.AddDefaulted_GetRef();
		Collision.CollisionMesh = Mesh;
		Collision.LocalTransform = FTransform::Identity;
		Collision.DrivenMeshPartId = 0;
		Collision.Mobility = EBuildCollisionMobility::Static;
	}
	return MeshParts.Num() == 1
		&& HasValidCollisionDefinition();
}

int64 UCityBuildingPieceDefinition::GetEstimatedTriangleCountPerInstance() const
{
	return MeshParts.Num() == 1 && MeshParts[0].Mesh ? MeshParts[0].Mesh->GetNumTriangles(0) : 0;
}

bool UCityBuildingPieceDefinition::ConfigureEntity(
	FBuildEntityRegistry& Registry, const FBuildEntityHandle Entity) const
{
	if (!Registry.IsAlive(Entity))
	{
		return false;
	}

	// Cold 是隐式默认态。FireState 与烧黑 CustomData 只在元素邻域真正需要时
	// 由 ElementGameplay 稀疏创建；未触发的转换状态与拾取投影也不能为数千万
	// 种子部件预分配 Fragment。
	return true;
}
