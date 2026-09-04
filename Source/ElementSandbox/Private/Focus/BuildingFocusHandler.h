#pragma once

#include "CoreMinimal.h"
#include "Focus/FocusHandler.h"

#include "BuildingFocusHandler.generated.h"

/** 解析 Door 的 E 互动或拆除锤的左键操作，并只向 Authority 提交 WorldEntityId。 */
UCLASS(NotBlueprintable)
class UBuildingFocusHandler final : public UFocusHandler
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
	virtual bool HandlePrimaryUseImpl(const FFocusQueryHit& Hit) override;
};
