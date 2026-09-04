#pragma once

#include "CoreMinimal.h"
#include "Definition/WorldObjectDefinition.h"

#include "CharcoalWorldObjectDefinition.generated.h"

class FWorldObjectEntityRegistry;

inline const FName CharcoalWorldObjectDefinitionId(TEXT("WorldObject.Charcoal"));

/** 燃尽转换生成的固定木炭块；进入 LooseDebris 物理生命周期，但自身不再燃烧或破坏。 */
UCLASS(NotBlueprintable)
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API UCharcoalWorldObjectDefinition final
	: public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	UCharcoalWorldObjectDefinition();

protected:
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const override;
};
