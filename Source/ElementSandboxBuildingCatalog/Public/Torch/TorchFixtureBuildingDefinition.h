#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"

#include "TorchFixtureBuildingDefinition.generated.h"

class FBuildEntityRegistry;

/** 火把插入建筑插槽后的 Building 形态；既是固定火源，也是普通可燃 Target。 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXBUILDINGCATALOG_API UTorchFixtureBuildingDefinition final
	: public UBuildingDefinition
{
	GENERATED_BODY()

public:
	UTorchFixtureBuildingDefinition();

protected:
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const override;
};
