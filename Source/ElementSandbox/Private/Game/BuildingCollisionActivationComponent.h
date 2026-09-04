#pragma once

#include "Collision/BuildCollisionTypes.h"
#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "BuildingCollisionActivationComponent.generated.h"

class UMovementComponent;
class APawn;

/**
 * 为本地预测 Pawn 或服务器权威 Pawn 注册局部 Chaos Collision Source。
 * Source 只描述主体、运动预测和镜头区域；碰撞驻留策略仍由 Building Subsystem 持有。
 */
UCLASS(NotBlueprintable, ClassGroup = (Building))
class UBuildingCollisionActivationComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildingCollisionActivationComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void UpdateMovementTickPrerequisite(UMovementComponent* NewMovementComponent);
	bool TryBuildSource(
		const APawn& Pawn,
		const FBuildCollisionActivationConfig& Config,
		FBuildCollisionSource& OutSource,
		FVector& OutViewDirection) const;
	bool ShouldSubmitSource(
		const APawn& Pawn,
		const FBuildCollisionSource& NewSource,
		const FVector& NewViewDirection,
		const FBuildCollisionActivationConfig& Config) const;
	void ClearSource();

	FBuildCollisionSourceHandle Source;
	FBuildCollisionSource LastSubmittedSource;
	FVector LastSubmittedViewDirection = FVector::ForwardVector;
	TWeakObjectPtr<APawn> LastSubmittedPawn;
	TWeakObjectPtr<UMovementComponent> PrerequisiteMovementComponent;
	uint64 NextSourceRevision = 1;
	bool bHasSubmittedSource = false;
};
