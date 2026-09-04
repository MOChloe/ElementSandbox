#pragma once

#include "CoreMinimal.h"
#include "Collision/BuildCollisionTypes.h"
#include "ElementGameplayWorldSubsystem.h"
#include "GameFramework/Actor.h"
#include "GameplayPrediction.h"

#include "FireballProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;

/** 发射初态必须作为一个 RepNotify 单元到达，避免速度先于 PredictionKey 触发双表现。 */
USTRUCT()
struct FFireballLaunchState final
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize100 Location = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize10 Velocity = FVector::ZeroVector;

	UPROPERTY()
	float ServerTimeSeconds = 0.0f;

	/** GAS PredictionKey 在对应 owning connection 上有效，用于接管本地预测球。 */
	UPROPERTY()
	FPredictionKey PredictionKey;
};

/** 服务端权威命中、客户端逐帧预测表现的短寿命火焰球投射物。 */
UCLASS()
class AFireballProjectile final : public AActor
{
	GENERATED_BODY()

  public:
	static constexpr float LaunchSpeedCentimetersPerSecond = 2600.0f;
	static constexpr float MaximumFlightSeconds = 5.0f;

	AFireballProjectile();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	/** Authority 逐帧 Sweep；飞行中仍只复制发射初态和最终命中。 */
	bool LaunchAuthority(const FVector& Direction, const FPredictionKey& PredictionKey);

	/** 本地预测 Ability 当帧创建的纯表现球；按客户端帧率积分，不产生 Gameplay 后果。 */
	bool LaunchLocalPrediction(const FVector& Direction, const FPredictionKey& PredictionKey);
	void RejectLocalPrediction();

	/** 仅 Authority 调用；命中点成为临时 Host，火源与残留表现时长读取唯一 Fire RuleSet。 */
	bool ImpactAtLocation(const FVector& InImpactLocation, const FVector& InImpactNormal);

	bool HasImpacted() const
	{
		return bImpacted;
	}
	bool IsLocalPredictionProxy() const
	{
		return bLocalPredictionProxy;
	}
	const FPredictionKey& GetLaunchPredictionKey() const
	{
		return LaunchState.PredictionKey;
	}
	FElementRuntimeFireSourceHandle GetImpactFireSource() const { return ImpactFireSource; }

  protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  private:
	UFUNCTION()
	void OnRep_Impacted();

	UFUNCTION()
	void OnRep_LaunchState();

	void UpdateVisualState();
	void EnsureVisualComponents();
	void StartClientFlightPrediction();
	void ApplyAuthoritativeImpact();
	void AdvanceClientPrediction(float DeltaSeconds);
	bool AdvanceAuthorityFlight(float DeltaSeconds);
	bool RefreshBuildingCollisionSource(
		const FVector& SegmentStart,
		const FVector& SegmentEnd,
		const FVector& Velocity);
	void ReleaseBuildingCollisionSource();
	void InitializeFlightState(const FVector& Velocity, float ElapsedSeconds = 0.0f);
	FVector GetFlightGravity() const;
	AFireballProjectile* FindMatchingLocalPrediction() const;
	float GetSynchronizedServerTimeSeconds() const;
	void ExpireAndDestroy();
	void ReleaseImpactFire();

	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<USphereComponent> CollisionSphere;

	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UStaticMeshComponent> FlightBall;

	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UStaticMeshComponent> LowerImpactFlame;

	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UStaticMeshComponent> UpperImpactFlame;

	UPROPERTY(ReplicatedUsing = OnRep_Impacted)
	bool bImpacted = false;

	UPROPERTY(ReplicatedUsing = OnRep_LaunchState)
	FFireballLaunchState LaunchState;

	UPROPERTY(Replicated)
	FVector_NetQuantize100 ImpactLocation = FVector::ZeroVector;

	UPROPERTY(Replicated)
	FVector_NetQuantizeNormal ImpactNormal = FVector::UpVector;

	FElementRuntimeFireSourceHandle ImpactFireSource;
	FBuildCollisionSourceHandle BuildingCollisionSource;
	FTimerHandle ExpireTimerHandle;
	FVector FlightVelocity = FVector::ZeroVector;
	float SimulatedFlightSeconds = 0.0f;
	bool bReleasingImpactFire = false;
	uint64 BuildingCollisionSourceRevision = 1;
	bool bLocalPredictionProxy = false;
	bool bVisualInitialized = false;
};
