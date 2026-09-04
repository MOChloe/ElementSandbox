#pragma once

#include "CoreMinimal.h"

#include "BuildFragment.generated.h"

/**
 * Building ECS Fragment 的空标记基类。
 *
 * Fragment 只保存可复制的数据，不执行 Tick、不调用外部系统。反射可见的 UObject
 * 引用仍被 Store 拒绝；经明确设计并覆盖原生复制/析构测试的 RAII 值成员可以自行
 * 管理外部生命周期。运行时数据由 FBuildEntityRegistry 的类型化 Pool 独占。
 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildFragment
{
	GENERATED_BODY()
};
