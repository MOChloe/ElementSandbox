#include "Inventory/InventoryComponent.h"

#include "Inventory/InventoryAdditionReceipt.h"
#include "Inventory/InventoryConsumptionReceipt.h"

#include "Equipment/EquipmentComponent.h"
#include "Equipment/EquippedItemActor.h"
#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemFeature.h"
#include "Item/ItemInstance.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"

UItemInstance* FInventoryAdditionReceipt::GetSingleCreatedItem() const
{
	if (!bActive)
	{
		return nullptr;
	}

	UItemInstance* Result = nullptr;
	for (const FChange& Change : Changes)
	{
		if (!Change.bCreatedItem)
		{
			continue;
		}
		if (Result)
		{
			return nullptr;
		}
		Result = Change.ItemInstance.Get();
	}
	return Result;
}

UInventoryComponent::UInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	bReplicateUsingRegisteredSubObjectList = true;

	QuickbarSlots.SetNum(QuickbarSlotCount);
	BackpackSlots.SetNum(BackpackSlotCount);
}

void UInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UInventoryComponent, QuickbarSlots, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UInventoryComponent, BackpackSlots, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UInventoryComponent, QuickbarSelection, COND_OwnerOnly);
}

void UInventoryComponent::ReadyForReplication()
{
	Super::ReadyForReplication();

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	for (UItemInstance* ItemInstance : QuickbarSlots)
	{
		if (IsValid(ItemInstance))
		{
			RegisterItemForReplication(*ItemInstance);
		}
	}
	for (UItemInstance* ItemInstance : BackpackSlots)
	{
		if (IsValid(ItemInstance))
		{
			RegisterItemForReplication(*ItemInstance);
		}
	}
}

UItemInstance* UInventoryComponent::GetItem(const FInventorySlotAddress& Address) const
{
	const TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Address.Container);
	return Slots && Slots->IsValidIndex(Address.Index) ? (*Slots)[Address.Index] : nullptr;
}

int32 UInventoryComponent::AddItem(
	TScriptInterface<IInventoryItemDefinition> Definition,
	const int32 Quantity,
	const EInventoryContainer PreferredContainer)
{
	FInventoryAdditionReceipt Receipt;
	int32 AddedQuantity = 0;
	if (!BeginAddItem(
			MoveTemp(Definition),
			Quantity,
			PreferredContainer,
			Receipt,
			AddedQuantity))
	{
		return 0;
	}

	ensureMsgf(
		CommitItemAddition(Receipt),
		TEXT("AddItem 创建的同步增加事务必须能够立即提交。"));
	return AddedQuantity;
}

