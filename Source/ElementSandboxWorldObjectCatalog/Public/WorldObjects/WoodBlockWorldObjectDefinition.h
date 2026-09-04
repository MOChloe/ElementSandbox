#pragma once

#include "CoreMinimal.h"
#include "Definition/WorldObjectDefinition.h"

#include "WoodBlockWorldObjectDefinition.generated.h"

class FWorldObjectEntityRegistry;

inline const FName WoodBlockWorldObjectDefinitionId(TEXT("WorldObject.WoodBlock"));

/** 固定 Mesh 木块的 Gameplay 定义；可拾取、可进入 Physics，但自身不可继续破坏。 */
UCLASS(NotBlueprintable)
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API UWoodBlockWorldObjectDefinition final
	: public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	UWoodBlockWorldObjectDefinition();

protected:
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const override;
};

