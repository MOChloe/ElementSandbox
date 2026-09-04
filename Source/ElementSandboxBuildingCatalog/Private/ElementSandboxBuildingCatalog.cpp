#include "Combustion/BuildCombustionCatalog.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Door/DoorBuildingDefinition.h"
#include "Modules/ModuleManager.h"
#include "Torch/TorchDefinition.h"
#include "Torch/TorchFixtureBuildingDefinition.h"
#include "Wood/WoodBuildingDefinition.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ElementSandboxBuildingCatalog)

bool TryGetBuildCombustionConfiguration(
	const UBuildingDefinition& Definition,
	int32& OutBurnCustomDataIndex)
{
	OutBurnCustomDataIndex = INDEX_NONE;
	if (Definition.IsA<UCityBuildingPieceDefinition>()
		|| Definition.IsA<UWoodBuildingDefinition>()
		|| Definition.IsA<UDoorBuildingDefinition>()
		|| Definition.IsA<UTorchFixtureBuildingDefinition>())
	{
		OutBurnCustomDataIndex = 0;
		return true;
	}
	return false;
}

bool TryGetBuildFixedFireEmitterKind(
	const FName DefinitionId,
	EBuildFixedFireEmitterKind& OutKind)
{
	if (DefinitionId == TEXT("FirePile"))
	{
		OutKind = EBuildFixedFireEmitterKind::FirePile;
		return true;
	}
	if (DefinitionId == GetMountedTorchBuildingDefinitionId())
	{
		OutKind = EBuildFixedFireEmitterKind::MountedTorch;
		return true;
	}
	return false;
}
