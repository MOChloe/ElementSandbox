#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "InteractionPromptWidget.generated.h"

class UTextBlock;

/** 只绘制外部提交的交互文字；不读取 Focus 或 Gameplay 状态。 */
UCLASS()
class ELEMENTSANDBOXUI_API UInteractionPromptWidget final : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetPromptText(const FText& InText);
	const FText& GetPromptText() const { return PromptText; }

protected:
	virtual void NativeOnInitialized() override;

private:
	void RefreshText();

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PromptTextBlock;

	FText PromptText;
};
