#pragma once

#include "Entity/BuildEntityHandle.h"

class FBuildEntityRegistry;

namespace UE::ElementSandbox::ElementGameplay::Private
{
/** Fire 对 Building ECS 状态与 Derived 烧黑表现的唯一写入边界。 */
class FElementFireBuildingState final
{
public:
	static bool SetBurnCustomData(FBuildEntityRegistry& Registry, FBuildEntityHandle Entity, int32 BurnCustomDataIndex,
								  float Value);
};
} // namespace UE::ElementSandbox::ElementGameplay::Private
