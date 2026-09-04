#pragma once

#include "CoreMinimal.h"
#include "Shape/ElementCompoundShape.h"

/** Element Fragment 的无行为数据根类型；禁止虚函数和 UObject 引用。 */
struct ELEMENTSANDBOXSIMULATION_API FElementFragment
{
};

/** 会向外产生空间影响的 Fragment 公共头；具体元素在派生值类型中追加自己的规则数据。 */
struct ELEMENTSANDBOXSIMULATION_API FElementInfluenceFragment : FElementFragment
{
	FElementCompoundShape Shape;
	FElementSpatialSnapshotHandle SpatialSnapshot;
	uint64 Revision = 1;
};

