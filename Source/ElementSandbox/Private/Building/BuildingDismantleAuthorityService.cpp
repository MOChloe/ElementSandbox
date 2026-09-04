#include "Building/BuildingDismantleAuthorityService.h"

#include "BuildingWorldSubsystem.h"
#include "Building/BuildingItemFeature.h"
#include "Definition/BuildingDefinition.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildTransformFragment.h"
#include "GameFramework/Pawn.h"
#include "Inventory/InventoryAdditionReceipt.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryTypes.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Items/DemolitionToolItemFeature.h"
#include "Spatial/BuildSpatialIndex.h"

EBuildingDismantleFailure FBuildingDismantleAuthorityService::TryBeginRequest(
	const bool bHealthDepleted,
	const bool bInventoryOpen,
	const double RequestTime,
	double& InOutLastRequestTime)
{
	if (bHealthDepleted)
	{
		return EBuildingDismantleFailure::PlayerUnavailable;
	}
	if (bInventoryOpen)
	{
		return EBuildingDismantleFailure::InventoryOpen;
	}
	if (!FMath::IsFinite(RequestTime))
	{
		return EBuildingDismantleFailure::InvalidTarget;
	}

	constexpr double MinimumDismantleIntervalSeconds = 0.08;
	if (RequestTime - InOutLastRequestTime < MinimumDismantleIntervalSeconds)
	{
		return EBuildingDismantleFailure::RateLimited;
	}
	InOutLastRequestTime = RequestTime;
	return EBuildingDismantleFailure::None;
}

EBuildingDismantleFailure FBuildingDismantleAuthorityService::TryDismantle(
	UBuildingWorldSubsystem& BuildingSubsystem,
	UInventoryComponent& Inventory,
	APawn& Pawn,
	const FWorldEntityId WorldEntityId,
	const double MaximumDistance)
{
	if (!Pawn.HasAuthority() || !WorldEntityId.IsSet() || !FMath::IsFinite(MaximumDistance)
		|| MaximumDistance <= 0.0)
	{
		return EBuildingDismantleFailure::PlayerUnavailable;
	}

	const int32 SelectedIndex = Inventory.GetSelectedQuickbarIndex();
	UItemInstance* SelectedItem = Inventory.GetItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, SelectedIndex));
	const UDemolitionToolItemFeature* Tool = SelectedItem
		? SelectedItem->FindFeature<UDemolitionToolItemFeature>()
		: nullptr;
	if (!Tool)
	{
		return EBuildingDismantleFailure::NoDemolitionTool;
	}

	const FBuildEntityHandle Entity = BuildingSubsystem.FindEntity(WorldEntityId);
	const FBuildEntityRegistry& Registry = BuildingSubsystem.GetRegistry();
	const FBuildDefinitionFragment* DefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment
		? DefinitionFragment->Definition.Get()
		: nullptr;
	if (!BuildingSubsystem.IsEntityAlive(Entity) || !Definition)
	{
		return EBuildingDismantleFailure::InvalidTarget;
	}

	const FBuildingDismantleReward* Reward = Tool->FindReward(Definition->DefinitionId);
	if (!Reward)
	{
		return EBuildingDismantleFailure::NotDismantleable;
	}
	UItemDefinition* RewardItem = Reward->ItemDefinition;
	const int32 RewardQuantity = Reward->Quantity;
	const FBuildTransformFragment* TargetTransform =
		Registry.FindFragment<FBuildTransformFragment>(Entity);
	if (Reward->bPreservePlacementShape && !TargetTransform)
	{
		return EBuildingDismantleFailure::InvalidTarget;
	}

	FBox TargetBounds(ForceInit);
	if (!BuildingSubsystem.GetSpatialIndex().TryGetBounds(Entity, TargetBounds)
		|| TargetBounds.ComputeSquaredDistanceToPoint(Pawn.GetActorLocation())
			> FMath::Square(MaximumDistance))
	{
		return EBuildingDismantleFailure::OutOfRange;
	}

	if (!Inventory.CanAddItem(RewardItem, RewardQuantity))
	{
		return EBuildingDismantleFailure::InventoryFull;
	}

	FInventoryAdditionReceipt RewardReceipt;
	int32 AddedQuantity = 0;
	if (!Inventory.BeginAddItem(
			RewardItem,
			RewardQuantity,
			EInventoryContainer::Backpack,
			RewardReceipt,
			AddedQuantity)
		|| AddedQuantity != RewardQuantity)
	{
		if (RewardReceipt.IsActive())
		{
			ensureMsgf(
				Inventory.RollbackItemAddition(RewardReceipt),
				TEXT("拆除返还未完整加入时必须撤销全部背包变化。"));
		}
		return EBuildingDismantleFailure::InventoryFull;
	}

	if (Reward->bPreservePlacementShape)
	{
		UItemInstance* ReclaimedItem = RewardReceipt.GetSingleCreatedItem();
		UBuildingItemFeature* BuildingItem = ReclaimedItem
			? ReclaimedItem->FindFeature<UBuildingItemFeature>()
			: nullptr;
		FTransform PlacementShape = TargetTransform->WorldTransform;
		PlacementShape.SetLocation(FVector::ZeroVector);
		PlacementShape.NormalizeRotation();
		if (!BuildingItem
			|| !BuildingItem->ConfigureReclaimedBuilding(
				Definition->DefinitionId,
				PlacementShape))
		{
			ensureMsgf(
				Inventory.RollbackItemAddition(RewardReceipt),
				TEXT("回收构件形态配置失败后必须撤销已经加入的背包实例。"));
			return EBuildingDismantleFailure::RewardConfigurationFailed;
		}
	}

	const bool bDestroyReportedSuccess = BuildingSubsystem.DestroyEntity(Entity);
	if (BuildingSubsystem.IsEntityAlive(Entity))
	{
		ensureMsgf(
			Inventory.RollbackItemAddition(RewardReceipt),
			TEXT("Building 拒绝拆除后必须撤销已经加入的返还物品。"));
		return EBuildingDismantleFailure::DestroyFailed;
	}

	// DestroyEntity 的派生表现/Journal 收尾可能在实体已经终结后报告失败；此时玩家仍必须得到返还物品。
	ensureMsgf(
		bDestroyReportedSuccess,
		TEXT("Building 已完成 GameplayDestroy，但派生清理报告失败；保留拆除返还以避免双重损失。"));
	ensureMsgf(
		Inventory.CommitItemAddition(RewardReceipt),
		TEXT("Building 已终结后返还物品事务必须能够提交。"));
	return EBuildingDismantleFailure::None;
}
