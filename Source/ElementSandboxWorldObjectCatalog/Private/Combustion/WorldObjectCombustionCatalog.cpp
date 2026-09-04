#include "Combustion/WorldObjectCombustionCatalog.h"

#include "Tree/SettlementTreeTypes.h"

bool TryGetWorldObjectCombustionProfileKind(
	const FName DefinitionId,
	EWorldObjectCombustionProfileKind& OutKind)
{
	if (DefinitionId == TEXT("Stick"))
	{
		OutKind = EWorldObjectCombustionProfileKind::Stick;
		return true;
	}
	if (DefinitionId == SettlementTreeDefinitionId)
	{
		// 树与普通木结构共享同一固定燃料档，避免复制另一套强度、范围与时长。
		OutKind = EWorldObjectCombustionProfileKind::Structure;
		return true;
	}
	return false;
}
