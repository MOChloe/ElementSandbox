#include "Items/DemolitionToolEquippedItemActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ADemolitionToolEquippedItemActor::ADemolitionToolEquippedItemActor()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CylinderMesh.Succeeded())
	{
		GetItemMesh()->SetStaticMesh(CylinderMesh.Object);
	}
	GetItemMesh()->SetRelativeLocation(FVector(0.0, 0.0, 28.0));
	GetItemMesh()->SetRelativeScale3D(FVector(0.045, 0.045, 0.56));
	GetItemMesh()->SetCanEverAffectNavigation(false);

	HammerHead = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HammerHead"));
	HammerHead->SetupAttachment(SceneRoot);
	if (CubeMesh.Succeeded())
	{
		HammerHead->SetStaticMesh(CubeMesh.Object);
	}
	HammerHead->SetRelativeLocation(FVector(0.0, 0.0, 56.0));
	HammerHead->SetRelativeScale3D(FVector(0.34, 0.12, 0.12));
	HammerHead->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HammerHead->SetGenerateOverlapEvents(false);
	HammerHead->SetCanEverAffectNavigation(false);
	HammerHead->SetCastShadow(false);
	HammerHead->bCastDynamicShadow = false;
	HammerHead->bCastStaticShadow = false;

	SetNetUpdateFrequency(15.0f);
}
