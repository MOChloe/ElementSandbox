#pragma once

#include "CoreMinimal.h"
#include "Runtime/ElementRuntimeTypes.h"
#include "Shape/ElementCompoundShape.h"

struct ELEMENTSANDBOXSIMULATION_API FElementSweptShapeResult final
{
	double ContactDurationSeconds = 0.0;
	double IntegratedWeightSeconds = 0.0;
	double MaximumWeight = 0.0;
	double EndWeight = 0.0;

	bool HasContact() const { return ContactDurationSeconds > 0.0; }
};

/**
 * 固定 Shape 的专用几何内核。它只处理 Sphere/Box/Capsule，不回退到 GJK，
 * 因而同类 Shape 可由查询层按标签分桶后批量执行。
 */
class ELEMENTSANDBOXSIMULATION_API FElementShapeKernels final
{
public:
	static bool Intersects(const FElementCompoundShape& Left, const FElementCompoundShape& Right);

	/**
	 * 返回 Target 实际 Shape 体积在 Influence 中可获得的最大空间权重。
	 * AABB 只用于 Broadphase，绝不参与衰减取样；距离细节不暴露给 Processor。
	 */
	static double CalculateWeight(
		const FElementCompoundShape& Influence,
		const FElementCompoundShape& Target,
		EElementSpatialWeightMode Mode);

	/** 求两份复合 Shape 的最短真实表面距离；相交时返回 0。 */
	static double CalculateSurfaceDistance(
		const FElementCompoundShape& Left,
		const FElementCompoundShape& Right);

	/** 使用真实表面距离和固定作用距离计算 SmoothStep 权重。 */
	static double CalculateSurfaceDistanceWeight(
		const FElementCompoundShape& Origin,
		const FElementCompoundShape& Target,
		double FalloffDistanceCentimeters);

	/**
	 * Target 沿一段权威近似路径运动。算法先做扩张 AABB 线段裁剪，再用固定 Shape
	 * 内核细分接触区间；不修改 BVH，也不保存接触关系。
	 */
	static bool Sweep(
		const FElementCompoundShape& Influence,
		const FElementCompoundShape& TargetTemplate,
		const FElementMotionSegment& Segment,
		EElementSpatialWeightMode Mode,
		FElementSweptShapeResult& OutResult,
		const FElementCompoundShape* FalloffOriginShape = nullptr,
		double FalloffDistanceCentimeters = 0.0);
};
