#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Focus/FocusQueryTypes.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "Entity/WorldEntityId.h"
#include "WorldObjects/WorldObjectPickupResolver.h"

#include "WorldObjectFocusQueryComponent.generated.h"

class UWorldObjectFocusHandler;

/** 通过 Portable 空间索引执行附近辅助、直接瞄准和目标保持；不依赖 Chaos Sleep。 */
UCLASS(NotBlueprintable, ClassGroup=(Focus))
class UWorldObjectFocusQueryComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldObjectFocusQueryComponent();
	UWorldObjectFocusHandler* GetHandler() const { return Handler; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FCandidate final
	{
		UE::ElementSandbox::FWorldObjectPickupResolution Pickup;
		FWorldEntityId WorldEntityId;
		FVector Location = FVector::ZeroVector;
		double Distance = 0.0;
		double Score = 0.0;
		bool bDirectAim = false;
	};

	void RunQuery(const FFocusQueryContext& Context, TArray<FFocusQueryHit>& OutHits) const;

	UPROPERTY(VisibleAnywhere, Instanced, Category="Focus")
	TObjectPtr<UWorldObjectFocusHandler> Handler;

	FFocusQueryRegistrationHandle RegistrationHandle;
	mutable FWorldObjectSpatialQueryScratch SpatialQueryScratch;
	mutable TArray<FWorldObjectEntityHandle> BroadphaseEntities;
	mutable TArray<FCandidate> CandidateScratch;
};
