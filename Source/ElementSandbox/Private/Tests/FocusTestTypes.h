#pragma once

#include "CoreMinimal.h"
#include "Focus/FocusHandler.h"

#include "FocusTestTypes.generated.h"

USTRUCT()
struct FFocusTestTarget final
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Value = 0;
};

UCLASS()
class UFocusTestHandler final : public UFocusHandler
{
	GENERATED_BODY()

public:
	int32 FocusGainedCount = 0;
	int32 FocusLostCount = 0;
	int32 InteractCount = 0;
	int32 LastGainedValue = INDEX_NONE;
	int32 LastLostValue = INDEX_NONE;
	int32 LastInteractValue = INDEX_NONE;
	mutable int32 PromptResolveCount = 0;
	bool bProvidePrompt = false;
	FText PromptText;

protected:
	virtual bool IsSameTargetImpl(
		const FFocusQueryHit& Left,
		const FFocusQueryHit& Right) const override;
	virtual void HandleFocusGainedImpl(const FFocusQueryHit& Hit) override;
	virtual void HandleFocusLostImpl(const FFocusQueryHit& Hit) override;
	virtual bool TryResolvePromptImpl(
		const FFocusQueryHit& Hit,
		FFocusInteractionPrompt& OutPrompt) const override;
	virtual bool HandleInteractImpl(const FFocusQueryHit& Hit) override;
};
