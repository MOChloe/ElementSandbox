#include "Items/AxeEquippedItemActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AAxeEquippedItemActor::AAxeEquippedItemActor()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WoodMaterial(
		TEXT("/Game/Building/Materials/MI_FirePileWood.MI_FirePileWood"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BladeMaterial(
		TEXT("/Game/Building/Materials/MI_BuildingWhite.MI_BuildingWhite"));

	if (CylinderMesh.Succeeded())
	{
		GetItemMesh()->SetStaticMesh(CylinderMesh.Object);
	}
	if (WoodMaterial.Succeeded())
	{
		GetItemMesh()->SetMaterial(0, WoodMaterial.Object);
	}
	GetItemMesh()->SetRelativeLocation(FVector(0.0, 0.0, 30.0));
	GetItemMesh()->SetRelativeScale3D(FVector(0.045, 0.045, 0.60));
	GetItemMesh()->SetCanEverAffectNavigation(false);

	const auto ConfigureHeadPart = [this](
		UStaticMeshComponent& Component,
		const FVector& Location,
		const FVector& Scale)
	{
		Component.SetupAttachment(SceneRoot);
		if (CubeMesh.Succeeded())
		{
			Component.SetStaticMesh(CubeMesh.Object);
		}
		if (BladeMaterial.Succeeded())
		{
			Component.SetMaterial(0, BladeMaterial.Object);
		}
		Component.SetRelativeLocation(Location);
		Component.SetRelativeScale3D(Scale);
		Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component.SetGenerateOverlapEvents(false);
		Component.SetCanEverAffectNavigation(false);
		Component.SetCastShadow(false);
		Component.bCastDynamicShadow = false;
		Component.bCastStaticShadow = false;
	};

	AxeHead = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AxeHead"));
	ConfigureHeadPart(*AxeHead, FVector(-3.0, 0.0, 59.0), FVector(0.18, 0.09, 0.10));

	// 头部放在握柄局部 -X 侧：只翻转刀锋朝向，不改变手柄沿手掌向下的握持方向。
	AxeBlade = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AxeBlade"));
	ConfigureHeadPart(*AxeBlade, FVector(-15.0, 0.0, 59.0), FVector(0.08, 0.055, 0.22));

	SetNetUpdateFrequency(15.0f);
}
