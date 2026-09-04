#pragma once

#include "NativeGameplayTags.h"

namespace ElementSandboxGameplayTags
{
	/** 本地输入映射到当前装备道具的主要行为。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Input_Use_Primary);

	/** 所有由装备赋予的主动行为共享的父 Tag。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_EquippedItem);

	/** 木棍挥动行为的具体 Ability Tag。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_EquippedItem_Swing);

	/** 火焰球投掷行为的具体 Ability Tag。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_EquippedItem_Fireball);

	/** 陨石打击行为的具体 Ability Tag。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_EquippedItem_MeteorStrike);

	/** 拆除锤挥击表现的具体 Ability Tag。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_EquippedItem_DemolitionTool);

	/** 任意装备主要行为执行期间由 ASC 持有。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_State_UsingEquippedItem);

	/** 角色当前持有 Burning GameplayEffect；供表现与后续 Tag 规则读取。 */
	ELEMENTSANDBOXABILITIES_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Burning);
}
