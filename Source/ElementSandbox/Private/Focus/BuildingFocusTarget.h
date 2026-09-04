#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

#include "BuildingFocusTarget.generated.h"

/** Building Focus Query 与后续表现/互动层之间的进程内稳定目标身份。 */
USTRUCT()
struct FBuildingFocusTarget final
{
	GENERATED_BODY()

	FBuildEntityHandle Entity;
	int32 PartId = INDEX_NONE;

	bool IsValid() const
	{
		return Entity.IsSet() && PartId != INDEX_NONE;
	}
};
