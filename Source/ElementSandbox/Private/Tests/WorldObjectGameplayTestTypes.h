#pragma once

#include "Definition/WorldObjectDefinition.h"

#include "WorldObjectGameplayTestTypes.generated.h"

/** 只为 Gameplay 测试提供 Physics Fragment，不预建逐实例拾取数据。 */
UCLASS()
class UWorldObjectGameplayTestPhysicsDefinition final : public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	UWorldObjectGameplayTestPhysicsDefinition();

protected:
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const override;
};