bool UInventoryComponent::BeginAddItem(
	TScriptInterface<IInventoryItemDefinition> Definition,
	int32 Quantity,
	const EInventoryContainer PreferredContainer,
	FInventoryAdditionReceipt& OutReceipt,
	int32& OutAddedQuantity)
{
	OutAddedQuantity = 0;
	if (!GetOwner() || !GetOwner()->HasAuthority()
		|| OutReceipt.IsActive()
		|| !IsValid(Definition.GetObject()) || !Definition.GetInterface() || Quantity <= 0)
	{
		return false;
	}

	const int32 RequestedQuantity = Quantity;
	TArray<TObjectPtr<UItemInstance>>* PreferredSlots = ResolveSlots(PreferredContainer);
	TArray<TObjectPtr<UItemInstance>>* OtherSlots = ResolveSlots(
		PreferredContainer == EInventoryContainer::Quickbar
			? EInventoryContainer::Backpack
			: EInventoryContainer::Quickbar);
	if (!PreferredSlots || !OtherSlots)
	{
		return false;
	}
	TArray<TArray<TObjectPtr<UItemInstance>>*> OrderedContainers { PreferredSlots, OtherSlots };
	bool bCreationFailed = false;
	OutReceipt.Inventory = this;
	OutReceipt.bActive = true;

	for (int32 ContainerIndex = 0; ContainerIndex < OrderedContainers.Num(); ++ContainerIndex)
	{
		TArray<TObjectPtr<UItemInstance>>* Slots = OrderedContainers[ContainerIndex];
		const EInventoryContainer Container = ContainerIndex == 0
			? PreferredContainer
			: (PreferredContainer == EInventoryContainer::Quickbar
				? EInventoryContainer::Backpack
				: EInventoryContainer::Quickbar);
		for (int32 SlotIndex = 0; SlotIndex < Slots->Num(); ++SlotIndex)
		{
			UItemInstance* ExistingItem = (*Slots)[SlotIndex];
			if (Quantity <= 0)
			{
				break;
			}
			if (IsValid(ExistingItem)
				&& ExistingItem->GetDefinition().GetObject() == Definition.GetObject())
			{
				if (UItemStackFeature* Stack = ExistingItem->FindFeature<UItemStackFeature>())
				{
					const int32 PreviousQuantity = Stack->GetQuantity();
					const int32 AddedToStack = Stack->AddQuantity(Quantity);
					if (AddedToStack > 0)
					{
						FInventoryAdditionReceipt::FChange& Change =
							OutReceipt.Changes.AddDefaulted_GetRef();
						Change.Address = FInventorySlotAddress(Container, SlotIndex);
						Change.ItemInstance.Reset(ExistingItem);
						Change.PreviousQuantity = PreviousQuantity;
						Change.ResultingQuantity = Stack->GetQuantity();
						Quantity -= AddedToStack;
					}
				}
			}
		}
	}

	for (int32 ContainerIndex = 0; ContainerIndex < OrderedContainers.Num(); ++ContainerIndex)
	{
		TArray<TObjectPtr<UItemInstance>>* Slots = OrderedContainers[ContainerIndex];
		const EInventoryContainer Container = ContainerIndex == 0
			? PreferredContainer
			: (PreferredContainer == EInventoryContainer::Quickbar
				? EInventoryContainer::Backpack
				: EInventoryContainer::Quickbar);
		if (bCreationFailed)
		{
			break;
		}
		for (int32 SlotIndex = 0; SlotIndex < Slots->Num(); ++SlotIndex)
		{
			TObjectPtr<UItemInstance>& Slot = (*Slots)[SlotIndex];
			if (Quantity <= 0)
			{
				break;
			}
			if (Slot)
			{
				continue;
			}

			UItemInstance* NewItem = CreateItemInstance(Definition, Quantity);
			if (!IsValid(NewItem))
			{
				bCreationFailed = true;
				break;
			}

			int32 InstanceQuantity = 1;
			if (UItemStackFeature* Stack = NewItem->FindFeature<UItemStackFeature>())
			{
				InstanceQuantity = FMath::Min(Quantity, Stack->GetMaxStackSize());
				Stack->SetQuantity(InstanceQuantity);
			}

			Slot = NewItem;
			RegisterItemForReplication(*NewItem);
			FInventoryAdditionReceipt::FChange& Change =
				OutReceipt.Changes.AddDefaulted_GetRef();
			Change.Address = FInventorySlotAddress(Container, SlotIndex);
			Change.ItemInstance.Reset(NewItem);
			Change.ResultingQuantity = InstanceQuantity;
			Change.bCreatedItem = true;
			Quantity -= InstanceQuantity;
		}
	}

	OutAddedQuantity = RequestedQuantity - Quantity;
	OutReceipt.AddedQuantity = OutAddedQuantity;
	if (OutAddedQuantity <= 0)
	{
		OutReceipt.Reset();
		return false;
	}

	NotifyItemDataChanged();
	return true;
}

