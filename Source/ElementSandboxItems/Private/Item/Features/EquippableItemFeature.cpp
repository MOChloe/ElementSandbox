#include "Item/Features/EquippableItemFeature.h"

#include "Net/UnrealNetwork.h"

void UEquippableItemFeature::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UEquippableItemFeature, EquippedActorClass);
	DOREPLIFETIME(UEquippableItemFeature, AttachmentSocket);
	DOREPLIFETIME(UEquippableItemFeature, AttachmentTransform);
}

void UEquippableItemFeature::OnRep_EquipmentData()
{
	NotifyItemChanged();
}
