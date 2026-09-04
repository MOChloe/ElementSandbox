#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Tree/SettlementTreeTypes.h"

#include "SettlementTreeCollisionSourceComponent.generated.h"

class UMovementComponent;
class APawn;

/** 本地预测 Pawn / Authority Pawn 的树碰撞观察源；不持有碰撞实例。 */
UCLASS(NotBlueprintable, ClassGroup=(WorldObject))
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API USettlementTreeCollisionSourceComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	USettlementTreeCollisionSourceComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void ClearSource();
	void UpdateMovementTickPrerequisite(UMovementComponent* MovementComponent);

	FSettlementTreeCollisionSourceHandle SourceHandle;
	TWeakObjectPtr<UMovementComponent> PrerequisiteMovementComponent;
	TWeakObjectPtr<APawn> LastSubmittedPawn;
	FSettlementTreeCollisionSource LastSubmittedSource;
	bool bHasSubmittedSource = false;
	uint64 NextRevision = 1;
};
