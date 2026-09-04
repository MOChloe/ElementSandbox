#include "Focus/FocusHighlightActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AFocusHighlightActor::AFocusHighlightActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);

	HighlightMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HighlightMesh"));
	SetRootComponent(HighlightMesh);
	HighlightMesh->SetMobility(EComponentMobility::Movable);
	HighlightMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	HighlightMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HighlightMesh->SetGenerateOverlapEvents(false);
	HighlightMesh->SetCanEverAffectNavigation(false);
	HighlightMesh->SetCastShadow(false);
	HighlightMesh->bCastDynamicShadow = false;
	HighlightMesh->bCastStaticShadow = false;
	HighlightMesh->bReceivesDecals = false;
	HighlightMesh->SetTranslucentSortPriority(100);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HighlightMaterialFinder(
		TEXT("/Game/Building/Materials/MI_BuildPreviewValid.MI_BuildPreviewValid"));
	HighlightMaterial = HighlightMaterialFinder.Object;

	SetActorHiddenInGame(true);
}

bool AFocusHighlightActor::SetHighlightedPart(
	UStaticMesh* Mesh,
	const FTransform& WorldTransform,
	const float UniformScale)
{
	if (!IsValid(Mesh)
		|| !IsValid(HighlightMaterial)
		|| WorldTransform.ContainsNaN())
	{
		SetHighlightVisible(false);
		return false;
	}

	if (HighlightMesh->GetStaticMesh() != Mesh)
	{
		HighlightMesh->SetStaticMesh(Mesh);
	}
	const int32 MaterialCount = FMath::Max(1, HighlightMesh->GetNumMaterials());
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		HighlightMesh->SetMaterial(MaterialIndex, HighlightMaterial);
	}

	FTransform HighlightTransform = WorldTransform;
	HighlightTransform.SetScale3D(
		HighlightTransform.GetScale3D() * FMath::Max(UniformScale, 1.001f));
	SetActorTransform(
		HighlightTransform,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	SetHighlightVisible(true);
	return true;
}

void AFocusHighlightActor::SetHighlightVisible(const bool bVisible)
{
	const bool bCanShow = bVisible
		&& IsValid(HighlightMesh)
		&& IsValid(HighlightMesh->GetStaticMesh())
		&& IsValid(HighlightMaterial);
	SetActorHiddenInGame(!bCanShow);
	HighlightMesh->SetVisibility(bCanShow, true);
}

bool AFocusHighlightActor::IsHighlightVisible() const
{
	return !IsHidden()
		&& IsValid(HighlightMesh)
		&& IsValid(HighlightMesh->GetStaticMesh())
		&& HighlightMesh->IsVisible();
}
