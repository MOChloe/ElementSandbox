#pragma once

#include "Definition/WorldObjectDefinition.h"

#include "WorldObjectTestFailingDefinition.generated.h"

class FWorldObjectEntityRegistry;

/** Automation 专用：验证 Definition 配置失败会回滚整次 Entity 创建。 */
UCLASS()
class UWorldObjectTestFailingDefinition final : public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	UWorldObjectTestFailingDefinition();

protected:
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const override;
};