bool UInventoryComponent::CommitItemAddition(FInventoryAdditionReceipt& Receipt)
{
	if (!Receipt.bActive || Receipt.Inventory.Get() != this
		|| !GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	Receipt.Reset();
	return true;
}

bool UInventoryComponent::RollbackItemAddition(FInventoryAdditionReceipt& Receipt)
{
	if (!Receipt.bActive || Receipt.Inventory.Get() != this
		|| !GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	for (const FInventoryAdditionReceipt::FChange& Change : Receipt.Changes)
	{
		const TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Change.Address.Container);
		UItemInstance* Item = Change.ItemInstance.Get();
		if (!Slots || !Slots->IsValidIndex(Change.Address.Index)
			|| (*Slots)[Change.Address.Index] != Item || !IsValid(Item))
		{
			return false;
		}
		const UItemStackFeature* Stack = Item->FindFeature<UItemStackFeature>();
		const int32 CurrentQuantity = Stack ? Stack->GetQuantity() : 1;
		if (CurrentQuantity != Change.ResultingQuantity)
		{
			return false;
		}
	}

	for (int32 ChangeIndex = Receipt.Changes.Num() - 1; ChangeIndex >= 0; --ChangeIndex)
	{
		const FInventoryAdditionReceipt::FChange& Change = Receipt.Changes[ChangeIndex];
		TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Change.Address.Container);
		UItemInstance* Item = Change.ItemInstance.Get();
		check(Slots && Item && (*Slots)[Change.Address.Index] == Item);
		if (Change.bCreatedItem)
		{
			(*Slots)[Change.Address.Index] = nullptr;
			UnregisterItemForReplication(*Item);
		}
		else
		{
			UItemStackFeature* Stack = Item->FindFeature<UItemStackFeature>();
			check(Stack && Change.PreviousQuantity > 0);
			Stack->SetQuantity(Change.PreviousQuantity);
		}
	}

	Receipt.Reset();
	NotifyItemDataChanged();
	return true;
}

bool UInventoryComponent::CanAddItem(
	TScriptInterface<IInventoryItemDefinition> Definition,
	const int32 Quantity) const
{
	if (!GetOwner()
		|| !GetOwner()->HasAuthority()
		|| !IsValid(Definition.GetObject())
		|| !Definition.GetInterface()
		|| Quantity <= 0)
	{
		return false;
	}

	int32 Remaining = Quantity;
	int32 EmptySlotCapacity = 1;
	for (const UItemFeature* Feature : Definition->GetItemFeatureTemplates())
	{
		if (const UItemStackFeature* StackTemplate = Cast<UItemStackFeature>(Feature))
		{
			EmptySlotCapacity = FMath::Max(1, StackTemplate->GetMaxStackSize());
			break;
		}
	}

	const TArray<const TArray<TObjectPtr<UItemInstance>>*> Containers {
		&QuickbarSlots,
		&BackpackSlots
	};
	for (const TArray<TObjectPtr<UItemInstance>>* Slots : Containers)
	{
		for (const UItemInstance* ExistingItem : *Slots)
		{
			if (Remaining <= 0)
			{
				return true;
			}
			if (!IsValid(ExistingItem))
			{
				Remaining -= EmptySlotCapacity;
				continue;
			}
			if (ExistingItem->GetDefinition().GetObject() == Definition.GetObject())
			{
				if (const UItemStackFeature* Stack =
					ExistingItem->FindFeature<UItemStackFeature>())
				{
					Remaining -= FMath::Max(
						0,
						Stack->GetMaxStackSize() - Stack->GetQuantity());
				}
			}
		}
	}
	return Remaining <= 0;
}

bool UInventoryComponent::GrantItemToQuickbar(
	TScriptInterface<IInventoryItemDefinition> Definition,
	const int32 QuickbarIndex,
	const int32 Quantity)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !QuickbarSlots.IsValidIndex(QuickbarIndex)
		|| QuickbarSlots[QuickbarIndex] || !IsValid(Definition.GetObject())
		|| !Definition.GetInterface() || Quantity <= 0)
	{
		return false;
	}

	UItemInstance* NewItem = CreateItemInstance(Definition, Quantity);
	if (!IsValid(NewItem))
	{
		return false;
	}

	if (UItemStackFeature* Stack = NewItem->FindFeature<UItemStackFeature>())
	{
		Stack->SetQuantity(FMath::Min(Quantity, Stack->GetMaxStackSize()));
	}

	QuickbarSlots[QuickbarIndex] = NewItem;
	RegisterItemForReplication(*NewItem);
	NotifyItemDataChanged();
	return true;
}

