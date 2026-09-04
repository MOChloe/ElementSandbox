#include "Item/Features/ItemDisplayFeature.h"

#include "Net/UnrealNetwork.h"

void UItemDisplayFeature::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UItemDisplayFeature, DisplayName);
	DOREPLIFETIME(UItemDisplayFeature, Icon);
	DOREPLIFETIME(UItemDisplayFeature, Tint);
}

void UItemDisplayFeature::OnRep_DisplayData()
{
	NotifyItemChanged();
}
