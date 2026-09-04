#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"

#include "BuildingPlacementTestTypes.generated.h"

/** 创建阶段固定失败，用于证明背包扣除与 Building 创建是同一可回滚事务。 */
UCLASS()
class UFailingBuildingPlacementDefinition final : public UBuildingDefinition
{
	GENERATED_BODY()

protected:
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const override
	{
		return false;
	}
};
