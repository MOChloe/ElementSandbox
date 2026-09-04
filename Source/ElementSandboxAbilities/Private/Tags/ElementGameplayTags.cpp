#include "Tags/ElementGameplayTags.h"

namespace ElementSandboxGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Input_Use_Primary, "Input.Use.Primary", "当前装备道具的主要使用输入");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		Ability_Type_EquippedItem,
		"Ability.Type.EquippedItem",
		"由装备赋予的主动 Gameplay Ability");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		Ability_Type_EquippedItem_Swing,
		"Ability.Type.EquippedItem.Swing",
		"手持物挥动 Gameplay Ability");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		Ability_Type_EquippedItem_Fireball,
		"Ability.Type.EquippedItem.Fireball",
		"火焰球投掷 Gameplay Ability");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		Ability_Type_EquippedItem_MeteorStrike,
		"Ability.Type.EquippedItem.MeteorStrike",
		"陨石打击 Gameplay Ability");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		Ability_Type_EquippedItem_DemolitionTool,
		"Ability.Type.EquippedItem.DemolitionTool",
		"拆除锤挥击表现 Gameplay Ability");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		Ability_State_UsingEquippedItem,
		"Ability.State.UsingEquippedItem",
		"装备主要行为正在执行");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(
		State_Burning,
		"State.Burning",
		"角色正在受到 Burning GameplayEffect");
}
