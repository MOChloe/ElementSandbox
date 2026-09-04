#include "Building/BuildingPlacementAuthorityService.h"

#include "Building/BuildingItemFeature.h"
#include "Building/BuildingPlacementResolver.h"
#include "BuildingWorldSubsystem.h"
#include "Definition/BuildingDefinition.h"
#include "GameFramework/Pawn.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryConsumptionReceipt.h"
#include "Inventory/InventoryTypes.h"
#include "Item/ItemInstance.h"

EBuildPlacementFailure FBuildingPlacementAuthorityService::TryBeginRequest(
	const bool bHealthDepleted,
	const bool bInventoryOpen,
	const double RequestTime,
	double& InOutLastRequestTime)
{
	if (bHealthDepleted)
	{
		return EBuildPlacementFailure::PlayerUnavailable;
	}
	if (bInventoryOpen)
	{
		return EBuildPlacementFailure::InventoryOpen;
	}
	if (!FMath::IsFinite(RequestTime))
	{
		return EBuildPlacementFailure::InvalidTransform;
	}
	constexpr double MinimumPlacementIntervalSeconds = 0.08;
	if (RequestTime - InOutLastRequestTime < MinimumPlacementIntervalSeconds)
	{
		return EBuildPlacementFailure::RateLimited;
	}
	InOutLastRequestTime = RequestTime;
	return EBuildPlacementFailure::None;
}

EBuildPlacementFailure FBuildingPlacementAuthorityService::TryPlace(
	UWorld& World,
	UBuildingWorldSubsystem& BuildingSubsystem,
	UInventoryComponent& Inventory,
	APawn& BuilderPawn,
	const int32 QuickbarIndex,
	const FVector& SurfaceLocation,
	const FVector& ExpectedResolvedLocation,
	const uint8 YawQuarterTurns,
	TFunctionRef<bool(const FVector& ResolvedLocation)> CanMutateResolvedLocation)
{
	if (QuickbarIndex < 0
		|| QuickbarIndex >= UInventoryComponent::QuickbarSlotCount
		|| Inventory.GetSelectedQuickbarIndex() != QuickbarIndex)
	{
		return EBuildPlacementFailure::InventoryChanged;
	}
	const FInventorySlotAddress Address(
		EInventoryContainer::Quickbar,
		QuickbarIndex);
	UItemInstance* Item = Inventory.GetItem(Address);
	const UBuildingItemFeature* BuildingFeature = Item
		? Item->FindFeature<UBuildingItemFeature>()
		: nullptr;
	if (!Item || !BuildingFeature
		|| BuildingFeature->GetBuildingDefinitionId().IsNone())
	{
		return EBuildPlacementFailure::NoBuildItem;
	}

	UBuildingDefinition* Definition = BuildingSubsystem.FindDefinition(
		BuildingFeature->GetBuildingDefinitionId());
	if (!Definition)
	{
		return EBuildPlacementFailure::MissingDefinition;
	}

	FBuildPlacementEvaluation Evaluation;
	FBuildingPlacementResolver::ResolveIntent(
		World,
		BuildingSubsystem,
		*Definition,
		SurfaceLocation,
		ExpectedResolvedLocation,
		BuildingFeature->GetPlacementShapeTransform(),
		YawQuarterTurns,
		BuilderPawn.GetActorLocation(),
		&BuilderPawn,
		Evaluation);
	if (!Evaluation.IsAllowed())
	{
		return Evaluation.Failure;
	}
	if (!CanMutateResolvedLocation(Evaluation.ResolvedTransform.GetLocation()))
	{
		return EBuildPlacementFailure::StreamingNotReady;
	}

	FInventoryConsumptionReceipt Receipt;
	if (!Inventory.BeginConsumeItemQuantity(Address, 1, Receipt))
	{
		return EBuildPlacementFailure::InventoryChanged;
	}
	const FBuildEntityHandle Entity = BuildingSubsystem.CreateEntity(
		*Definition,
		Evaluation.ResolvedTransform,
		EBuildSpatialMobility::Static);
	if (!Entity.IsSet())
	{
		ensureMsgf(
			Inventory.RollbackItemConsumption(Receipt),
			TEXT("Building 创建失败后必须完整恢复背包扣除。"));
		return EBuildPlacementFailure::CreateFailed;
	}
	if (!Inventory.CommitItemConsumption(Receipt))
	{
		ensureMsgf(
			BuildingSubsystem.DestroyEntity(Entity),
			TEXT("背包事务提交失败后必须撤销已创建的 Building。"));
		ensureMsgf(
			Inventory.RollbackItemConsumption(Receipt),
			TEXT("背包事务提交失败后必须恢复物品。"));
		return EBuildPlacementFailure::CreateFailed;
	}
	return EBuildPlacementFailure::None;
}
