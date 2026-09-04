#include "Building/BuildPlacementPreviewActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Definition/BuildingDefinition.h"
#include "Engine/CollisionProfile.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ABuildPlacementPreviewActor::ABuildPlacementPreviewActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> AllowedMaterialFinder(
		TEXT("/Game/Building/Materials/MI_BuildPreviewValid.MI_BuildPreviewValid"));
	AllowedMaterial = AllowedMaterialFinder.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BlockedMaterialFinder(
		TEXT("/Game/Building/Materials/MI_BuildPreviewInvalid.MI_BuildPreviewInvalid"));
	BlockedMaterial = BlockedMaterialFinder.Object;

	SetActorHiddenInGame(true);
}

bool ABuildPlacementPreviewActor::SetDefinition(
	const UBuildingDefinition* Definition)
{
	if (CurrentDefinition.Get() == Definition)
	{
		return IsValid(Definition) && !MeshComponents.IsEmpty();
	}

	ClearMeshComponents();
	CurrentDefinition = Definition;
	if (!IsValid(Definition))
	{
		SetActorHiddenInGame(true);
		return false;
	}

	for (int32 PartId = 0; PartId < Definition->MeshParts.Num(); ++PartId)
	{
		const FBuildMeshPartDefinition& Part = Definition->MeshParts[PartId];
		if (!IsValid(Part.Mesh))
		{
			ClearMeshComponents();
			CurrentDefinition.Reset();
			SetActorHiddenInGame(true);
			return false;
		}

		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
			this,
			*FString::Printf(TEXT("PreviewMeshPart_%d"), PartId),
			RF_Transient);
		if (!Component)
		{
			ClearMeshComponents();
			CurrentDefinition.Reset();
			SetActorHiddenInGame(true);
			return false;
		}
		Component->SetupAttachment(SceneRoot);
		Component->SetStaticMesh(Part.Mesh);
		Component->SetRelativeTransform(Part.LocalTransform);
		Component->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
		Component->bCastDynamicShadow = false;
		Component->bCastStaticShadow = false;
		Component->RegisterComponent();
		MeshComponents.Add(Component);
	}

	ApplyPreviewMaterial(bCurrentAllowed);
	return !MeshComponents.IsEmpty();
}

void ABuildPlacementPreviewActor::SetPlacementState(
	const FTransform& WorldTransform,
	const bool bAllowed)
{
	SetActorTransform(WorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
	if (bCurrentAllowed != bAllowed)
	{
		bCurrentAllowed = bAllowed;
		ApplyPreviewMaterial(bAllowed);
	}
}

void ABuildPlacementPreviewActor::SetPreviewVisible(const bool bVisible)
{
	SetActorHiddenInGame(!bVisible || MeshComponents.IsEmpty());
}

void ABuildPlacementPreviewActor::ApplyPreviewMaterial(const bool bAllowed)
{
	UMaterialInterface* Material = bAllowed ? AllowedMaterial : BlockedMaterial;
	for (UStaticMeshComponent* Component : MeshComponents)
	{
		if (!IsValid(Component))
		{
			continue;
		}
		const int32 MaterialCount = FMath::Max(1, Component->GetNumMaterials());
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			if (Material)
			{
				Component->SetMaterial(MaterialIndex, Material);
			}
		}
	}
}

void ABuildPlacementPreviewActor::ClearMeshComponents()
{
	for (UStaticMeshComponent* Component : MeshComponents)
	{
		if (IsValid(Component))
		{
			Component->DestroyComponent();
		}
	}
	MeshComponents.Reset();
}
