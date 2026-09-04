#pragma once

#include "CoreMinimal.h"

class UBuildingDefinition;

/** Building 内容显式声明的永续固定火源能力；同一宿主也可以独立声明为可燃目标。 */
enum class EBuildFixedFireEmitterKind : uint8
{
	FirePile,
	MountedTorch
};

/**
 * Catalog 对生产 Building Definition 的显式燃烧资格映射。
 * SurfaceProfileId 仍是中性内容标签；ElementGameplay 不从 Wall/Stone/Wood 名称猜资格。
 */
ELEMENTSANDBOXBUILDINGCATALOG_API bool TryGetBuildCombustionConfiguration(
	const UBuildingDefinition& Definition,
	int32& OutBurnCustomDataIndex);

/** 按稳定 DefinitionId 解析固定火源；未登记的 Building 不会被名称猜成 Emitter。 */
ELEMENTSANDBOXBUILDINGCATALOG_API bool TryGetBuildFixedFireEmitterKind(
	FName DefinitionId,
	EBuildFixedFireEmitterKind& OutKind);
