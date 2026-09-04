#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"

#include "FirePileBuildingDefinition.generated.h"

class FBuildEntityRegistry;

/**
 * Demo 永续火焰堆的共享 Building 配置。
 *
 * Definition 只描述宿主外形；固定 Emitter 资格由 Catalog 按 DefinitionId 显式登记，
 * 强度、范围、策略和 Capsule 全部由 ElementGameplay 的唯一 RuleSet 冻结。
 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXBUILDINGCATALOG_API UFirePileBuildingDefinition final
	: public UBuildingDefinition
{
	GENERATED_BODY()

public:
	UFirePileBuildingDefinition();

protected:
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const override;
};
