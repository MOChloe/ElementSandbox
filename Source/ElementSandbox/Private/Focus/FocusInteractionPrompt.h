#pragma once

#include "CoreMinimal.h"

/** 当前 Focus Handler 已重新验证的本地交互提示。 */
struct FFocusInteractionPrompt final
{
	FText Text;

	bool IsValid() const
	{
		return !Text.IsEmpty();
	}
};
