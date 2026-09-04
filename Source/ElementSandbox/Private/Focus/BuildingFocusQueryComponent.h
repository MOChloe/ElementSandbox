#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Focus/FocusQueryTypes.h"
#include "Spatial/BuildSpatialIndex.h"

#include "BuildingFocusQueryComponent.generated.h"

class UBuildingFocusHandler;

/** 使用 Building ECS Sparse Chunk + AABB Tree 的本地玩家聚焦查询。 */
UCLASS(NotBlueprintable, ClassGroup = (Focus))
class UBuildingFocusQueryComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildingFocusQueryComponent();
	UBuildingFocusHandler* GetHandler() const { return Handler; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void RunQuery(const FFocusQueryContext& Context, TArray<FFocusQueryHit>& OutHits) const;

	UPROPERTY(VisibleAnywhere, Instanced, Category = "Focus")
	TObjectPtr<UBuildingFocusHandler> Handler;

	FFocusQueryRegistrationHandle RegistrationHandle;
	mutable FBuildSpatialQueryScratch SpatialQueryScratch;
	mutable TArray<FBuildEntityHandle> BroadphaseOverlapEntities;
	mutable TArray<FBuildSpatialRayHit> BroadphaseHits;
};
