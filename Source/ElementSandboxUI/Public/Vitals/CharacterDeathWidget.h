#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CharacterDeathWidget.generated.h"

/** 只表现外部提交的死亡状态；不拥有 Health、重生规则或输入。 */
UCLASS()
class ELEMENTSANDBOXUI_API UCharacterDeathWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetDeathShown(bool bShown);
	bool IsDeathShown() const { return bDeathShown; }

protected:
	virtual void NativeOnInitialized() override;

private:
	bool bDeathShown = false;

	void RefreshVisibility();
};
