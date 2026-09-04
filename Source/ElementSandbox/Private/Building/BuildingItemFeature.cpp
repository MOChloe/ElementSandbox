#include "Building/BuildingItemFeature.h"

#include "Net/UnrealNetwork.h"

void UBuildingItemFeature::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UBuildingItemFeature, BuildingDefinitionId);
	DOREPLIFETIME(UBuildingItemFeature, PlacementShapeTransform);
}

bool UBuildingItemFeature::ConfigureReclaimedBuilding(
	const FName InBuildingDefinitionId,
	const FTransform& InPlacementShapeTransform)
{
	const FVector Scale = InPlacementShapeTransform.GetScale3D();
	const FQuat Rotation = InPlacementShapeTransform.GetRotation();
	if (InBuildingDefinitionId.IsNone()
		|| InPlacementShapeTransform.ContainsNaN()
		|| !InPlacementShapeTransform.GetLocation().IsNearlyZero()
		|| !Rotation.IsNormalized()
		|| !FMath::IsFinite(Scale.X) || !FMath::IsFinite(Scale.Y) || !FMath::IsFinite(Scale.Z)
		|| Scale.X <= UE_SMALL_NUMBER || Scale.Y <= UE_SMALL_NUMBER || Scale.Z <= UE_SMALL_NUMBER)
	{
		return false;
	}

	BuildingDefinitionId = InBuildingDefinitionId;
	PlacementShapeTransform = InPlacementShapeTransform;
	NotifyItemChanged();
	return true;
}

void UBuildingItemFeature::OnRep_BuildingData()
{
	NotifyItemChanged();
}
