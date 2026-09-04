#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Abilities/DemolitionToolSwingGameplayAbility.h"
#include "Abilities/FireballGameplayAbility.h"
#include "Abilities/AxeSwingGameplayAbility.h"
#include "Abilities/StickSwingGameplayAbility.h"
#include "Building/BuildingItemFeature.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Components/StaticMeshComponent.h"
#include "Game/ElementSandboxGameMode.h"
#include "Items/AxeEquippedItemActor.h"
#include "Items/AxeItemDefinition.h"
#include "Items/AxeSwingAbilityFeature.h"
#include "Items/DemolitionToolEquippedItemActor.h"
#include "Items/DemolitionToolItemDefinition.h"
#include "Items/DemolitionToolItemFeature.h"
#include "Items/DemolitionToolSwingAbilityFeature.h"
#include "Items/FireballAbilityFeature.h"
#include "Items/FireballEquippedItemActor.h"
#include "Items/FireballItemDefinition.h"
#include "Items/MeteorStrikeItemDefinition.h"
#include "Items/ReclaimedBuildingItemDefinition.h"
#include "Items/StickEquippedItemActor.h"
#include "Items/StickSwingAbilityFeature.h"
#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Tags/ElementGameplayTags.h"
#include "Torch/TorchDefinition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProjectItemAssetsTest,
	"ElementSandbox.Project.ItemAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectItemAssetsTest::RunTest(const FString& Parameters)
{
	const UItemDefinition* Definition = LoadObject<UItemDefinition>(nullptr, TEXT("/Game/Items/DA_Stick.DA_Stick"));
	TestNotNull(TEXT("DA_Stick 可以加载"), Definition);
	if (!Definition)
	{
		return false;
	}

	const UItemDisplayFeature* Display = Definition->FindFeatureTemplate<UItemDisplayFeature>();
	const UItemStackFeature* Stack = Definition->FindFeatureTemplate<UItemStackFeature>();
	const UEquippableItemFeature* Equippable = Definition->FindFeatureTemplate<UEquippableItemFeature>();
	const UStickSwingAbilityFeature* Swing = Definition->FindFeatureTemplate<UStickSwingAbilityFeature>();
	TestNotNull(TEXT("包含展示 Feature"), Display);
	TestNotNull(TEXT("包含堆叠 Feature"), Stack);
	TestNotNull(TEXT("包含装备 Feature"), Equippable);
	TestNotNull(TEXT("包含木棍挥动 Ability Feature"), Swing);
	if (Display)
	{
		TestEqual(TEXT("当前展示名称是木棍"), Display->DisplayName.ToString(), FString(TEXT("木棍")));
	}
	if (Stack)
	{
		TestEqual(TEXT("木棍最大堆叠为 1"), Stack->GetMaxStackSize(), 1);
	}
	if (Equippable)
	{
		TestEqual(TEXT("装备投影使用纯 C++ 木棍 Actor"),
			Equippable->EquippedActorClass.Get(), AStickEquippedItemActor::StaticClass());
		TestEqual(TEXT("默认挂到 hand_r"), Equippable->AttachmentSocket, FName(TEXT("hand_r")));
	}
	if (Swing)
	{
		TestEqual(TEXT("木棍只授予一个主要行为"), Swing->GetAbilitySet().Abilities.Num(), 1);
		if (Swing->GetAbilitySet().Abilities.Num() == 1)
		{
			const FElementAbilityGrant& Grant = Swing->GetAbilitySet().Abilities[0];
			TestEqual(TEXT("授予本地预测挥动 Ability"), Grant.Ability.Get(), UStickSwingGameplayAbility::StaticClass());
			TestEqual(TEXT("使用统一主要输入 Tag"), Grant.InputTag, ElementSandboxGameplayTags::Input_Use_Primary.GetTag());
		}
	}

	const UFireballItemDefinition* Fireball =
		GetDefault<UFireballItemDefinition>();
	TestNotNull(TEXT("存在纯 C++ 火焰球 Definition"), Fireball);
	if (Fireball)
	{
		const UItemDisplayFeature* FireballDisplay =
			Fireball->FindFeatureTemplate<UItemDisplayFeature>();
		const UItemStackFeature* FireballStack =
			Fireball->FindFeatureTemplate<UItemStackFeature>();
		const UEquippableItemFeature* FireballEquippable =
			Fireball->FindFeatureTemplate<UEquippableItemFeature>();
		const UFireballAbilityFeature* FireballAbility =
			Fireball->FindFeatureTemplate<UFireballAbilityFeature>();
		TestEqual(TEXT("火焰球由展示、堆叠、装备和 GAS 四个 Feature 组成"),
			Fireball->FeatureTemplates.Num(), 4);
		TestTrue(TEXT("原生 CDO 是可联网解析的稳定 Definition"),
			Fireball->IsNameStableForNetworking());
		TestTrue(TEXT("火焰球展示名正确"),
			FireballDisplay
			&& FireballDisplay->DisplayName.ToString() == TEXT("火焰球"));
		TestTrue(TEXT("火焰球单堆可以容纳默认 999 个"),
			FireballStack && FireballStack->GetMaxStackSize() == 999);
		TestTrue(TEXT("火焰球装备使用独立 C++ 表现 Actor"),
			FireballEquippable
			&& FireballEquippable->EquippedActorClass.Get()
				== AFireballEquippedItemActor::StaticClass());
			TestTrue(TEXT("火焰球只授予一个主要投掷 Ability"),
				FireballAbility
				&& FireballAbility->GetAbilitySet().Abilities.Num() == 1
				&& FireballAbility->GetAbilitySet().Abilities[0].Ability.Get()
					== UFireballGameplayAbility::StaticClass()
				&& FireballAbility->GetAbilitySet().Abilities[0].InputTag
					== ElementSandboxGameplayTags::Input_Use_Primary.GetTag());

			UItemInstance* RuntimeFireball = NewObject<UItemInstance>();
			TestTrue(TEXT("原生火焰球 Definition 可以创建运行期 ItemInstance"),
				RuntimeFireball->Initialize(const_cast<UFireballItemDefinition*>(Fireball)));
			for (const UItemFeature* RuntimeFeature : RuntimeFireball->GetFeatures())
			{
				TestTrue(TEXT("运行期火焰球 Feature 归属于 ItemInstance"),
					RuntimeFeature && RuntimeFeature->GetOuter() == RuntimeFireball);
				TestFalse(TEXT("运行期火焰球 Feature 不携带 CDO/Archetype 模板标记"),
					RuntimeFeature && RuntimeFeature->IsTemplate());
			}
		}

	const UMeteorStrikeItemDefinition* Meteor = GetDefault<UMeteorStrikeItemDefinition>();
	const UItemStackFeature* MeteorStack = Meteor
		? Meteor->FindFeatureTemplate<UItemStackFeature>() : nullptr;
	TestTrue(TEXT("陨石核心不可堆叠且默认只发一枚"),
		Meteor && MeteorStack && MeteorStack->GetMaxStackSize() == 1
		&& AElementSandboxGameMode::DefaultMeteorQuantity == 1);

	const UAxeItemDefinition* Axe = GetDefault<UAxeItemDefinition>();
	TestNotNull(TEXT("存在纯 C++ 斧头 Definition"), Axe);
	if (Axe)
	{
		const UItemDisplayFeature* AxeDisplay =
			Axe->FindFeatureTemplate<UItemDisplayFeature>();
		const UItemStackFeature* AxeStack =
			Axe->FindFeatureTemplate<UItemStackFeature>();
		const UEquippableItemFeature* AxeEquippable =
			Axe->FindFeatureTemplate<UEquippableItemFeature>();
		const UAxeSwingAbilityFeature* AxeSwing =
			Axe->FindFeatureTemplate<UAxeSwingAbilityFeature>();
		TestEqual(TEXT("斧头只配置展示、单件堆叠、装备和 GAS 挥击四个 Feature"),
			Axe->FeatureTemplates.Num(), 4);
		TestTrue(TEXT("斧头原生 CDO 可作为稳定联网 Definition"),
			Axe->IsNameStableForNetworking());
		TestTrue(TEXT("斧头展示名正确"),
			AxeDisplay && AxeDisplay->DisplayName.ToString() == TEXT("斧头"));
		TestTrue(TEXT("斧头不可堆叠"),
			AxeStack && AxeStack->GetMaxStackSize() == 1);
		TestTrue(TEXT("斧头使用独立 C++ 手持表现"),
			AxeEquippable
			&& AxeEquippable->EquippedActorClass.Get()
				== AAxeEquippedItemActor::StaticClass());
		AAxeEquippedItemActor* AxeActor = GetMutableDefault<AAxeEquippedItemActor>();
		const UStaticMeshComponent* AxeBlade = AxeActor
			? Cast<UStaticMeshComponent>(AxeActor->GetDefaultSubobjectByName(TEXT("AxeBlade")))
			: nullptr;
		TestTrue(TEXT("斧刃绕握柄翻向背侧但不颠倒整把斧头"),
			AxeBlade
			&& FMath::IsNearlyEqual(AxeBlade->GetRelativeLocation().X, -15.0)
			&& AxeBlade->GetRelativeLocation().Z > 0.0);
		TestTrue(TEXT("斧头握柄从 hand_r 掌心向下延伸"),
			AxeEquippable
			&& AxeEquippable->AttachmentTransform.GetRotation().Equals(
				FQuat(FRotator(90.0, 0.0, 0.0))));
		TestTrue(TEXT("斧头主输入授予独立服务器权威破坏 Ability"),
			AxeSwing
				&& AxeSwing->GetAbilitySet().Abilities.Num() == 1
				&& AxeSwing->GetAbilitySet().Abilities[0].Ability.Get()
					== UAxeSwingGameplayAbility::StaticClass()
			&& AxeSwing->GetAbilitySet().Abilities[0].InputTag
				== ElementSandboxGameplayTags::Input_Use_Primary.GetTag());
		TestNull(TEXT("斧头当前没有拆除、伤害或采集 Feature"),
			Axe->FindFeatureTemplate<UDemolitionToolItemFeature>());
	}

	const UDemolitionToolItemDefinition* DemolitionTool =
		GetDefault<UDemolitionToolItemDefinition>();
	TestNotNull(TEXT("存在纯 C++ 拆除锤 Definition"), DemolitionTool);
	if (DemolitionTool)
	{
		const UItemDisplayFeature* DemolitionDisplay =
			DemolitionTool->FindFeatureTemplate<UItemDisplayFeature>();
		const UItemStackFeature* DemolitionStack =
			DemolitionTool->FindFeatureTemplate<UItemStackFeature>();
		const UEquippableItemFeature* DemolitionEquippable =
			DemolitionTool->FindFeatureTemplate<UEquippableItemFeature>();
		const UDemolitionToolItemFeature* Dismantle =
			DemolitionTool->FindFeatureTemplate<UDemolitionToolItemFeature>();
		const UDemolitionToolSwingAbilityFeature* DemolitionSwing =
			DemolitionTool->FindFeatureTemplate<UDemolitionToolSwingAbilityFeature>();
		TestEqual(TEXT("拆除锤由展示、单件堆叠、装备、独立挥击 Ability 和拆除装配五个 Feature 组成"),
			DemolitionTool->FeatureTemplates.Num(), 5);
		TestTrue(TEXT("拆除锤展示名正确"),
			DemolitionDisplay
			&& DemolitionDisplay->DisplayName.ToString() == TEXT("拆除锤"));
		TestTrue(TEXT("拆除锤不可堆叠"),
			DemolitionStack && DemolitionStack->GetMaxStackSize() == 1);
		TestTrue(TEXT("拆除锤使用独立 C++ 手持表现"),
			DemolitionEquippable
			&& DemolitionEquippable->EquippedActorClass.Get()
				== ADemolitionToolEquippedItemActor::StaticClass());
		TestTrue(TEXT("拆除锤握柄从 hand_r 掌心向下延伸"),
			DemolitionEquippable
			&& DemolitionEquippable->AttachmentTransform.GetRotation().Equals(
				FQuat(FRotator(90.0, 0.0, 0.0))));
		TestTrue(TEXT("拆除锤只授予自己的主要挥击 Ability"),
			DemolitionSwing
			&& DemolitionSwing->GetAbilitySet().Abilities.Num() == 1
			&& DemolitionSwing->GetAbilitySet().Abilities[0].Ability.Get()
				== UDemolitionToolSwingGameplayAbility::StaticClass()
			&& DemolitionSwing->GetAbilitySet().Abilities[0].InputTag
				== ElementSandboxGameplayTags::Input_Use_Primary.GetTag());
		TestTrue(TEXT("拆除锤覆盖标准建造件与全部城镇种子 Building"),
			Dismantle && Dismantle->Rewards.Num() == 18);
		for (const FName DefinitionId : {FName(TEXT("WoodWall")), FName(TEXT("WoodFloor")), FName(TEXT("WoodPillar"))})
		{
			UItemDefinition* RewardItem = nullptr;
			int32 RewardQuantity = 0;
			TestTrue(*FString::Printf(TEXT("拆除锤可解析 %s 的返还物品"), *DefinitionId.ToString()),
				Dismantle
				&& Dismantle->TryResolveReward(DefinitionId, RewardItem, RewardQuantity));
			TestTrue(TEXT("每次拆除返还一个有效建造物品"),
				IsValid(RewardItem) && RewardQuantity == 1);
		}
			const FName CityPieceId = GetCityBuildingPieceDefinitionId(
				ECityBuildingPieceKind::SolidBox,
				TEXT("Surface.City.Wall"));
			UItemDefinition* CityReward = nullptr;
			int32 CityRewardQuantity = 0;
			TestTrue(TEXT("AI 城镇 primitive 使用可保形的回收构件返还"),
				Dismantle
				&& Dismantle->TryResolveReward(
					CityPieceId,
					CityReward,
					CityRewardQuantity)
				&& CityReward == GetDefault<UReclaimedBuildingItemDefinition>()
				&& CityRewardQuantity == 1
				&& Dismantle->FindReward(CityPieceId)->bPreservePlacementShape);
			TestTrue(TEXT("聚落门和挂墙火把同样属于可回收的预置玩家建筑"),
				Dismantle
				&& Dismantle->FindReward(TEXT("Settlement.Door"))
				&& Dismantle->FindReward(GetMountedTorchBuildingDefinitionId()));
			UItemDefinition* UnsupportedReward = nullptr;
			int32 UnsupportedQuantity = 0;
			TestFalse(TEXT("FirePile 不作为房屋构件返还"),
				Dismantle
				&& Dismantle->TryResolveReward(
					TEXT("FirePile"), UnsupportedReward, UnsupportedQuantity));
		}

		const UReclaimedBuildingItemDefinition* ReclaimedBuilding =
			GetDefault<UReclaimedBuildingItemDefinition>();
		const UBuildingItemFeature* ReclaimedBuildingFeature = ReclaimedBuilding
			? ReclaimedBuilding->FindFeatureTemplate<UBuildingItemFeature>()
			: nullptr;
		TestTrue(TEXT("回收构件 Definition 是不可堆叠的动态 Building 载体"),
			ReclaimedBuilding
			&& ReclaimedBuilding->FindFeatureTemplate<UItemStackFeature>()
			&& ReclaimedBuilding->FindFeatureTemplate<UItemStackFeature>()->GetMaxStackSize() == 1
			&& ReclaimedBuildingFeature
			&& ReclaimedBuildingFeature->GetBuildingDefinitionId().IsNone()
			&& ReclaimedBuildingFeature->GetPlacementShapeTransform().Equals(
				FTransform::Identity));

	const UItemDefinition* Charcoal = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Charcoal.DA_Charcoal"));
	TestNotNull(TEXT("DA_Charcoal 可以加载"), Charcoal);
	if (Charcoal)
	{
		const UItemDisplayFeature* CharcoalDisplay =
			Charcoal->FindFeatureTemplate<UItemDisplayFeature>();
		const UItemStackFeature* CharcoalStack =
			Charcoal->FindFeatureTemplate<UItemStackFeature>();
		TestEqual(TEXT("木炭只配置展示与堆叠两个 Feature"),
			Charcoal->FeatureTemplates.Num(), 2);
		TestNotNull(TEXT("木炭包含展示 Feature"), CharcoalDisplay);
		TestNotNull(TEXT("木炭包含堆叠 Feature"), CharcoalStack);
		TestNull(TEXT("木炭本轮没有装备能力"),
			Charcoal->FindFeatureTemplate<UEquippableItemFeature>());
		if (CharcoalDisplay)
		{
			TestEqual(TEXT("木炭展示名正确"),
				CharcoalDisplay->DisplayName.ToString(), FString(TEXT("木炭")));
		}
		if (CharcoalStack)
		{
			TestEqual(TEXT("木炭最大堆叠为 20"),
				CharcoalStack->GetMaxStackSize(), 20);
		}
	}

	struct FBuildingItemExpectation
	{
		const TCHAR* AssetPath;
		const TCHAR* DisplayName;
		const TCHAR* DefinitionId;
	};
	const FBuildingItemExpectation BuildingItems[] =
	{
		{TEXT("/Game/Items/DA_WoodWall.DA_WoodWall"), TEXT("木墙"), TEXT("WoodWall")},
		{TEXT("/Game/Items/DA_WoodFloor.DA_WoodFloor"), TEXT("木地板"), TEXT("WoodFloor")},
		{TEXT("/Game/Items/DA_WoodPillar.DA_WoodPillar"), TEXT("木柱"), TEXT("WoodPillar")}
	};
	for (const FBuildingItemExpectation& Expected : BuildingItems)
	{
		const UItemDefinition* BuildingItem = LoadObject<UItemDefinition>(
			nullptr,
			Expected.AssetPath);
		TestNotNull(*FString::Printf(TEXT("%s 可以加载"), Expected.AssetPath), BuildingItem);
		if (!BuildingItem)
		{
			continue;
		}
		const UItemDisplayFeature* BuildingDisplay =
			BuildingItem->FindFeatureTemplate<UItemDisplayFeature>();
		const UItemStackFeature* BuildingStack =
			BuildingItem->FindFeatureTemplate<UItemStackFeature>();
		const UBuildingItemFeature* BuildingFeature =
			BuildingItem->FindFeatureTemplate<UBuildingItemFeature>();
		TestEqual(TEXT("建造物品只配置展示、堆叠和 Building 桥接"),
			BuildingItem->FeatureTemplates.Num(), 3);
		TestNotNull(TEXT("建造物品包含展示 Feature"), BuildingDisplay);
		TestNotNull(TEXT("建造物品包含堆叠 Feature"), BuildingStack);
		TestNotNull(TEXT("建造物品包含 Building Feature"), BuildingFeature);
		if (BuildingDisplay)
		{
			TestEqual(TEXT("建造物品展示名正确"),
				BuildingDisplay->DisplayName.ToString(), FString(Expected.DisplayName));
		}
		if (BuildingStack)
		{
			TestEqual(TEXT("建造物品最大堆叠为 20"),
				BuildingStack->GetMaxStackSize(), 20);
		}
		if (BuildingFeature)
		{
			TestEqual(TEXT("建造物品解析稳定 DefinitionId"),
				BuildingFeature->GetBuildingDefinitionId(), FName(Expected.DefinitionId));
			TestTrue(TEXT("普通建造物品使用 Identity 摆放形态"),
				BuildingFeature->GetPlacementShapeTransform().Equals(
					FTransform::Identity));
		}
	}

	const UInputMappingContext* MappingContext = LoadObject<UInputMappingContext>(
		nullptr, TEXT("/Game/Input/Items/IMC_Items.IMC_Items"));
	TestNotNull(TEXT("IMC_Items 可以加载"), MappingContext);
	if (MappingContext)
	{
		TestEqual(TEXT("包含 10 个快捷栏、1 个背包和 1 个 Ability 输入映射"), MappingContext->GetMappings().Num(), 12);

		const UInputAction* UseAction = LoadObject<UInputAction>(
			nullptr, TEXT("/Game/Input/Items/Actions/IA_UseEquippedItem.IA_UseEquippedItem"));
		TestNotNull(TEXT("IA_UseEquippedItem 可以加载"), UseAction);
		const bool bHasLeftMouseUse = UseAction && MappingContext->GetMappings().ContainsByPredicate(
			[UseAction](const FEnhancedActionKeyMapping& Mapping)
			{
				return Mapping.Action == UseAction && Mapping.Key == EKeys::LeftMouseButton;
			});
		TestTrue(TEXT("主要 Use 映射到鼠标左键"), bHasLeftMouseUse);
	}

	const UInputMappingContext* BuildingMapping = LoadObject<UInputMappingContext>(
		nullptr,
		TEXT("/Game/Input/Building/IMC_Building.IMC_Building"));
	const UInputAction* ConfirmAction = LoadObject<UInputAction>(
		nullptr,
		TEXT("/Game/Input/Building/Actions/IA_BuildConfirm.IA_BuildConfirm"));
	const UInputAction* CancelAction = LoadObject<UInputAction>(
		nullptr,
		TEXT("/Game/Input/Building/Actions/IA_BuildCancel.IA_BuildCancel"));
	const UInputAction* RotateAction = LoadObject<UInputAction>(
		nullptr,
		TEXT("/Game/Input/Building/Actions/IA_BuildRotate.IA_BuildRotate"));
	TestNotNull(TEXT("建造独立 Input Mapping 可以加载"), BuildingMapping);
	TestNotNull(TEXT("建造确认 Action 可以加载"), ConfirmAction);
	TestNotNull(TEXT("建造取消 Action 可以加载"), CancelAction);
	TestNotNull(TEXT("建造旋转 Action 可以加载"), RotateAction);
	if (BuildingMapping)
	{
		TestEqual(TEXT("建造 Mapping 只包含确认、取消和旋转"),
			BuildingMapping->GetMappings().Num(), 3);
		TestTrue(TEXT("左键确认摆放"), BuildingMapping->GetMappings().ContainsByPredicate(
			[ConfirmAction](const FEnhancedActionKeyMapping& Mapping)
			{
				return Mapping.Action == ConfirmAction
					&& Mapping.Key == EKeys::LeftMouseButton;
			}));
		TestTrue(TEXT("右键取消建造"), BuildingMapping->GetMappings().ContainsByPredicate(
			[CancelAction](const FEnhancedActionKeyMapping& Mapping)
			{
				return Mapping.Action == CancelAction
					&& Mapping.Key == EKeys::RightMouseButton;
			}));
		TestTrue(TEXT("滚轮按 90 度旋转"), BuildingMapping->GetMappings().ContainsByPredicate(
			[RotateAction](const FEnhancedActionKeyMapping& Mapping)
			{
				return Mapping.Action == RotateAction
					&& Mapping.Key == EKeys::MouseWheelAxis;
			}));
	}
	TestNotNull(TEXT("合法蓝色预览材质可以加载"),
		LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/Building/Materials/MI_BuildPreviewValid.MI_BuildPreviewValid")));
	TestNotNull(TEXT("非法红色预览材质可以加载"),
		LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/Building/Materials/MI_BuildPreviewInvalid.MI_BuildPreviewInvalid")));
	return true;
}

#endif
