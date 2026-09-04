#include "WorldObjects/WoodProductFlightMaterialSet.h"
#include "Materials/MaterialInterface.h"

int32 UWoodProductFlightMaterialSet::ComputeTier(float Extent)
{
	if (!FMath::IsFinite(Extent) || Extent < 0) return INDEX_NONE;
	int32 Tier = 0;
	double Bound = 3200.0;
	while (Bound < Extent && Tier < 24) { ++Tier; Bound *= 2.0; }
	return Tier;
}
float UWoodProductFlightMaterialSet::GetTierExtent(int32 Tier) { return 3200.0f * FMath::Pow(2.0f, static_cast<float>(Tier)); }
int32 UWoodProductFlightMaterialSet::FindTier(float Extent) const
{
	for (int32 Index = 0; Index < Tiers.Num(); ++Index)
		if (Tiers[Index].MaximumDisplacement >= Extent) return Index;
	return INDEX_NONE;
}
UMaterialInterface* UWoodProductFlightMaterialSet::GetMaterial(bool Charcoal, int32 Tier) const
{
	if (Tier == INDEX_NONE) return Charcoal ? StaticCharcoal.Get() : StaticWood.Get();
	return Tiers.IsValidIndex(Tier) ? (Charcoal ? Tiers[Tier].Charcoal.Get() : Tiers[Tier].Wood.Get()) : nullptr;
}
