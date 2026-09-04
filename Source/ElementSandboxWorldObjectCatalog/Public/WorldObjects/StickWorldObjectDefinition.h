#pragma once

#include "CoreMinimal.h"
#include "Definition/WorldObjectDefinition.h"

#include "StickWorldObjectDefinition.generated.h"

class FWorldObjectEntityRegistry;

/** Stick content definition owned by WorldObjectCatalog. */
UCLASS(NotBlueprintable)
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API UStickWorldObjectDefinition final : public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	UStickWorldObjectDefinition();

protected:
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const override;
};
