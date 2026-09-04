#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

/** 非 Dedicated Client 启动时创建一次的三 LOD 单材质低模树。 */
class FSettlementTreeMeshFactory final
{
public:
	static UStaticMesh* Create(UObject& Outer);
};
