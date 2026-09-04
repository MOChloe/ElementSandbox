#pragma once

#include "CoreMinimal.h"
#include "MeteorRuntimeTypes.h"

namespace UE::ElementSandbox::Meteor
{
	/** BallisticGroundPlane 的标量契约与 4-lane 页面编译器。 */
	class ELEMENTSANDBOXMETEOR_API FMeteorBallisticKernel final
	{
	public:
		static bool SolveGroundIntersectionSeconds(
			float StartZ,
			float VelocityZ,
			float GravityZ,
			float GroundPlaneZ,
			float& OutSeconds);

		/** 由水平径向、方位散射、仰角和速度构造爆炸半球初速度。 */
		static bool BuildExplosionLaunchVelocity(
			const FVector3f& HorizontalDirection,
			float AzimuthOffsetDegrees,
			float ElevationDegrees,
			float Speed,
			FVector3f& OutVelocity);

		/**
		 * 在源点相对爆心的外向半平面内采样速度。偏转严格小于 90°，
		 * 因而任何有效输出的水平主速度都不会指回爆心。
		 */
		static bool BuildOutwardExplosionLaunchVelocity(
			const FVector3f& HorizontalRadial,
			float AzimuthDeviationDegrees,
			float ElevationDegrees,
			float Speed,
			FVector3f& OutVelocity);

		static FVector3f SamplePosition(
			const FVector3f& Start,
			const FVector3f& Velocity,
			const FVector3f& Acceleration,
			float Seconds);
		static FVector3f SampleVelocity(
			const FVector3f& Velocity,
			const FVector3f& Acceleration,
			float Seconds);
		static FQuat4f SampleRotation(
			const FQuat4f& StartRotation,
			const FVector3f& AngularVelocityDegrees,
			float Seconds);
		/** 选择最低势能支撑面，并用最短倾倒旋转得到精确躺平姿态。 */
		static FQuat4f ComputeStableRestRotation(
			const FBox3f& ProductLocalBounds,
			const FVector3f& Scale,
			const FQuat4f& ImpactRotation);
		/** CPU/Shader 共用的触地后解析结算曲线。NormalizedTime 位于 [0,1]。 */
		static void SampleSettlingPose(
			const FVector3f& ImpactPosition,
			const FQuat4f& ImpactRotation,
			const FVector3f& RestPosition,
			const FQuat4f& RestRotation,
			float LiftHeight,
			float NormalizedTime,
			FVector3f& OutPosition,
			FQuat4f& OutRotation);

		/**
		 * 返回 Actor 中心相对地面的高度，使给定 Bounds 在最终 Scale/Rotation 下的
		 * 最低点恰好接触地面。解析等待态与普通 WorldObject 共用这个中心姿态。
		 */
		static float ComputeGroundContactCenterOffsetZ(
			const FBox3f& ProductLocalBounds,
			const FVector3f& Scale,
			const FQuat4f& Rotation);
		/** 为姿态插值求最小抬升包络，避免长方体翻滚过程中穿过统一地面。 */
		static float ComputeSettlingLiftHeight(
			const FBox3f& ProductLocalBounds,
			const FVector3f& Scale,
			float GroundPlaneZ,
			const FVector3f& ImpactPosition,
			const FQuat4f& ImpactRotation,
			const FVector3f& RestPosition,
			const FQuat4f& RestRotation);

		/** 页面输入封闭后调用；输出不引用 Work Page，允许源槽立即回收。 */
		static bool CompilePage(
			const FMeteorWorkPage& WorkPage,
			const FMeteorRuntimeConfig& Config,
			FMeteorBurstId BurstId,
			uint64 PageId,
			FMeteorTrajectoryPage& OutPage);
	};
}
