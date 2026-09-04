#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "ElementSandboxCharacterMovementComponent.generated.h"

/** 保留 UE 标准角色移动，只为项目物理对象提供有边界的接触推力策略。 */
UCLASS()
class UElementSandboxCharacterMovementComponent final : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	/** 双端在移动前预测紧接着的物理接触，让推力赶上下一次 Chaos 步，仍保留全部胶囊阻挡。 */
	virtual void PerformMovement(float DeltaSeconds) override;
	virtual void ApplyImpactPhysicsForces(
		const FHitResult& Impact,
		const FVector& ImpactAcceleration,
		const FVector& ImpactVelocity) override;

private:
	void PredictDebrisContacts(float DeltaSeconds);
	TArray<FHitResult> DebrisContactScratch;
};
