#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "FocusPromptPresenterComponent.generated.h"

class UInteractionPromptWidget;

/** 将本地 Focus Host 的唯一有效提示固定投影到视口中心下方。 */
UCLASS(NotBlueprintable, ClassGroup=(Focus))
class UFocusPromptPresenterComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UFocusPromptPresenterComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool EnsurePromptWidget();
	void HidePrompt();

	/** 与准星保持稳定视觉关系，不再依赖高大目标的世界 Bounds 顶点。 */
	UPROPERTY(EditDefaultsOnly, Category="Focus|Prompt")
	FVector2D PromptViewportOffset = FVector2D(0.0f, 64.0f);

	UPROPERTY(Transient)
	TObjectPtr<UInteractionPromptWidget> PromptWidget;
};
