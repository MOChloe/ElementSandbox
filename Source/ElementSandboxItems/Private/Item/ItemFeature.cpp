#include "Item/ItemFeature.h"

#include "Item/ItemInstance.h"
#include "Net/UnrealNetwork.h"

void UItemFeature::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void UItemFeature::NotifyItemChanged() const
{
	if (UItemInstance* ItemInstance = Cast<UItemInstance>(GetOuter()))
	{
		ItemInstance->NotifyOwningInventoryChanged();
	}
}
