#pragma once

#include "CoreMinimal.h"

/** WorldObject 内容选择的 Fire 数值档；具体数值仍只来自 UElementFireRuleSet。 */
enum class EWorldObjectCombustionProfileKind : uint8
{
	Structure,
	Stick
};

/**
 * 显式声明具体 WorldObject Definition 是否可燃以及复用哪一档规则。
 * Mesh、SurfaceProfileId 或是否已有运行期 Fragment 都不能隐式授予 Fire 资格。
 */
ELEMENTSANDBOXWORLDOBJECTCATALOG_API bool TryGetWorldObjectCombustionProfileKind(
	FName DefinitionId,
	EWorldObjectCombustionProfileKind& OutKind);
