#pragma once
#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"

/** 只属于表现的阶段；服务器 ECS 是否存在由普通 WorldObject Upsert 决定。 */
enum class EWoodProductFlightPhase : uint8 { Prepared, Active, Settled, Canceled };
struct ELEMENTSANDBOXWORLDOBJECTCATALOG_API FWoodProductFlight final
{
	FWorldEntityId WorldEntityId;
	FName DefinitionId;
	uint64 BurstId = 0;
	uint64 BatchId = 0;
	uint32 Revision = 0;
	EWoodProductFlightPhase Phase = EWoodProductFlightPhase::Prepared;
	FTransform RestTransform = FTransform::Identity;
	FVector3f StartOffset = FVector3f::ZeroVector;
	FVector3f ImpactOffset = FVector3f::ZeroVector;
	FVector3f Velocity = FVector3f::ZeroVector;
	FVector3f Acceleration = FVector3f::ZeroVector;
	FVector3f AngularVelocityDegrees = FVector3f::ZeroVector;
	FQuat4f StartRotation = FQuat4f::Identity;
	double LocalStartTime = 0.0;
	float ImpactSeconds = 0.0f;
	float SettlingSeconds = 0.0f;
	float LiftHeight = 0.0f;
	float Radius = 0.0f;

	static constexpr int32 CustomFloatCount = 28;
	static constexpr int32 PhaseIndex = 27;
	bool IsValid() const;
	/** 求各坐标轴二次函数的极值，并加入旋转和触地翻倒的保守包络。 */
	float GetDisplacementExtent() const;
	void PackCustomData(TArray<float>& Out) const;
};
