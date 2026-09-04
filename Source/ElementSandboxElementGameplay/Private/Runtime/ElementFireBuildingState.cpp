#include "Runtime/ElementFireBuildingState.h"

#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildRenderCustomDataFragment.h"

namespace UE::ElementSandbox::ElementGameplay::Private
{
bool FElementFireBuildingState::SetBurnCustomData(FBuildEntityRegistry& Registry, const FBuildEntityHandle Entity,
												  const int32 BurnCustomDataIndex, const float Value)
{
	if (BurnCustomDataIndex < 0 || !FMath::IsFinite(Value))
	{
		return false;
	}
	FBuildRenderCustomDataFragment* Custom = Registry.FindMutableFragment<FBuildRenderCustomDataFragment>(Entity);
	if (!Custom)
	{
		if (FMath::IsNearlyZero(Value))
		{
			return true;
		}
		FBuildRenderCustomDataFragment NewCustom;
		NewCustom.Values.Init(0.0f, BurnCustomDataIndex + 1);
		if (!Registry.AddFragment(Entity, NewCustom))
		{
			return false;
		}
		Custom = Registry.FindMutableFragment<FBuildRenderCustomDataFragment>(Entity);
	}
	if (!Custom)
	{
		return false;
	}
	if (!Custom->Values.IsValidIndex(BurnCustomDataIndex))
	{
		Custom->Values.SetNumZeroed(BurnCustomDataIndex + 1);
	}
	Custom->Values[BurnCustomDataIndex] = Value;
	return true;
}
} // namespace UE::ElementSandbox::ElementGameplay::Private
