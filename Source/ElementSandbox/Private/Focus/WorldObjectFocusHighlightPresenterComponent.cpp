#include "Focus/WorldObjectFocusHighlightPresenterComponent.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Focus/FocusHighlightActor.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "Game/ElementSandboxPlayerController.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupComponent.h"
#include "WorldObjects/WorldObjectPickupResolver.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"

UWorldObjectFocusHighlightPresenterComponent::UWorldObjectFocusHighlightPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UWorldObjectFocusHighlightPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	const auto* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	const bool bLocal = Controller && Controller->IsLocalController()
		&& GetNetMode() != NM_DedicatedServer && !IsRunningCommandlet();
	SetComponentTickEnabled(bLocal);
	if (bLocal)
		if (auto* Focus = Controller->FindComponentByClass<UFocusHostComponent>())
			AddTickPrerequisiteComponent(Focus);
}

void UWorldObjectFocusHighlightPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	HideHighlight();
	if (IsValid(HighlightActor)) HighlightActor->Destroy();
	HighlightActor = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UWorldObjectFocusHighlightPresenterComponent::TickComponent(const float DeltaTime,
	const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	RefreshHighlight();
}

void UWorldObjectFocusHighlightPresenterComponent::RefreshHighlight()
{
	const auto* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	const auto* Focus = Controller ? Controller->FindComponentByClass<UFocusHostComponent>() : nullptr;
	const auto* Hit = Focus ? Focus->GetFocusedHit() : nullptr;
	const auto* Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
	UWorld* World = GetWorld();
	const auto* Objects = World ? World->GetSubsystem<UWorldObjectWorldSubsystem>() : nullptr;
	const auto* Catalog = World ? World->GetSubsystem<UWorldObjectItemCatalogSubsystem>() : nullptr;
	const auto* PickupInput = Controller ? Controller->FindComponentByClass<UWorldObjectPickupComponent>() : nullptr;
	UE::ElementSandbox::FWorldObjectPickupResolution Pickup;
	if (!Controller || !Controller->CanUseFocusInteraction() || !Target || !Objects || !Catalog
		|| GetNetMode() == NM_DedicatedServer || IsRunningCommandlet()
		|| (PickupInput && PickupInput->IsTargetUnavailable(Target->WorldEntityId))
		|| !UE::ElementSandbox::TryResolveWorldObjectPickup(*Objects,
			Objects->FindEntity(Target->WorldEntityId), *Catalog, Pickup))
	{
		HideHighlight();
		return;
	}

	UStaticMesh* Mesh = nullptr;
	FTransform Transform;
	UHierarchicalInstancedStaticMeshComponent* Instances = nullptr;
	int32 InstanceIndex = INDEX_NONE;
	const auto* Products = World->GetSubsystem<UWoodProductPresentationWorldSubsystem>();
	if (Products && Products->FindInstance(Target->WorldEntityId, Instances, InstanceIndex)
		&& IsValid(Instances) && Instances->GetInstanceTransform(InstanceIndex, Transform, true))
	{
		Mesh = Instances->GetStaticMesh();
	}
	else if (const AActor* Actor = Pickup.ProjectionActor.Get())
	{
		// 自定义木棍保留自己的可见 Mesh；自动 Box Proxy 没有 StaticMesh，不会被误画成物件。
		if (const auto* ItemMesh = Actor->FindComponentByClass<UStaticMeshComponent>())
		{
			Mesh = ItemMesh->GetStaticMesh();
			Transform = ItemMesh->GetComponentTransform();
		}
	}
	if (!IsValid(Mesh))
	{
		HideHighlight();
		return;
	}
	if (!IsValid(HighlightActor))
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		HighlightActor = World->SpawnActor<AFocusHighlightActor>(AFocusHighlightActor::StaticClass(), FTransform::Identity, Params);
	}
	if (IsValid(HighlightActor) && HighlightActor->SetHighlightedPart(Mesh, Transform, 1.025f))
		HighlightedId = Target->WorldEntityId;
	else HideHighlight();
}

bool UWorldObjectFocusHighlightPresenterComponent::IsHighlightVisible() const
{
	return HighlightedId.IsSet() && IsValid(HighlightActor) && HighlightActor->IsHighlightVisible();
}

void UWorldObjectFocusHighlightPresenterComponent::HideHighlight()
{
	HighlightedId = {};
	if (IsValid(HighlightActor)) HighlightActor->SetHighlightVisible(false);
}
