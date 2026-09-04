#pragma once

#include "Collision/WorldObjectCollisionTypes.h"
#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "WorldObjectCollisionActivationComponent.generated.h"

class APawn;
class UPrimitiveComponent;
class UMovementComponent;

/** 为本地预测 Pawn 或服务器权威 Pawn 提交普通 WorldObject 近场碰撞 Source。 */
UCLASS(NotBlueprintable, ClassGroup = (WorldObject))
class UWorldObjectCollisionActivationComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldObjectCollisionActivationComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void UpdateMovementTickPrerequisite(UMovementComponent* NewMovementComponent);
	void UpdatePawnHitBinding(APawn* NewPawn);
	bool TryBuildSource(const APawn& Pawn, const FWorldObjectCollisionActivationConfig& Config,
		FWorldObjectCollisionSource& OutSource, FVector& OutViewDirection) const;
	bool ShouldSubmitSource(const APawn& Pawn, const FWorldObjectCollisionSource& NewSource,
		const FVector& NewViewDirection, const FWorldObjectCollisionActivationConfig& Config) const;
	void ClearSource();

	UFUNCTION()
	void HandlePawnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

	FWorldObjectCollisionSourceHandle Source;
	FWorldObjectCollisionSource LastSubmittedSource;
	FVector LastSubmittedViewDirection = FVector::ForwardVector;
	TWeakObjectPtr<APawn> LastSubmittedPawn;
	TWeakObjectPtr<UMovementComponent> PrerequisiteMovementComponent;
	TWeakObjectPtr<UPrimitiveComponent> BoundPawnCollisionComponent;
	uint64 NextSourceRevision = 1;
	bool bHasSubmittedSource = false;
};