bool UInventoryComponent::MoveItem(const FInventorySlotAddress& From, const FInventorySlotAddress& To)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsAddressValid(From) || !IsAddressValid(To)
		|| (From.Container == To.Container && From.Index == To.Index))
	{
		return false;
	}

	TArray<TObjectPtr<UItemInstance>>* FromSlots = ResolveSlots(From.Container);
	TArray<TObjectPtr<UItemInstance>>* ToSlots = ResolveSlots(To.Container);
	TObjectPtr<UItemInstance>& Source = (*FromSlots)[From.Index];
	TObjectPtr<UItemInstance>& Destination = (*ToSlots)[To.Index];
	if (!Source)
	{
		return false;
	}

	if (Destination
		&& Source->GetDefinition().GetObject() == Destination->GetDefinition().GetObject())
	{
		UItemStackFeature* SourceStack = Source->FindFeature<UItemStackFeature>();
		UItemStackFeature* DestinationStack = Destination->FindFeature<UItemStackFeature>();
		if (SourceStack && DestinationStack)
		{
			const int32 MovedQuantity = DestinationStack->AddQuantity(SourceStack->GetQuantity());
			if (MovedQuantity > 0)
			{
				const int32 RemainingQuantity = SourceStack->GetQuantity() - MovedQuantity;
				if (RemainingQuantity <= 0)
				{
					UItemInstance* RemovedItem = Source;
					Source = nullptr;
					UnregisterItemForReplication(*RemovedItem);
				}
				else
				{
					SourceStack->SetQuantity(RemainingQuantity);
				}
				ClearSelectionIfMovedSlotWasSelected(From, To);
				NotifyItemDataChanged();
				return true;
			}
		}
	}

	Swap(Source, Destination);
	ClearSelectionIfMovedSlotWasSelected(From, To);
	NotifyItemDataChanged();
	return true;
}

void UInventoryComponent::RequestMoveItem(
	const FInventorySlotAddress& From,
	const FInventorySlotAddress& To)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		MoveItem(From, To);
	}
	else
	{
		ServerMoveItem(From, To);
	}
}

void UInventoryComponent::ServerMoveItem_Implementation(
	const FInventorySlotAddress From,
	const FInventorySlotAddress To)
{
	MoveItem(From, To);
}

bool UInventoryComponent::RemoveItem(const FInventorySlotAddress& Address)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsAddressValid(Address))
	{
		return false;
	}

	TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Address.Container);
	UItemInstance* RemovedItem = (*Slots)[Address.Index];
	if (!IsValid(RemovedItem))
	{
		return false;
	}

	(*Slots)[Address.Index] = nullptr;
	ClearSelectionIfMovedSlotWasSelected(Address, Address);
	UnregisterItemForReplication(*RemovedItem);
	NotifyItemDataChanged();
	return true;
}

bool UInventoryComponent::BeginConsumeItemQuantity(
	const FInventorySlotAddress& Address,
	const int32 Quantity,
	FInventoryConsumptionReceipt& OutReceipt)
{
	if (OutReceipt.IsActive()
		|| !GetOwner() || !GetOwner()->HasAuthority()
		|| !IsAddressValid(Address) || Quantity <= 0)
	{
		return false;
	}

	TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Address.Container);
	UItemInstance* Item = Slots ? (*Slots)[Address.Index].Get() : nullptr;
	if (!IsValid(Item))
	{
		return false;
	}

	UItemStackFeature* Stack = Item->FindFeature<UItemStackFeature>();
	const int32 AvailableQuantity = Stack ? Stack->GetQuantity() : 1;
	if (Quantity > AvailableQuantity || (!Stack && Quantity != 1))
	{
		return false;
	}

	OutReceipt.Inventory = this;
	OutReceipt.ItemInstance.Reset(Item);
	OutReceipt.Address = Address;
	OutReceipt.PreviousQuantity = AvailableQuantity;
	OutReceipt.ConsumedQuantity = Quantity;
	OutReceipt.PreviousSelectedQuickbarIndex = QuickbarSelection.Index;
	OutReceipt.bRemovedWholeInstance = Quantity == AvailableQuantity;
	OutReceipt.bActive = true;

	if (!OutReceipt.bRemovedWholeInstance)
	{
		check(Stack);
		Stack->SetQuantity(AvailableQuantity - Quantity);
		NotifyItemDataChanged();
		return true;
	}

	(*Slots)[Address.Index] = nullptr;
	ClearSelectionIfMovedSlotWasSelected(Address, Address);
	UnregisterItemForReplication(*Item);
	NotifyItemDataChanged();
	return true;
}

