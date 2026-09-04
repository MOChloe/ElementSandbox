#include "Item/ItemInstance.h"

#include "ElementSandboxItems.h"
#include "Inventory/InventoryComponent.h"
#include "Item/ItemFeature.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UObjectGlobals.h"

UWorld* UItemInstance::GetWorld() const
{
	return GetOuter() ? GetOuter()->GetWorld() : nullptr;
}

void UItemInstance::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UItemInstance, Definition);
	DOREPLIFETIME(UItemInstance, Features);
}

bool UItemInstance::Initialize(TScriptInterface<IInventoryItemDefinition> InDefinition)
{
	Definition = nullptr;
	Features.Reset();

	if (!IsValid(InDefinition.GetObject()) || !InDefinition.GetInterface())
	{
		return false;
	}

	for (UItemFeature* FeatureTemplate : InDefinition->GetItemFeatureTemplates())
	{
		if (!IsValid(FeatureTemplate))
		{
			UE_LOG(
				LogElementSandboxItems,
				Error,
				TEXT("Inventory item definition %s 包含空 Feature 模板。"),
				*GetNameSafe(InDefinition.GetObject()));
			Features.Reset();
			return false;
		}

		FObjectDuplicationParameters DuplicationParameters(FeatureTemplate, this);
		// Native Definition CDOs own their templates as default subobjects. Those
		// template-only flags must never leak onto the per-item runtime duplicate,
		// otherwise UE rejects the Feature when Inventory registers it for replication.
		DuplicationParameters.FlagMask &= ~(
			RF_ClassDefaultObject
			| RF_ArchetypeObject
			| RF_DefaultSubObject);
		UItemFeature* RuntimeFeature = Cast<UItemFeature>(
			StaticDuplicateObjectEx(DuplicationParameters));
		if (!IsValid(RuntimeFeature))
		{
			Features.Reset();
			return false;
		}

		Features.Add(RuntimeFeature);
	}

	Definition = MoveTemp(InDefinition);
	for (UItemFeature* Feature : Features)
	{
		Feature->OnItemCreated(*this);
	}
	return true;
}
void UItemInstance::NotifyOwningInventoryChanged() const
{
	if (UInventoryComponent* Inventory = Cast<UInventoryComponent>(GetOuter()))
	{
		Inventory->NotifyItemDataChanged();
	}
}

void UItemInstance::OnRep_ItemData()
{
	NotifyOwningInventoryChanged();
}
