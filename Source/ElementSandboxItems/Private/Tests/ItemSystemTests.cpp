#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Equipment/EquipmentComponent.h"
#include "Equipment/EquippedItemActor.h"
#include "Inventory/InventoryAdditionReceipt.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryConsumptionReceipt.h"
#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Tests/ItemDefinitionTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"

namespace ElementSandbox::Items::Tests
{
	UItemDefinition* MakeDefinition(UObject* Outer, const int32 MaxStackSize = 1, const bool bEquippable = false)
	{
		UItemDefinition* Definition = NewObject<UItemDefinition>(Outer);
		UItemDisplayFeature* Display = NewObject<UItemDisplayFeature>(Definition);
		Display->DisplayName = FText::FromString(TEXT("Test Item"));
		Definition->FeatureTemplates.Add(Display);

		UItemStackFeature* Stack = NewObject<UItemStackFeature>(Definition);
		Stack->MaxStackSize = MaxStackSize;
		Definition->FeatureTemplates.Add(Stack);

		if (bEquippable)
		{
			UEquippableItemFeature* Equippable = NewObject<UEquippableItemFeature>(Definition);
			Equippable->EquippedActorClass = AEquippedItemActor::StaticClass();
			Equippable->AttachmentSocket = NAME_None;
			Definition->FeatureTemplates.Add(Equippable);
		}
		return Definition;
	}

	struct FInventoryTestWorld
	{
		FInventoryTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Owner = World->SpawnActor<APlayerState>();
			Pawn = World->SpawnActor<APawn>();
			Pawn->SetPlayerState(Owner);
			Inventory = NewObject<UInventoryComponent>(Owner, TEXT("TestInventory"));
			Owner->AddInstanceComponent(Inventory);
			Inventory->RegisterComponent();
		}

