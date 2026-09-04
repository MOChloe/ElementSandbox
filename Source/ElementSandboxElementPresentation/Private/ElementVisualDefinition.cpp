#include "ElementVisualDefinition.h"

#include "Engine/StaticMesh.h"

bool FElementVisualDefinition::IsValid() const
{
	return !DefinitionId.IsNone() && ::IsValid(StaticMesh.Get())
		&& CustomDataFloatCount >= 0 && CustomDataFloatCount <= 5;
}