bool UInventoryComponent::CommitItemConsumption(
	FInventoryConsumptionReceipt& Receipt)
{
	if (!Receipt.bActive || Receipt.Inventory.Get() != this
		|| !GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}
	Receipt.Reset();
	return true;
}

bool UInventoryComponent::RollbackItemConsumption(
	FInventoryConsumptionReceipt& Receipt)
{
	if (!Receipt.bActive || Receipt.Inventory.Get() != this
		|| !GetOwner() || !GetOwner()->HasAuthority()
		|| !IsAddressValid(Receipt.Address))
	{
		return false;
	}

	TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Receipt.Address.Container);
	UItemInstance* Item = Receipt.ItemInstance.Get();
	if (!Slots || !IsValid(Item))
	{
		return false;
	}

	if (Receipt.bRemovedWholeInstance)
	{
		if ((*Slots)[Receipt.Address.Index])
		{
			return false;
		}
		(*Slots)[Receipt.Address.Index] = Item;
		RegisterItemForReplication(*Item);
	}
	else
	{
		if ((*Slots)[Receipt.Address.Index] != Item)
		{
			return false;
		}
		UItemStackFeature* Stack = Item->FindFeature<UItemStackFeature>();
		if (!Stack
			|| Stack->GetQuantity()
				!= Receipt.PreviousQuantity - Receipt.ConsumedQuantity)
		{
			return false;
		}
		Stack->SetQuantity(Receipt.PreviousQuantity);
	}

	SetAuthorityQuickbarSelection(Receipt.PreviousSelectedQuickbarIndex);
	if (Receipt.Address.Container == EInventoryContainer::Quickbar
		&& Receipt.Address.Index == QuickbarSelection.Index)
	{
		if (APawn* OwningPawn = GetOwningPawn())
		{
			if (UEquipmentComponent* Equipment =
				OwningPawn->FindComponentByClass<UEquipmentComponent>())
			{
				const UEquippableItemFeature* Equippable =
					Item->FindFeature<UEquippableItemFeature>();
				if (!Equippable || !Equipment->EquipItem(Item, *Equippable))
				{
					Equipment->UnequipItem();
				}
			}
		}
	}

	Receipt.Reset();
	NotifyItemDataChanged();
	return true;
}

bool UInventoryComponent::ExtractSelectedEquippedItem(
	UItemInstance*& OutItemInstance,
	AEquippedItemActor*& OutEquippedActor)
{
	OutItemInstance = nullptr;
	OutEquippedActor = nullptr;
	if (!GetOwner()
		|| !GetOwner()->HasAuthority()
		|| !QuickbarSlots.IsValidIndex(QuickbarSelection.Index))
	{
		return false;
	}

	APawn* OwningPawn = GetOwningPawn();
	UEquipmentComponent* Equipment = IsValid(OwningPawn)
		? OwningPawn->FindComponentByClass<UEquipmentComponent>()
		: nullptr;
	UItemInstance* SelectedItem = QuickbarSlots[QuickbarSelection.Index];
	if (!Equipment
		|| !IsValid(SelectedItem)
		|| Equipment->GetCurrentEquippedItem() != SelectedItem
		|| !IsValid(Equipment->GetEquippedActor()))
	{
		return false;
	}

	const int32 ExtractedSlotIndex = QuickbarSelection.Index;
	QuickbarSlots[ExtractedSlotIndex] = nullptr;
	SetAuthorityQuickbarSelection(INDEX_NONE);
	UnregisterItemForReplication(*SelectedItem);

	AEquippedItemActor* ReleasedActor = nullptr;
	if (!Equipment->ReleaseEquippedActor(SelectedItem, ReleasedActor))
	{
		QuickbarSlots[ExtractedSlotIndex] = SelectedItem;
		SetAuthorityQuickbarSelection(ExtractedSlotIndex);
		RegisterItemForReplication(*SelectedItem);
		return false;
	}

	OutItemInstance = SelectedItem;
	OutEquippedActor = ReleasedActor;
	NotifyItemDataChanged();
	return true;
}

