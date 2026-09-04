#pragma once

#include "CoreMinimal.h"

/** 当前 demo 的轻量 C++ UMG 视觉令牌；Inventory 与 Vitals 共用，避免各自硬编码一套风格。 */
namespace ElementSandbox::UIStyle
{
	inline const FLinearColor PanelBackground(0.01f, 0.015f, 0.02f, 0.82f);
	inline const FLinearColor OpaquePanelBackground(0.015f, 0.02f, 0.03f, 0.94f);
	inline const FLinearColor LoadingBackdrop(0.008f, 0.014f, 0.022f, 0.985f);
	inline const FLinearColor LoadingAccent(0.10f, 0.58f, 0.82f, 1.0f);
	inline const FLinearColor IdleFrame(0.16f, 0.18f, 0.20f, 0.92f);
	inline const FLinearColor IdleSurface(0.025f, 0.03f, 0.035f, 0.92f);
	inline const FLinearColor SelectedFrame(0.10f, 0.62f, 0.84f, 0.98f);
	inline const FLinearColor SelectedSurface(0.045f, 0.075f, 0.09f, 0.96f);
	inline const FLinearColor PrimaryText(0.91f, 0.93f, 0.94f, 1.0f);
	inline const FLinearColor SecondaryText(0.58f, 0.64f, 0.68f, 1.0f);
	inline const FLinearColor HealthTrack(0.055f, 0.065f, 0.072f, 1.0f);
	inline const FLinearColor HealthFill(0.64f, 0.095f, 0.07f, 1.0f);
	inline const FLinearColor CriticalHealthFill(0.90f, 0.18f, 0.08f, 1.0f);

	inline constexpr int32 SmallFontSize = 11;
	inline constexpr int32 BodyFontSize = 13;
	inline constexpr int32 TitleFontSize = 22;
}
