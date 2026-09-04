#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

/** 生成固定 140x48x40cm 演示木块网格；编辑器资产与运行时测试共用同一几何。 */
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API FWoodBlockMeshFactory final
{
public:
	static UStaticMesh* Create(
		UObject& Outer,
		FName ObjectName = TEXT("WoodBlockRuntimeMesh"),
		EObjectFlags Flags = RF_Transient);
};
