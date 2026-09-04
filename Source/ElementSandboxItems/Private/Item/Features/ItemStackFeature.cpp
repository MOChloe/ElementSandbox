#include "Item/Features/ItemStackFeature.h"

#include "Net/UnrealNetwork.h"

void UItemStackFeature::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UItemStackFeature, MaxStackSize);
	DOREPLIFETIME(UItemStackFeature, Quantity);
}

void UItemStackFeature::SetQuantity(const int32 InQuantity)
{
	Quantity = FMath::Clamp(InQuantity, 1, FMath::Max(1, MaxStackSize));
	NotifyItemChanged();
}

int32 UItemStackFeature::AddQuantity(const int32 Amount)
{
	if (Amount <= 0)
	{
		return 0;
	}

	const int32 AddedAmount = FMath::Min(Amount, GetAvailableSpace());
	Quantity += AddedAmount;
	if (AddedAmount > 0)
	{
		NotifyItemChanged();
	}
	return AddedAmount;
}

void UItemStackFeature::OnRep_StackData()
{
	NotifyItemChanged();
}
