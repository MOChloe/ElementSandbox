#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "MeteorStrikeActor.generated.h"

class USceneComponent;
class UStaticMeshComponent;

/** 单颗陨石的轻量复制表现状态；没有刚体、碰撞或逐帧位置复制。 */
USTRUCT()
struct FMeteorStrikeVisualState final
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize100 StartLocation = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize100 ImpactLocation = FVector::ZeroVector;

	UPROPERTY()
	float StartServerTimeSeconds = 0.0f;

	UPROPERTY()
	float ImpactServerTimeSeconds = 0.0f;

	UPROPERTY()
	float MeteorDiameterCentimeters = 0.0f;

	bool IsValid() const;
};

UCLASS()
class AMeteorStrikeActor final : public AActor
{
	GENERATED_BODY()

public:
	AMeteorStrikeActor();
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Authority 在排程 Burst 前写入完整绝对时间状态；之后只依赖 RepNotify。 */
	bool LaunchAuthority(
		const FVector& StartLocation,
		const FVector& ImpactLocation,
		double StartTimeSeconds,
		double ImpactTimeSeconds,
		float MeteorDiameterCentimeters);

private:
	UFUNCTION()
	void OnRep_VisualState();
	void ApplyAbsoluteTime();
	void ApplyVisualScale();
	float GetServerTimeSeconds() const;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> MeteorMesh;

	UPROPERTY(ReplicatedUsing=OnRep_VisualState)
	FMeteorStrikeVisualState VisualState;
};
