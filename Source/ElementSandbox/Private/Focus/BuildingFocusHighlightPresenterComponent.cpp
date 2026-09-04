#include "Focus/BuildingFocusHighlightPresenterComponent.h"

#include "BuildingWorldSubsystem.h"
#include "Definition/BuildingDefinition.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Focus/FocusHighlightActor.h"
#include "Focus/BuildingFocusTarget.h"
#include "Focus/FocusHostComponent.h"
#include "Game/ElementSandboxPlayerController.h"

UBuildingFocusHighlightPresenterComponent::UBuildingFocusHighlightPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UBuildingFocusHighlightPresenterComponent::BeginPlay()
{
	Super::BeginPlay();

	const AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	const bool bCanRunLocally = PlayerController
		&& PlayerController->IsLocalController()
		&& GetNetMode() != NM_DedicatedServer
		&& !IsRunningCommandlet();
	SetComponentTickEnabled(bCanRunLocally);
	if (!bCanRunLocally)
	{
		return;
	}

	if (UFocusHostComponent* FocusHost =
		PlayerController->FindComponentByClass<UFocusHostComponent>())
	{
		AddTickPrerequisiteComponent(FocusHost);
	}
}

void UBuildingFocusHighlightPresenterComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	HideHighlight();
	if (IsValid(HighlightActor))
	{
		HighlightActor->Destroy();
	}
	HighlightActor = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UBuildingFocusHighlightPresenterComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	RefreshHighlight();
}

void UBuildingFocusHighlightPresenterComponent::RefreshHighlight()
{
	UStaticMesh* Mesh = nullptr;
	FTransform WorldTransform = FTransform::Identity;
	FBuildEntityHandle Entity;
	int32 PartId = INDEX_NONE;
	if (!TryResolveFocusedPart(Mesh, WorldTransform, Entity, PartId)
		|| !EnsureHighlightActor()
		|| !HighlightActor->SetHighlightedPart(Mesh, WorldTransform, HighlightScale))
	{
		HideHighlight();
		return;
	}
	HighlightedEntity = Entity;
	HighlightedPartId = PartId;
}

bool UBuildingFocusHighlightPresenterComponent::IsHighlightVisible() const
{
	return HighlightedEntity.IsSet()
		&& HighlightedPartId != INDEX_NONE
		&& IsValid(HighlightActor)
		&& HighlightActor->IsHighlightVisible();
}

bool UBuildingFocusHighlightPresenterComponent::EnsureHighlightActor()
{
	if (IsValid(HighlightActor))
	{
		return true;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	HighlightActor = World->SpawnActor<AFocusHighlightActor>(
		AFocusHighlightActor::StaticClass(),
		FTransform::Identity,
		SpawnParameters);
	return IsValid(HighlightActor);
}

bool UBuildingFocusHighlightPresenterComponent::TryResolveFocusedPart(
	UStaticMesh*& OutMesh,
	FTransform& OutWorldTransform,
	FBuildEntityHandle& OutEntity,
	int32& OutPartId) const
{
	OutMesh = nullptr;
	OutWorldTransform = FTransform::Identity;
	OutEntity = {};
	OutPartId = INDEX_NONE;

	const AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	const UFocusHostComponent* FocusHost = PlayerController
		? PlayerController->FindComponentByClass<UFocusHostComponent>()
		: nullptr;
	const FFocusQueryHit* FocusedHit = FocusHost ? FocusHost->GetFocusedHit() : nullptr;
	const FBuildingFocusTarget* Target = FocusedHit
		? FocusedHit->Target.GetPtr<FBuildingFocusTarget>()
		: nullptr;
	UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	const UBuildingWorldSubsystem* BuildingSubsystem = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!PlayerController
		|| !PlayerController->CanUseFocusInteraction()
		|| !Target
		|| !Target->IsValid()
		|| !BuildingSubsystem
		|| !BuildingSubsystem->IsEntityAlive(Target->Entity))
	{
		return false;
	}

	const FBuildEntityRegistry& Registry = BuildingSubsystem->GetRegistry();
	const FBuildTransformFragment* Transform =
		Registry.FindFragment<FBuildTransformFragment>(Target->Entity);
	const FBuildDefinitionFragment* DefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Target->Entity);
	const FBuildPartTransformFragment* PartTransforms =
		Registry.FindFragment<FBuildPartTransformFragment>(Target->Entity);
	const UBuildingDefinition* Definition = DefinitionFragment
		? DefinitionFragment->Definition.Get()
		: nullptr;
	if (!Transform
		|| !Definition
		|| !Definition->MeshParts.IsValidIndex(Target->PartId)
		|| (PartTransforms
			&& PartTransforms->LocalTransforms.Num() != Definition->MeshParts.Num()))
	{
		return false;
	}

	const FBuildMeshPartDefinition& Part = Definition->MeshParts[Target->PartId];
	const FTransform& PartLocalTransform = PartTransforms
		? PartTransforms->LocalTransforms[Target->PartId]
		: Part.LocalTransform;
	const FTransform PartWorldTransform =
		PartLocalTransform * Transform->WorldTransform;
	if (!IsValid(Part.Mesh) || PartWorldTransform.ContainsNaN())
	{
		return false;
	}

	OutMesh = Part.Mesh;
	OutWorldTransform = PartWorldTransform;
	OutEntity = Target->Entity;
	OutPartId = Target->PartId;
	return true;
}

void UBuildingFocusHighlightPresenterComponent::HideHighlight()
{
	HighlightedEntity = {};
	HighlightedPartId = INDEX_NONE;
	if (IsValid(HighlightActor))
	{
		HighlightActor->SetHighlightVisible(false);
	}
}
