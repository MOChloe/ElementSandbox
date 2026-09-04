#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Entity/WorldEntityId.h"
#include "WorldObjectFocusHighlightPresenterComponent.generated.h"

class AFocusHighlightActor;

/** 仅解析当前 WorldObject 的已存在渲染投影，最多用一个本地 Actor 显示高亮。 */
UCLASS(NotBlueprintable, ClassGroup=(Focus))
class UWorldObjectFocusHighlightPresenterComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldObjectFocusHighlightPresenterComponent();
	void RefreshHighlight();
	bool IsHighlightVisible() const;
	FWorldEntityId GetHighlightedId() const { return HighlightedId; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void HideHighlight();
	UPROPERTY(Transient)
	TObjectPtr<AFocusHighlightActor> HighlightActor;
	FWorldEntityId HighlightedId;
};
