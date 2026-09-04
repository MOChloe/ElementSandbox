#pragma once

#include "CoreMinimal.h"
#include "Focus/FocusHandler.h"

#include "WorldObjectFocusHandler.generated.h"

/** 把可拾取 Portable 的本地 Focus 转成只携带 WorldEntityId 的拾取请求。 */
UCLASS(NotBlueprintable)
class UWorldObjectFocusHandler final : public UFocusHandler
{
	GENERATED_BODY()

protected:
	virtual bool IsSameTargetImpl(
		const FFocusQueryHit& Left,
		const FFocusQueryHit& Right) const override;
	virtual bool TryResolvePromptImpl(
		const FFocusQueryHit& Hit,
		FFocusInteractionPrompt& OutPrompt) const override;
	virtual bool HandleInteractImpl(const FFocusQueryHit& Hit) override;
};