		~FInventoryTestWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* World = nullptr;
		APlayerState* Owner = nullptr;
		APawn* Pawn = nullptr;
		UInventoryComponent* Inventory = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInventoryDefinitionInterfaceTest,
	"ElementSandbox.Items.DefinitionInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInventoryDefinitionInterfaceTest::RunTest(const FString& Parameters)
{
	UTestInventoryItemDefinition* Definition = NewObject<UTestInventoryItemDefinition>();
	UItemStackFeature* StackTemplate = NewObject<UItemStackFeature>(Definition);
	StackTemplate->MaxStackSize = 8;
	Definition->FeatureTemplates.Add(StackTemplate);

	UItemInstance* Item = NewObject<UItemInstance>();
	TestTrue(TEXT("任意实现定义接口的 UObject 都能创建 ItemInstance"), Item->Initialize(Definition));
	TestTrue(TEXT("ItemInstance 保留原始定义对象身份"), Item->GetDefinition().GetObject() == Definition);
	const UItemStackFeature* RuntimeStack = Item->FindFeature<UItemStackFeature>();
	TestNotNull(TEXT("接口提供的 Feature 模板被复制"), RuntimeStack);
	TestTrue(TEXT("运行时 Feature 不复用接口对象中的模板"), RuntimeStack != StackTemplate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FItemFeatureIsolationTest,
	"ElementSandbox.Items.FeatureIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FItemFeatureIsolationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Items::Tests;
	UItemDefinition* Definition = MakeDefinition(GetTransientPackage(), 10);
	UItemInstance* FirstItem = NewObject<UItemInstance>();
	UItemInstance* SecondItem = NewObject<UItemInstance>();

	TestTrue(TEXT("第一个实例初始化成功"), FirstItem->Initialize(Definition));
	TestTrue(TEXT("第二个实例初始化成功"), SecondItem->Initialize(Definition));
	TestNotNull(TEXT("运行时可查询 DisplayFeature"), FirstItem->FindFeature<UItemDisplayFeature>());
	TestTrue(TEXT("两个实例的 StackFeature 彼此独立"),
		FirstItem->FindFeature<UItemStackFeature>() != SecondItem->FindFeature<UItemStackFeature>());
	TestTrue(TEXT("运行时 Feature 不复用 Definition 模板"),
		FirstItem->FindFeature<UItemStackFeature>() != Definition->FindFeatureTemplate<UItemStackFeature>());
	TestFalse(TEXT("运行时 Feature 必须作为动态网络子对象创建"),
		FirstItem->FindFeature<UItemDisplayFeature>()->IsNameStableForNetworking());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInventorySlotsAndStackTest,
	"ElementSandbox.Items.InventorySlotsAndStack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInventorySlotsAndStackTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Items::Tests;
	FInventoryTestWorld Harness;
	UItemDefinition* StackableDefinition = MakeDefinition(Harness.World, 10);
	UItemDefinition* OtherDefinition = MakeDefinition(Harness.World, 1);

	TestEqual(TEXT("快捷栏固定为 10 格"), Harness.Inventory->GetQuickbarSlots().Num(), 10);
	TestEqual(TEXT("背包固定为 30 格"), Harness.Inventory->GetBackpackSlots().Num(), 30);
	TestEqual(TEXT("12 个道具全部加入"), Harness.Inventory->AddItem(StackableDefinition, 12), 12);

	const UItemInstance* FirstStack = Harness.Inventory->GetItem(
		FInventorySlotAddress(EInventoryContainer::Backpack, 0));
	const UItemInstance* SecondStack = Harness.Inventory->GetItem(
		FInventorySlotAddress(EInventoryContainer::Backpack, 1));
	TestEqual(TEXT("第一堆达到上限"), FirstStack->FindFeature<UItemStackFeature>()->GetQuantity(), 10);
	TestEqual(TEXT("剩余数量进入第二个固定槽位"), SecondStack->FindFeature<UItemStackFeature>()->GetQuantity(), 2);

	TestTrue(TEXT("同 Definition 移动时优先合并堆叠"), Harness.Inventory->MoveItem(
		FInventorySlotAddress(EInventoryContainer::Backpack, 0),
		FInventorySlotAddress(EInventoryContainer::Backpack, 1)));
	TestEqual(TEXT("目标堆叠达到上限"),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 1))
			->FindFeature<UItemStackFeature>()->GetQuantity(), 10);
	TestEqual(TEXT("未合并完的数量留在源槽"),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 0))
			->FindFeature<UItemStackFeature>()->GetQuantity(), 2);

	TestTrue(TEXT("不同 Definition 可交换槽位"), Harness.Inventory->GrantItemToQuickbar(OtherDefinition, 0));
	TestTrue(TEXT("快捷栏与背包可以交换"), Harness.Inventory->MoveItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, 0),
		FInventorySlotAddress(EInventoryContainer::Backpack, 2)));
	TestTrue(TEXT("交换后 Definition 正确"),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 2))
			->GetDefinition().GetObject() == OtherDefinition);
	TestFalse(TEXT("非法快捷栏索引被拒绝"), Harness.Inventory->MoveItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, 10),
		FInventorySlotAddress(EInventoryContainer::Backpack, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInventoryQuantityTransactionTest,
	"ElementSandbox.Items.InventoryQuantityTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInventoryQuantityTransactionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Items::Tests;
	FInventoryTestWorld Harness;
	UItemDefinition* Definition = MakeDefinition(Harness.World, 20);
	const FInventorySlotAddress Address(EInventoryContainer::Quickbar, 0);
	TestTrue(TEXT("向指定快捷栏发放十个测试物品"),
		Harness.Inventory->GrantItemToQuickbar(Definition, 0, 10));
	Harness.Inventory->SelectQuickbarSlot(0);
	UItemInstance* OriginalItem = Harness.Inventory->GetItem(Address);
	TestNotNull(TEXT("取得原始 Stack"), OriginalItem);

	FInventoryConsumptionReceipt PartialRollback;
	TestTrue(TEXT("开启部分数量扣除"),
		Harness.Inventory->BeginConsumeItemQuantity(Address, 3, PartialRollback));
	TestEqual(TEXT("事务中数量立即减少"),
		OriginalItem->FindFeature<UItemStackFeature>()->GetQuantity(), 7);
	TestTrue(TEXT("部分扣除可以完整回滚"),
		Harness.Inventory->RollbackItemConsumption(PartialRollback));
	TestEqual(TEXT("回滚恢复原数量"),
		OriginalItem->FindFeature<UItemStackFeature>()->GetQuantity(), 10);

	FInventoryConsumptionReceipt PartialCommit;
	TestTrue(TEXT("再次开启部分扣除"),
		Harness.Inventory->BeginConsumeItemQuantity(Address, 3, PartialCommit));
	TestTrue(TEXT("部分扣除提交"),
		Harness.Inventory->CommitItemConsumption(PartialCommit));
	TestEqual(TEXT("提交后保留剩余七个"),
		OriginalItem->FindFeature<UItemStackFeature>()->GetQuantity(), 7);

	FInventoryConsumptionReceipt LastStackRollback;
	TestTrue(TEXT("扣除最后七个"),
		Harness.Inventory->BeginConsumeItemQuantity(Address, 7, LastStackRollback));
	TestNull(TEXT("最后一个数量被扣除时槽位临时清空"),
		Harness.Inventory->GetItem(Address));
	TestEqual(TEXT("临时移除会清除选中槽"),
		Harness.Inventory->GetSelectedQuickbarIndex(), INDEX_NONE);
	TestTrue(TEXT("整 Stack 回滚"),
		Harness.Inventory->RollbackItemConsumption(LastStackRollback));
	TestEqual(TEXT("整 Stack 回滚恢复同一实例"),
		Harness.Inventory->GetItem(Address), OriginalItem);
	TestEqual(TEXT("整 Stack 回滚恢复选中槽"),
		Harness.Inventory->GetSelectedQuickbarIndex(), 0);

	FInventoryConsumptionReceipt LastStackCommit;
	TestTrue(TEXT("再次扣除最后七个"),
		Harness.Inventory->BeginConsumeItemQuantity(Address, 7, LastStackCommit));
	TestTrue(TEXT("最后一个数量提交"),
		Harness.Inventory->CommitItemConsumption(LastStackCommit));
	TestNull(TEXT("提交后槽位保持为空"), Harness.Inventory->GetItem(Address));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInventoryAdditionTransactionTest,
	"ElementSandbox.Items.InventoryAdditionTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInventoryAdditionTransactionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Items::Tests;
	FInventoryTestWorld Harness;
	UItemDefinition* Definition = MakeDefinition(Harness.World, 10);
	TestEqual(TEXT("先加入八个测试物品"), Harness.Inventory->AddItem(Definition, 8), 8);
	UItemInstance* OriginalStack = Harness.Inventory->GetItem(
		FInventorySlotAddress(EInventoryContainer::Backpack, 0));
	TestNotNull(TEXT("取得原始背包堆叠"), OriginalStack);

	FInventoryAdditionReceipt RollbackReceipt;
	int32 AddedQuantity = 0;
	TestTrue(TEXT("开启跨旧堆叠和新槽位的增加事务"),
		Harness.Inventory->BeginAddItem(
			Definition,
			5,
			EInventoryContainer::Backpack,
			RollbackReceipt,
			AddedQuantity));
	TestEqual(TEXT("事务完整加入五个"), AddedQuantity, 5);
	TestEqual(TEXT("旧堆叠在事务中达到上限"),
		OriginalStack->FindFeature<UItemStackFeature>()->GetQuantity(), 10);
	TestNotNull(TEXT("余量在事务中新建第二堆"),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 1)));
	TestEqual(TEXT("增加事务可只读取得唯一新建实例供提交前配置"),
		RollbackReceipt.GetSingleCreatedItem(),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 1)));
	TestTrue(TEXT("增加事务可以完整回滚"),
		Harness.Inventory->RollbackItemAddition(RollbackReceipt));
	TestNull(TEXT("事务回滚后不再暴露新建实例"),
		RollbackReceipt.GetSingleCreatedItem());
	TestEqual(TEXT("回滚恢复原堆叠数量"),
		OriginalStack->FindFeature<UItemStackFeature>()->GetQuantity(), 8);
	TestNull(TEXT("回滚移除事务中新建的槽位"),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 1)));

	FInventoryAdditionReceipt CommitReceipt;
	AddedQuantity = 0;
	TestTrue(TEXT("再次开启同一增加事务"),
		Harness.Inventory->BeginAddItem(
			Definition,
			5,
			EInventoryContainer::Backpack,
			CommitReceipt,
			AddedQuantity));
	TestTrue(TEXT("增加事务提交"),
		Harness.Inventory->CommitItemAddition(CommitReceipt));
	TestEqual(TEXT("提交后第一堆保持十个"),
		OriginalStack->FindFeature<UItemStackFeature>()->GetQuantity(), 10);
	TestEqual(TEXT("提交后第二堆保持三个"),
		Harness.Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Backpack, 1))
			->FindFeature<UItemStackFeature>()->GetQuantity(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FItemSelectionAndEquipmentTest,
	"ElementSandbox.Items.SelectionAndEquipment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FItemSelectionAndEquipmentTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Items::Tests;
	FInventoryTestWorld Harness;
	ACharacter* Character = Harness.World->SpawnActor<ACharacter>();
	UEquipmentComponent* Equipment = NewObject<UEquipmentComponent>(Character, TEXT("TestEquipment"));
	Character->AddInstanceComponent(Equipment);
	Equipment->RegisterComponent();

	UItemDefinition* Definition = MakeDefinition(Harness.World, 1, true);
	UItemInstance* Item = NewObject<UItemInstance>();
	TestTrue(TEXT("可装备道具初始化成功"), Item->Initialize(Definition));
	const UEquippableItemFeature* Equippable = Item->FindFeature<UEquippableItemFeature>();
	TestNotNull(TEXT("可查询装备配置"), Equippable);
	TestTrue(TEXT("装备系统生成 Actor 投影"), Equipment->EquipItem(Item, *Equippable));
	AEquippedItemActor* FirstProjection = Equipment->GetEquippedActor();
	TestNotNull(TEXT("装备生成世界投影"), FirstProjection);
	TestTrue(TEXT("重复装备保持幂等"), Equipment->EquipItem(Item, *Equippable));
	TestEqual(TEXT("没有重复 Spawn"), Equipment->GetEquippedActor(), FirstProjection);
	AEquippedItemActor* ReleasedProjection = nullptr;
	TestTrue(TEXT("服务器可把装备 Actor 交给上层而不销毁"),
		Equipment->ReleaseEquippedActor(Item, ReleasedProjection));
	TestEqual(TEXT("Release 返回同一 Actor"), ReleasedProjection, FirstProjection);
	TestTrue(TEXT("Release 后 Actor 仍存活"), IsValid(ReleasedProjection));
	TestNull(TEXT("Release 清空装备组件中的 Actor 引用"), Equipment->GetEquippedActor());
	TestTrue(TEXT("Release 后可重新装备"), Equipment->EquipItem(Item, *Equippable));
	TestTrue(TEXT("服务器可卸下装备"), Equipment->UnequipItem());
	TestNull(TEXT("卸下后清空投影指针"), Equipment->GetEquippedActor());
	return true;
}

#endif
