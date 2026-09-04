#pragma once

#include "CoreMinimal.h"
#include "Definition/WorldObjectDefinition.h"

#include "WorldObjectTestPhysicsDefinition.generated.h"

UCLASS()
class UWorldObjectTestPhysicsDefinition final : public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	UWorldObjectTestPhysicsDefinition();

protected:
	virtual bool ConfigureEntity(
		FWorldObjectEntityRegistry& Registry,
		FWorldObjectEntityHandle Entity) const override;
};
