#include "Definition/WorldDestructionDefinition.h"

#include "Definition/WorldObjectDefinition.h"

bool FWorldDestructionDefinition::IsEnabled() const
{
	return MaxDurability > UE_SMALL_NUMBER && ProductClass != nullptr;
}

namespace
{
	bool IsFiniteOrderedRange(const FVector2D& Range, const double Minimum)
	{
		return FMath::IsFinite(Range.X)
			&& FMath::IsFinite(Range.Y)
			&& Range.X >= Minimum
			&& Range.Y >= Range.X;
	}
}

bool FWorldDestructionDefinition::IsValid() const
{
	if (!IsEnabled())
	{
		// ProductClass 是明确开启破坏的开关；普通 WorldObject 可保留
		// 100 点第一版默认耐久，但在未配置产物时仍不可被破坏。
		return FMath::IsFinite(MaxDurability) && MaxDurability >= 0.0;
	}

	return FMath::IsFinite(MaxDurability)
		&& MaxDurability > UE_SMALL_NUMBER
		&& MinimumProductCount > 0
		&& MaximumProductCount >= MinimumProductCount
		&& IsFiniteOrderedRange(UniformScaleRange, UE_SMALL_NUMBER)
		&& !SpawnOffsetExtent.ContainsNaN()
		&& SpawnOffsetExtent.GetMin() >= 0.0
		&& IsFiniteOrderedRange(HorizontalSpeedRange, 0.0)
		&& IsFiniteOrderedRange(UpwardSpeedRange, 0.0)
		&& IsFiniteOrderedRange(AngularSpeedRange, 0.0)
		&& FMath::IsFinite(ProductMassKg)
		&& ProductMassKg > UE_SMALL_NUMBER;
}
