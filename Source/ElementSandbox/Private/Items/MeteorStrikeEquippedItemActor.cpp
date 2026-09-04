#include "Items/MeteorStrikeEquippedItemActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AMeteorStrikeEquippedItemActor::AMeteorStrikeEquippedItemActor()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Flame(
		TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	if (Sphere.Succeeded()) GetItemMesh()->SetStaticMesh(Sphere.Object);
	if (Flame.Succeeded()) GetItemMesh()->SetMaterial(0, Flame.Object);
	GetItemMesh()->SetRelativeScale3D(FVector(0.4));
	GetItemMesh()->SetCanEverAffectNavigation(false);
}
