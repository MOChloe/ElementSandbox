#include "Items/DemolitionToolItemFeature.h"

#include "Item/ItemDefinition.h"
#include "Net/UnrealNetwork.h"

bool FBuildingDismantleReward::IsValid() const
{
	return !BuildingDefinitionId.IsNone()
		&& ::IsValid(ItemDefinition.Get())
		&& Quantity > 0
		&& (!bPreservePlacementShape || Quantity == 1);
}

void UDemolitionToolItemFeature::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDemolitionToolItemFeature, Rewards);
}

bool UDemolitionToolItemFeature::TryResolveReward(
	const FName BuildingDefinitionId,
	UItemDefinition*& OutItemDefinition,
	int32& OutQuantity) const
{
	OutItemDefinition = nullptr;
	OutQuantity = 0;
	if (BuildingDefinitionId.IsNone())
	{
		return false;
	}

	if (const FBuildingDismantleReward* Reward = FindReward(BuildingDefinitionId))
	{
		OutItemDefinition = Reward->ItemDefinition;
		OutQuantity = Reward->Quantity;
		return true;
	}
	return false;
}

const FBuildingDismantleReward* UDemolitionToolItemFeature::FindReward(
	const FName BuildingDefinitionId) const
{
	if (BuildingDefinitionId.IsNone())
	{
		return nullptr;
	}
	for (const FBuildingDismantleReward& Reward : Rewards)
	{
		if (Reward.BuildingDefinitionId == BuildingDefinitionId && Reward.IsValid())
		{
			return &Reward;
		}
	}
	return nullptr;
}

void UDemolitionToolItemFeature::OnRep_DismantleRewards()
{
	NotifyItemChanged();
}