void UInventoryComponent::SelectQuickbarSlot(const int32 QuickbarIndex)
{
	if (!QuickbarSlots.IsValidIndex(QuickbarIndex))
	{
		return;
	}

	if (GetOwner() && GetOwner()->HasAuthority())
	{
		SelectQuickbarSlotAuthority(QuickbarIndex);
	}
	else
	{
		const uint32 MinimumRevision = QuickbarSelection.Revision == MAX_uint32
			? 1u
			: QuickbarSelection.Revision + 1u;
		if (NextQuickbarSelectionRevision < MinimumRevision)
		{
			NextQuickbarSelectionRevision = MinimumRevision;
		}
		uint32 PredictionRevision = NextQuickbarSelectionRevision++;
		if (PredictionRevision == 0)
		{
			PredictionRevision = NextQuickbarSelectionRevision++;
		}
		PredictedQuickbarSelection.Index = QuickbarIndex;
		PredictedQuickbarSelection.Revision = PredictionRevision;
		NotifyItemDataChanged();
		ServerSelectQuickbarSlot(QuickbarIndex, PredictionRevision);
	}
}

void UInventoryComponent::ServerSelectQuickbarSlot_Implementation(
	const int32 QuickbarIndex,
	const uint32 SelectionRevision)
{
	if (SelectionRevision <= QuickbarSelection.Revision)
	{
		return;
	}

	if (!QuickbarSlots.IsValidIndex(QuickbarIndex) || !IsValid(GetOwningPawn()))
	{
		// 可靠 RPC 也必须有确定的拒绝确认；否则 owning client 会永久保留
		// 一个服务器从未接受的预测槽位。
		SetAuthorityQuickbarSelection(QuickbarSelection.Index, SelectionRevision);
		NotifyItemDataChanged();
		return;
	}
	SelectQuickbarSlotAuthority(QuickbarIndex, SelectionRevision);
}

void UInventoryComponent::NotifyItemDataChanged()
{
	InventoryChangedEvent.Broadcast();
}

void UInventoryComponent::OnRep_Slots()
{
	NotifyItemDataChanged();
}

void UInventoryComponent::OnRep_QuickbarSelection()
{
	if (PredictedQuickbarSelection.Revision <= QuickbarSelection.Revision)
	{
		PredictedQuickbarSelection = {};
	}
	const uint32 MinimumRevision = QuickbarSelection.Revision == MAX_uint32
		? 1u
		: QuickbarSelection.Revision + 1u;
	NextQuickbarSelectionRevision = FMath::Max(
		NextQuickbarSelectionRevision,
		MinimumRevision);
	NotifyItemDataChanged();
}

bool UInventoryComponent::IsAddressValid(const FInventorySlotAddress& Address) const
{
	const TArray<TObjectPtr<UItemInstance>>* Slots = ResolveSlots(Address.Container);
	return Slots && Slots->IsValidIndex(Address.Index);
}

TArray<TObjectPtr<UItemInstance>>* UInventoryComponent::ResolveSlots(const EInventoryContainer Container)
{
	switch (Container)
	{
	case EInventoryContainer::Quickbar:
		return &QuickbarSlots;
	case EInventoryContainer::Backpack:
		return &BackpackSlots;
	default:
		return nullptr;
	}
}

const TArray<TObjectPtr<UItemInstance>>* UInventoryComponent::ResolveSlots(
	const EInventoryContainer Container) const
{
	switch (Container)
	{
	case EInventoryContainer::Quickbar:
		return &QuickbarSlots;
	case EInventoryContainer::Backpack:
		return &BackpackSlots;
	default:
		return nullptr;
	}
}

UItemInstance* UInventoryComponent::CreateItemInstance(
	TScriptInterface<IInventoryItemDefinition> Definition,
	const int32 Quantity)
{
	UItemInstance* ItemInstance = NewObject<UItemInstance>(this);
	if (!ItemInstance->Initialize(Definition))
	{
		return nullptr;
	}

	if (UItemStackFeature* Stack = ItemInstance->FindFeature<UItemStackFeature>())
	{
		Stack->SetQuantity(FMath::Min(Quantity, Stack->GetMaxStackSize()));
	}
	return ItemInstance;
}

