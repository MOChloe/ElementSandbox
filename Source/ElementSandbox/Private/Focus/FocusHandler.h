#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FocusHandler.generated.h"

struct FFocusQueryHit;
struct FFocusInteractionPrompt;

/**
 * 一个 Focus Query 的类型化目标处理器。
	 * Host 负责排序与状态切换；Handler 只比较自己的 Target，并接收聚焦进入、离开和离散操作。
 */
UCLASS(Abstract, NotBlueprintable)
class UFocusHandler : public UObject
{
	GENERATED_BODY()

public:
	bool IsSameTarget(const FFocusQueryHit& Left, const FFocusQueryHit& Right) const;
	void HandleFocusGained(const FFocusQueryHit& Hit);
	void HandleFocusLost(const FFocusQueryHit& Hit);
	bool TryResolvePrompt(
		const FFocusQueryHit& Hit,
		FFocusInteractionPrompt& OutPrompt) const;
	bool HandleInteract(const FFocusQueryHit& Hit);
	bool HandlePrimaryUse(const FFocusQueryHit& Hit);

protected:
	virtual bool IsSameTargetImpl(
		const FFocusQueryHit& Left,
		const FFocusQueryHit& Right) const PURE_VIRTUAL(
		UFocusHandler::IsSameTargetImpl,
		return false;);

	virtual void HandleFocusGainedImpl(const FFocusQueryHit& Hit) {}
	virtual void HandleFocusLostImpl(const FFocusQueryHit& Hit) {}
	virtual bool TryResolvePromptImpl(
		const FFocusQueryHit& Hit,
		FFocusInteractionPrompt& OutPrompt) const
	{
		return false;
	}
	virtual bool HandleInteractImpl(const FFocusQueryHit& Hit) { return false; }
	virtual bool HandlePrimaryUseImpl(const FFocusQueryHit& Hit) { return false; }
};
