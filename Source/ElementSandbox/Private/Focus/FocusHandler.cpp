#include "Focus/FocusHandler.h"

#include "Focus/FocusInteractionPrompt.h"
#include "Focus/FocusQueryTypes.h"

bool UFocusHandler::IsSameTarget(
	const FFocusQueryHit& Left,
	const FFocusQueryHit& Right) const
{
	return Left.IsValid() && Right.IsValid() && IsSameTargetImpl(Left, Right);
}

void UFocusHandler::HandleFocusGained(const FFocusQueryHit& Hit)
{
	if (Hit.IsValid())
	{
		HandleFocusGainedImpl(Hit);
	}
}

void UFocusHandler::HandleFocusLost(const FFocusQueryHit& Hit)
{
	if (Hit.IsValid())
	{
		HandleFocusLostImpl(Hit);
	}
}

bool UFocusHandler::TryResolvePrompt(
	const FFocusQueryHit& Hit,
	FFocusInteractionPrompt& OutPrompt) const
{
	OutPrompt = {};
	if (!Hit.IsValid()
		|| !TryResolvePromptImpl(Hit, OutPrompt)
		|| !OutPrompt.IsValid())
	{
		OutPrompt = {};
		return false;
	}
	return true;
}

bool UFocusHandler::HandleInteract(const FFocusQueryHit& Hit)
{
	return Hit.IsValid() && HandleInteractImpl(Hit);
}

bool UFocusHandler::HandlePrimaryUse(const FFocusQueryHit& Hit)
{
	return Hit.IsValid() && HandlePrimaryUseImpl(Hit);
}
