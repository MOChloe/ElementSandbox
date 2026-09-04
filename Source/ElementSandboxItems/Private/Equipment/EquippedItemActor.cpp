#include "Equipment/EquippedItemActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"

AEquippedItemActor::AEquippedItemActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(false);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	ItemMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ItemMesh"));
	ItemMesh->SetupAttachment(SceneRoot);
	ItemMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ItemMesh->SetGenerateOverlapEvents(false);
	ItemMesh->SetCastShadow(false);
	ItemMesh->bCastDynamicShadow = false;
	ItemMesh->bCastStaticShadow = false;
}