void UInventoryComponent::RegisterItemForReplication(UItemInstance& ItemInstance)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsReadyForReplication())
	{
		return;
	}

	AddReplicatedSubObject(&ItemInstance, COND_OwnerOnly);
	for (UItemFeature* Feature : ItemInstance.GetFeatures())
	{
		if (IsValid(Feature))
		{
			AddReplicatedSubObject(Feature, COND_OwnerOnly);
		}
	}
}

void UInventoryComponent::UnregisterItemForReplication(UItemInstance& ItemInstance)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsReadyForReplication())
	{
		return;
	}

	for (UItemFeature* Feature : ItemInstance.GetFeatures())
	{
		if (IsValid(Feature))
		{
			RemoveReplicatedSubObject(Feature);
		}
	}
	RemoveReplicatedSubObject(&ItemInstance);
}

void UInventoryComponent::ClearSelectionIfMovedSlotWasSelected(
	const FInventorySlotAddress& From, const FInventorySlotAddress& To)
{
	if (QuickbarSelection.Index == INDEX_NONE)
	{
		return;
	}

	const bool bTouchesSelectedSlot =
		(From.Container == EInventoryContainer::Quickbar && From.Index == QuickbarSelection.Index)
		|| (To.Container == EInventoryContainer::Quickbar && To.Index == QuickbarSelection.Index);
	if (!bTouchesSelectedSlot)
	{
		return;
	}

	SetAuthorityQuickbarSelection(INDEX_NONE);
	if (APawn* OwningPawn = GetOwningPawn())
	{
		if (UEquipmentComponent* Equipment = OwningPawn->FindComponentByClass<UEquipmentComponent>())
		{
			Equipment->UnequipItem();
		}
	}
}

void UInventoryComponent::SelectQuickbarSlotAuthority(
	const int32 QuickbarIndex,
	const uint32 RequestedRevision)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !QuickbarSlots.IsValidIndex(QuickbarIndex))
	{
		return;
	}

	APawn* OwningPawn = GetOwningPawn();
	if (!IsValid(OwningPawn))
	{
		return;
	}

	SetAuthorityQuickbarSelection(QuickbarIndex, RequestedRevision);
	if (UEquipmentComponent* Equipment = OwningPawn->FindComponentByClass<UEquipmentComponent>())
	{
		UItemInstance* ItemInstance = QuickbarSlots[QuickbarIndex];
		const UEquippableItemFeature* Equippable = IsValid(ItemInstance)
			? ItemInstance->FindFeature<UEquippableItemFeature>()
			: nullptr;
		if (!Equippable || !Equipment->EquipItem(ItemInstance, *Equippable))
		{
			Equipment->UnequipItem();
		}
	}
	NotifyItemDataChanged();
}

void UInventoryComponent::SetAuthorityQuickbarSelection(
	const int32 QuickbarIndex,
	const uint32 RequestedRevision)
{
	check(GetOwner() && GetOwner()->HasAuthority());
	QuickbarSelection.Index = QuickbarIndex;
	if (RequestedRevision > QuickbarSelection.Revision)
	{
		QuickbarSelection.Revision = RequestedRevision;
	}
	else
	{
		++QuickbarSelection.Revision;
		if (QuickbarSelection.Revision == 0)
		{
			++QuickbarSelection.Revision;
		}
	}
	GetOwner()->ForceNetUpdate();
}

APawn* UInventoryComponent::GetOwningPawn() const
{
	const APlayerState* PlayerState = Cast<APlayerState>(GetOwner());
	return PlayerState ? PlayerState->GetPawn() : nullptr;
}

void UInventoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GetOwner() && GetOwner()->HasAuthority() && IsReadyForReplication())
	{
		for (UItemInstance* ItemInstance : QuickbarSlots)
		{
			if (IsValid(ItemInstance))
			{
				UnregisterItemForReplication(*ItemInstance);
			}
		}
		for (UItemInstance* ItemInstance : BackpackSlots)
		{
			if (IsValid(ItemInstance))
			{
				UnregisterItemForReplication(*ItemInstance);
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}
