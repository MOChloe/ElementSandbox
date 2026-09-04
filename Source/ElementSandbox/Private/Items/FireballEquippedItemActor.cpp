#include "Items/FireballEquippedItemActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AFireballEquippedItemActor::AFireballEquippedItemActor()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FlameMaterial(
		TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	if (SphereMesh.Succeeded())
	{
		GetItemMesh()->SetStaticMesh(SphereMesh.Object);
	}
	if (FlameMaterial.Succeeded())
	{
		GetItemMesh()->SetMaterial(0, FlameMaterial.Object);
	}
	GetItemMesh()->SetRelativeScale3D(FVector(0.30));
	GetItemMesh()->SetCanEverAffectNavigation(false);
	SetNetUpdateFrequency(15.0f);
}
