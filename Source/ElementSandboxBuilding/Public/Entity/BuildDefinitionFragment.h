#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"
#include "UObject/StrongObjectPtr.h"

#include "BuildDefinitionFragment.generated.h"

class UBuildingDefinition;

/** 让 Building Entity 直接强持有其共享 Definition 的通用能力数据。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildDefinitionFragment : public FBuildFragment
{
	GENERATED_BODY()

	/**
	 * 有意不声明为 UPROPERTY：TStrongObjectPtr 自行 AddRef/ReleaseRef，Pool 只需正确
	 * 执行该 Fragment 的原生复制与析构。
	 */
	TStrongObjectPtr<const UBuildingDefinition> Definition;
};
