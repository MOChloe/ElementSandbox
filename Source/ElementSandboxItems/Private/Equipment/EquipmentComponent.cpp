#include "Equipment/EquipmentComponent.h"

#include "Equipment/EquippedItemActor.h"
#include "Item/Features/EquippableItemFeature.h"
#include "Item/ItemInstance.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"

UEquipmentComponent::UEquipmentComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UEquipmentComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UEquipmentComponent, EquippedActor);
}

bool UEquipmentComponent::EquipItem(UItemInstance* ItemInstance, const UEquippableItemFeature& EquippableFeature)
{
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!IsValid(Character) || !Character->HasAuthority() || !IsValid(ItemInstance)
		|| !EquippableFeature.EquippedActorClass || !IsValid(Character->GetMesh()))
	{
		return false;
	}

	if (CurrentEquippedItem == ItemInstance && IsValid(EquippedActor))
	{
		return true;
	}

	UnequipItem();

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = Character;
	SpawnParameters.Instigator = Character;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AEquippedItemActor* NewEquippedActor = GetWorld()->SpawnActor<AEquippedItemActor>(
		EquippableFeature.EquippedActorClass, Character->GetActorTransform(), SpawnParameters);
	if (!IsValid(NewEquippedActor))
	{
		return false;
	}

	NewEquippedActor->AttachToComponent(
		Character->GetMesh(),
		FAttachmentTransformRules::SnapToTargetNotIncludingScale,
		EquippableFeature.AttachmentSocket);
	NewEquippedActor->SetActorRelativeTransform(EquippableFeature.AttachmentTransform);

	CurrentEquippedItem = ItemInstance;
	EquippedActor = NewEquippedActor;
	EquippedItemChangedEvent.Broadcast(nullptr, ItemInstance);
	return true;
}

bool UEquipmentComponent::UnequipItem()
{
	AActor* OwnerActor = GetOwner();
	if (!IsValid(OwnerActor) || !OwnerActor->HasAuthority())
	{
		return false;
	}

	UItemInstance* PreviousItem = CurrentEquippedItem;
	if (IsValid(EquippedActor))
	{
		EquippedActor->Destroy();
	}

	EquippedActor = nullptr;
	CurrentEquippedItem = nullptr;
	if (IsValid(PreviousItem))
	{
		EquippedItemChangedEvent.Broadcast(PreviousItem, nullptr);
	}
	return true;
}

bool UEquipmentComponent::ReleaseEquippedActor(
	UItemInstance* ExpectedItem,
	AEquippedItemActor*& OutReleasedActor)
{
	OutReleasedActor = nullptr;
	AActor* OwnerActor = GetOwner();
	if (!IsValid(OwnerActor)
		|| !OwnerActor->HasAuthority()
		|| !IsValid(ExpectedItem)
		|| CurrentEquippedItem != ExpectedItem
		|| !IsValid(EquippedActor))
	{
		return false;
	}

	OutReleasedActor = EquippedActor;
	EquippedActor = nullptr;
	CurrentEquippedItem = nullptr;
	EquippedItemChangedEvent.Broadcast(ExpectedItem, nullptr);
	return true;
}

void UEquipmentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GetOwner() && GetOwner()->HasAuthority() && IsValid(EquippedActor))
	{
		EquippedActor->Destroy();
	}
	EquippedActor = nullptr;
	CurrentEquippedItem = nullptr;

	Super::EndPlay(EndPlayReason);
}
