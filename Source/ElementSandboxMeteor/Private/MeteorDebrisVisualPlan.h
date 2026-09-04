#pragma once

#include "CoreMinimal.h"

namespace UE::ElementSandbox::Meteor
{
	struct FMeteorDebrisVisualPlanInput final
	{
		FBox SourceBounds = FBox(ForceInit);
		FBox ProductLocalBounds = FBox(ForceInit);
		FVector2D UniformScaleRange = FVector2D(1.0, 1.0);
		FVector2D AngularSpeedRange = FVector2D::ZeroVector;
		uint32 StableSeed = 0;
		int32 ProductCount = 0;

		bool IsValid() const;
	};

	struct FMeteorDebrisVisualLane final
	{
		/** 飞行期直接使用规范可拾取木块的 Transform，不再拉伸成墙板或树干。 */
		FTransform FlightWorldTransform = FTransform::Identity;
		/** 落地普通 WorldObject 沿用完全相同的均匀产品尺度。 */
		FVector SettlementScale = FVector::OneVector;
		FVector AngularVelocityDegrees = FVector::ZeroVector;
	};

	/**
	 * 不做运行时几何切片或视觉替身。飞行、落地和拾取始终是同一产品 Mesh，
	 * 并使用同一均匀尺度；源 Bounds 只决定各木块的确定性起始散布位置。
	 */
	bool BuildMeteorDebrisVisualPlan(
		const FMeteorDebrisVisualPlanInput& Input,
		TArray<FMeteorDebrisVisualLane>& OutLanes);
}
