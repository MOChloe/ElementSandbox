// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/ElementSandboxPlayerController.h"

#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Building/BuildingDismantleAuthorityService.h"
#include "Building/BuildingPlacementAuthorityService.h"
#include "Building/BuildingPlacementComponent.h"
#include "BuildingCatalogWorldSubsystem.h"
#include "BuildingWorldSubsystem.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Game/BuildingCollisionActivationComponent.h"
#include "Game/PresentationViewSourceComponent.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Game/PlayerHealthCoordinatorComponent.h"
#include "Game/WorldStreamingHUDPresenterComponent.h"
#include "Game/WorldObjectCollisionActivationComponent.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryHUDWidget.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Blueprint/UserWidget.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Focus/BuildingFocusQueryComponent.h"
#include "Focus/BuildingFocusHighlightPresenterComponent.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/FocusPromptPresenterComponent.h"
#include "Focus/WorldObjectFocusQueryComponent.h"
#include "Focus/WorldObjectFocusHighlightPresenterComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "Items/DemolitionToolItemFeature.h"
#include "Camera/PlayerCameraManager.h"
#include "NetBulkTransferScheduler.h"
#include "Tags/ElementGameplayTags.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldObjectWorldSubsystem.h"
#include "Tree/SettlementTreeCollisionSourceComponent.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "Network/MeteorStreamingComponent.h"
#include "WorldObjects/WorldObjectEquipmentBridgeComponent.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupAuthorityService.h"
#include "WorldObjects/WorldObjectPickupComponent.h"

AElementSandboxPlayerController::AElementSandboxPlayerController()
{
	FocusHostComponent = CreateDefaultSubobject<UFocusHostComponent>(TEXT("FocusHostComponent"));
	BuildingFocusQueryComponent =
		CreateDefaultSubobject<UBuildingFocusQueryComponent>(TEXT("BuildingFocusQueryComponent"));
	BuildingFocusHighlightPresenterComponent =
		CreateDefaultSubobject<UBuildingFocusHighlightPresenterComponent>(
			TEXT("BuildingFocusHighlightPresenterComponent"));
	WorldObjectFocusQueryComponent =
		CreateDefaultSubobject<UWorldObjectFocusQueryComponent>(TEXT("WorldObjectFocusQueryComponent"));
	WorldObjectPickupComponent =
		CreateDefaultSubobject<UWorldObjectPickupComponent>(TEXT("WorldObjectPickupComponent"));
	WorldObjectFocusHighlightPresenterComponent = CreateDefaultSubobject<UWorldObjectFocusHighlightPresenterComponent>(
		TEXT("WorldObjectFocusHighlightPresenterComponent"));
	FocusPromptPresenterComponent =
		CreateDefaultSubobject<UFocusPromptPresenterComponent>(TEXT("FocusPromptPresenterComponent"));
	PresentationViewSourceComponent =
		CreateDefaultSubobject<UPresentationViewSourceComponent>(TEXT("PresentationViewSourceComponent"));
	BuildingCollisionActivationComponent =
		CreateDefaultSubobject<UBuildingCollisionActivationComponent>(TEXT("BuildingCollisionActivationComponent"));
	SettlementTreeCollisionSourceComponent =
		CreateDefaultSubobject<USettlementTreeCollisionSourceComponent>(TEXT("SettlementTreeCollisionSourceComponent"));
	WorldObjectCollisionActivationComponent =
		CreateDefaultSubobject<UWorldObjectCollisionActivationComponent>(TEXT("WorldObjectCollisionActivationComponent"));
	BuildingPlacementComponent =
		CreateDefaultSubobject<UBuildingPlacementComponent>(TEXT("BuildingPlacementComponent"));
	WorldChunkStreamingComponent =
		CreateDefaultSubobject<UWorldChunkStreamingComponent>(TEXT("WorldChunkStreamingComponent"));
	WorldStreamingHUDPresenterComponent =
		CreateDefaultSubobject<UWorldStreamingHUDPresenterComponent>(TEXT("WorldStreamingHUDPresenterComponent"));
	PlayerHealthCoordinatorComponent =
		CreateDefaultSubobject<UPlayerHealthCoordinatorComponent>(TEXT("PlayerHealthCoordinatorComponent"));
	MeteorStreamingComponent =
		CreateDefaultSubobject<UMeteorStreamingComponent>(TEXT("MeteorStreamingComponent"));
	BulkTransferScheduler = MakeShared<UE::ElementSandbox::NetBulk::FConnectionScheduler>();
	WorldChunkStreamingComponent->SetBulkTransferScheduler(BulkTransferScheduler);
	MeteorStreamingComponent->SetBulkTransferScheduler(BulkTransferScheduler);

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultMappingContext(
		TEXT("/Game/Input/IMC_Default.IMC_Default"));
	if (DefaultMappingContext.Succeeded())
	{
		DefaultMappingContexts.Add(DefaultMappingContext.Object);
	}

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookMappingContext(
		TEXT("/Game/Input/IMC_MouseLook.IMC_MouseLook"));
	if (MouseLookMappingContext.Succeeded())
	{
		DefaultMappingContexts.Add(MouseLookMappingContext.Object);
	}

	QuickbarActions.Reserve(UInventoryComponent::QuickbarSlotCount);
	for (int32 Index = 0; Index < UInventoryComponent::QuickbarSlotCount; ++Index)
	{
		const FString AssetName = FString::Printf(TEXT("IA_Quickbar%d"), Index + 1);
		const FString AssetPath = FString::Printf(TEXT("/Game/Input/Items/Actions/%s.%s"), *AssetName, *AssetName);
		ConstructorHelpers::FObjectFinder<UInputAction> QuickbarAction(*AssetPath);
		QuickbarActions.Add(QuickbarAction.Object);
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> ToggleInventoryInput(
		TEXT("/Game/Input/Items/Actions/IA_ToggleInventory.IA_ToggleInventory"));
	ToggleInventoryAction = ToggleInventoryInput.Object;

	static ConstructorHelpers::FObjectFinder<UInputAction> UseEquippedItemInput(
		TEXT("/Game/Input/Items/Actions/IA_UseEquippedItem.IA_UseEquippedItem"));
	UseEquippedItemAction = UseEquippedItemInput.Object;

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> ItemsMappingContext(
		TEXT("/Game/Input/Items/IMC_Items.IMC_Items"));
	ItemMappingContext = ItemsMappingContext.Object;
}

void AElementSandboxPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (IsLocalPlayerController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
				ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}
			if (ItemMappingContext)
			{
				Subsystem->AddMappingContext(ItemMappingContext, 1);
			}
		}
	}

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		for (int32 Index = 0; Index < QuickbarActions.Num(); ++Index)
		{
			if (QuickbarActions[Index])
			{
				EnhancedInputComponent->BindAction(QuickbarActions[Index], ETriggerEvent::Started, this,
												   &AElementSandboxPlayerController::HandleQuickbarSelection, Index);
			}
		}
		if (ToggleInventoryAction)
		{
			EnhancedInputComponent->BindAction(ToggleInventoryAction, ETriggerEvent::Started, this,
											   &AElementSandboxPlayerController::ToggleInventory);
		}
		if (UseEquippedItemAction)
		{
			EnhancedInputComponent->BindAction(UseEquippedItemAction, ETriggerEvent::Started, this,
											   &AElementSandboxPlayerController::PressUseEquippedItem);
			EnhancedInputComponent->BindAction(UseEquippedItemAction, ETriggerEvent::Completed, this,
											   &AElementSandboxPlayerController::ReleaseUseEquippedItem);
			EnhancedInputComponent->BindAction(UseEquippedItemAction, ETriggerEvent::Canceled, this,
											   &AElementSandboxPlayerController::ReleaseUseEquippedItem);
		}
	}
	if (BuildingPlacementComponent)
	{
		BuildingPlacementComponent->BindInput(*InputComponent);
	}

	// 当前没有为 Interact 制作数据资产；这里仍只把物理 E 映射到通用意图。
	InputComponent->BindKey(EKeys::E, IE_Pressed, this, &AElementSandboxPlayerController::PressInteract);
	InputComponent->BindKey(EKeys::E, IE_Released, this, &AElementSandboxPlayerController::ReleaseInteract);
	InputComponent->BindKey(EKeys::G, IE_Pressed, this, &AElementSandboxPlayerController::PressThrowWorldObject);
	InputComponent->BindKey(EKeys::R, IE_Pressed, this, &AElementSandboxPlayerController::HandleRotateOrRespawn);
}

void AElementSandboxPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (PlayerCameraManager)
	{
		PlayerCameraManager->ViewPitchMin = FMath::Min(ViewPitchMin, ViewPitchMax);
		PlayerCameraManager->ViewPitchMax = FMath::Max(ViewPitchMin, ViewPitchMax);
	}
	TryInitializeInventoryUI();
	if (PlayerHealthCoordinatorComponent)
	{
		PlayerHealthCoordinatorComponent->RefreshBinding();
	}
	if (WorldStreamingHUDPresenterComponent)
	{
		WorldStreamingHUDPresenterComponent->EnsureInitialized();
	}
	RefreshLocalGameplayInputMode();
}

void AElementSandboxPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	if (WorldChunkStreamingComponent)
	{
		WorldChunkStreamingComponent->NotifyPawnChanged();
	}
	if (PlayerHealthCoordinatorComponent)
	{
		PlayerHealthCoordinatorComponent->RefreshBinding();
	}
}

void AElementSandboxPlayerController::OnUnPossess()
{
	ReleaseInteract();
	Super::OnUnPossess();
	if (WorldChunkStreamingComponent)
	{
		WorldChunkStreamingComponent->NotifyPawnChanged();
	}
}

void AElementSandboxPlayerController::ApplyMouseLookInput(const FVector2D& LookInput)
{
	if (!IsLocalPlayerController() || IsLookInputIgnored())
	{
		return;
	}

	AddYawInput(LookInput.X * MouseYawScale);
	AddPitchInput(-LookInput.Y * MousePitchScale);
}

void AElementSandboxPlayerController::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	TryInitializeInventoryUI();
	if (PlayerHealthCoordinatorComponent)
	{
		PlayerHealthCoordinatorComponent->RefreshBinding();
	}
	RefreshLocalGameplayInputMode();
}

void AElementSandboxPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (InventoryHUD)
	{
		InventoryHUD->RemoveFromParent();
		InventoryHUD = nullptr;
	}
	if (bLocalUIOrDeathInputBlocked)
	{
		SetIgnoreMoveInput(false);
		SetIgnoreLookInput(false);
		bLocalUIOrDeathInputBlocked = false;
	}
	Super::EndPlay(EndPlayReason);
}

void AElementSandboxPlayerController::HandleQuickbarSelection(const int32 QuickbarIndex)
{
	if (IsHealthDepleted())
	{
		return;
	}

	if (UInventoryComponent* Inventory = GetInventoryComponent())
	{
		Inventory->SelectQuickbarSlot(QuickbarIndex);
	}
}

void AElementSandboxPlayerController::PressUseEquippedItem()
{
	// 背包打开时左键属于 UI 拖放，不应同时向 ASC 提交 Ability 输入。
	if (IsHealthDepleted() || (InventoryHUD && InventoryHUD->IsBackpackOpen()) ||
		(BuildingPlacementComponent && BuildingPlacementComponent->IsPlacementActive()))
	{
		return;
	}
	const bool bDemolitionToolSelected = IsDemolitionToolSelected();
	if (UElementAbilitySystemComponent* AbilitySystem = GetElementAbilitySystemComponent())
	{
		AbilitySystem->AbilityInputTagPressed(ElementSandboxGameplayTags::Input_Use_Primary);
	}
	if (bDemolitionToolSelected)
	{
		if (FocusHostComponent)
		{
			FocusHostComponent->HandlePrimaryUse();
		}
	}
}

void AElementSandboxPlayerController::ReleaseUseEquippedItem()
{
	if (UElementAbilitySystemComponent* AbilitySystem = GetElementAbilitySystemComponent())
	{
		AbilitySystem->AbilityInputTagReleased(ElementSandboxGameplayTags::Input_Use_Primary);
	}
}

void AElementSandboxPlayerController::PressInteract()
{
	if (!CanUseFocusInteraction())
	{
		return;
	}

	if (WorldObjectPickupComponent) WorldObjectPickupComponent->BeginInteract();
}

void AElementSandboxPlayerController::ReleaseInteract()
{
	if (WorldObjectPickupComponent) WorldObjectPickupComponent->EndInteract();
}

void AElementSandboxPlayerController::HandleRotateOrRespawn()
{
	if (BuildingPlacementComponent && BuildingPlacementComponent->TryRotateClockwise())
	{
		return;
	}
	RequestRespawn();
}

bool AElementSandboxPlayerController::CanUseFocusInteraction() const
{
	return IsLocalController() && !IsHealthDepleted() && (!InventoryHUD || !InventoryHUD->IsBackpackOpen()) &&
		   (!BuildingPlacementComponent || !BuildingPlacementComponent->IsPlacementActive());
}

bool AElementSandboxPlayerController::CanUseBuildingPlacement() const
{
	return IsLocalController() && !IsHealthDepleted() && (!InventoryHUD || !InventoryHUD->IsBackpackOpen()) &&
		   WorldChunkStreamingComponent && WorldChunkStreamingComponent->GetStreamingStats().bActivationCoreReady;
}

UInventoryComponent* AElementSandboxPlayerController::GetInventoryComponentForGameplay() const
{
	return GetInventoryComponent();
}

void AElementSandboxPlayerController::RequestBuildingPlacement(const int32 QuickbarIndex,
														   const FVector& SurfaceLocation,
														   const FVector& ExpectedResolvedLocation,
														   const uint8 YawQuarterTurns, const uint16 RequestId)
{
	if (!IsLocalController() || RequestId == 0
		|| SurfaceLocation.ContainsNaN()
		|| ExpectedResolvedLocation.ContainsNaN())
	{
		return;
	}
	if (HasAuthority())
	{
		const EBuildPlacementFailure Failure = TryPlaceBuilding(
			QuickbarIndex,
			SurfaceLocation,
			ExpectedResolvedLocation,
			YawQuarterTurns);
		if (BuildingPlacementComponent)
		{
			BuildingPlacementComponent->HandlePlacementResult(RequestId, Failure);
		}
		return;
	}
	ServerRequestBuildingPlacement(
		QuickbarIndex,
		SurfaceLocation,
		ExpectedResolvedLocation,
		YawQuarterTurns,
		RequestId);
}

void AElementSandboxPlayerController::ServerRequestBuildingPlacement_Implementation(
	const int32 QuickbarIndex,
	const FVector_NetQuantize10 SurfaceLocation,
	const FVector_NetQuantize10 ExpectedResolvedLocation,
	const uint8 YawQuarterTurns,
	const uint16 RequestId)
{
	const EBuildPlacementFailure Failure = RequestId == 0
											   ? EBuildPlacementFailure::InvalidTransform
											   : TryPlaceBuilding(
												   QuickbarIndex,
												   SurfaceLocation,
												   ExpectedResolvedLocation,
												   YawQuarterTurns);
	ClientBuildingPlacementResult(RequestId, Failure);
}

void AElementSandboxPlayerController::ClientBuildingPlacementResult_Implementation(const uint16 RequestId,
																				   const EBuildPlacementFailure Failure)
{
	if (BuildingPlacementComponent)
	{
		BuildingPlacementComponent->HandlePlacementResult(RequestId, Failure);
	}
}

EBuildPlacementFailure AElementSandboxPlayerController::TryPlaceBuilding(const int32 QuickbarIndex,
															 const FVector& SurfaceLocation,
															 const FVector& ExpectedResolvedLocation,
															 const uint8 YawQuarterTurns)
{
	UWorld* World = GetWorld();
	APawn* ControlledPawn = GetPawn();
	UInventoryComponent* Inventory = GetInventoryComponent();
	UBuildingWorldSubsystem* BuildingSubsystem = World ? World->GetSubsystem<UBuildingWorldSubsystem>() : nullptr;
	if (!HasAuthority() || !World || !IsValid(ControlledPawn) || !Inventory || !BuildingSubsystem ||
		!WorldChunkStreamingComponent || !WorldChunkStreamingComponent->GetStreamingStats().bActivationCoreReady)
	{
		return EBuildPlacementFailure::PlayerUnavailable;
	}

	const double RequestTime = World->GetTimeSeconds();
	const EBuildPlacementFailure GateFailure = FBuildingPlacementAuthorityService::TryBeginRequest(
		IsHealthDepleted(), bServerInventoryOpen, RequestTime, LastBuildingPlacementRequestTime);
	if (GateFailure != EBuildPlacementFailure::None)
	{
		return GateFailure;
	}

	UWorldChunkStreamingComponent* StreamingComponent = WorldChunkStreamingComponent;
	return FBuildingPlacementAuthorityService::TryPlace(
		*World,
		*BuildingSubsystem,
		*Inventory,
		*ControlledPawn,
		QuickbarIndex,
		SurfaceLocation,
		ExpectedResolvedLocation,
		YawQuarterTurns,
		[StreamingComponent](const FVector& ResolvedLocation)
		{
			return StreamingComponent->IsAuthorityChunkReadyForLiveMutation(
				FWorldChunkCoord::FromWorldLocation(ResolvedLocation));
		});
}

void AElementSandboxPlayerController::PressThrowWorldObject()
{
	if (IsHealthDepleted() || (InventoryHUD && InventoryHUD->IsBackpackOpen()))
	{
		return;
	}

	if (HasAuthority())
	{
		TryThrowSelectedWorldObject();
	}
	else
	{
		ServerThrowSelectedWorldObject();
	}
}

void AElementSandboxPlayerController::ServerThrowSelectedWorldObject_Implementation() { TryThrowSelectedWorldObject(); }

bool AElementSandboxPlayerController::TryThrowSelectedWorldObject()
{
	AElementSandboxCharacter* SandboxCharacter = Cast<AElementSandboxCharacter>(GetPawn());
	UInventoryComponent* Inventory = GetInventoryComponent();
	UWorldObjectEquipmentBridgeComponent* Bridge =
		SandboxCharacter ? SandboxCharacter->GetWorldObjectEquipmentBridgeComponent() : nullptr;
	return HasAuthority() && !IsHealthDepleted() && SandboxCharacter && Inventory && Bridge &&
		   Bridge->ThrowSelectedStick(*Inventory, GetControlRotation().Vector(), StickThrowForwardSpeed,
									  StickThrowUpwardSpeed);
}

bool AElementSandboxPlayerController::RequestPickupWorldObject(const FWorldEntityId WorldEntityId)
{
	if (!CanUseFocusInteraction() || !WorldEntityId.IsSet() || !WorldObjectPickupComponent
		|| !WorldObjectPickupComponent->TryBeginRequest(WorldEntityId))
	{
		return false;
	}
	if (HasAuthority())
	{
		WorldObjectPickupComponent->CompleteRequest(WorldEntityId, TryPickupWorldObject(WorldEntityId));
	}
	else
	{
		ServerPickupWorldObject(WorldEntityId);
	}
	return true;
}

bool AElementSandboxPlayerController::RequestDoorInteraction(const FWorldEntityId WorldEntityId)
{
	if (!IsLocalController() || !WorldEntityId.IsSet() || !CanUseFocusInteraction())
	{
		return false;
	}
	if (HasAuthority())
	{
		return TryInteractWithDoor(WorldEntityId);
	}

	ServerRequestDoorInteraction(WorldEntityId);
	return true;
}

bool AElementSandboxPlayerController::RequestBuildingDismantle(
	const FWorldEntityId WorldEntityId)
{
	if (!IsLocalController() || !WorldEntityId.IsSet()
		|| !CanUseFocusInteraction() || !IsDemolitionToolSelected())
	{
		return false;
	}
	if (HasAuthority())
	{
		return TryDismantleBuilding(WorldEntityId);
	}

	ServerRequestBuildingDismantle(WorldEntityId);
	return true;
}

bool AElementSandboxPlayerController::IsDemolitionToolSelected() const
{
	UInventoryComponent* Inventory = GetInventoryComponent();
	const int32 SelectedIndex = Inventory
		? Inventory->GetSelectedQuickbarIndex()
		: INDEX_NONE;
	const UItemInstance* Item = Inventory
		? Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Quickbar, SelectedIndex))
		: nullptr;
	return Item && Item->FindFeature<UDemolitionToolItemFeature>();
}

bool AElementSandboxPlayerController::CanDismantleBuildingDefinition(
	const FName BuildingDefinitionId) const
{
	UItemDefinition* IgnoredItem = nullptr;
	int32 IgnoredQuantity = 0;
	return TryResolveSelectedDismantleReward(
		BuildingDefinitionId,
		IgnoredItem,
		IgnoredQuantity);
}

bool AElementSandboxPlayerController::TryResolveSelectedDismantleReward(
	const FName BuildingDefinitionId,
	UItemDefinition*& OutItemDefinition,
	int32& OutQuantity) const
{
	OutItemDefinition = nullptr;
	OutQuantity = 0;
	UInventoryComponent* Inventory = GetInventoryComponent();
	const int32 SelectedIndex = Inventory
		? Inventory->GetSelectedQuickbarIndex()
		: INDEX_NONE;
	const UItemInstance* Item = Inventory
		? Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Quickbar, SelectedIndex))
		: nullptr;
	const UDemolitionToolItemFeature* Tool = Item
		? Item->FindFeature<UDemolitionToolItemFeature>()
		: nullptr;
	return Tool && Tool->TryResolveReward(
		BuildingDefinitionId,
		OutItemDefinition,
		OutQuantity);
}

void AElementSandboxPlayerController::ServerRequestDoorInteraction_Implementation(const FWorldEntityId WorldEntityId)
{
	TryInteractWithDoor(WorldEntityId);
}

void AElementSandboxPlayerController::ServerRequestBuildingDismantle_Implementation(
	const FWorldEntityId WorldEntityId)
{
	TryDismantleBuilding(WorldEntityId);
}

bool AElementSandboxPlayerController::TryInteractWithDoor(const FWorldEntityId WorldEntityId)
{
	UWorld* World = GetWorld();
	AElementSandboxCharacter* ControlledCharacter = Cast<AElementSandboxCharacter>(GetPawn());
	UBuildingWorldSubsystem* Building = World ? World->GetSubsystem<UBuildingWorldSubsystem>() : nullptr;
	UBuildingCatalogWorldSubsystem* Catalog = World ? World->GetSubsystem<UBuildingCatalogWorldSubsystem>() : nullptr;
	if (!HasAuthority() || !World || !WorldEntityId.IsSet() || IsHealthDepleted() || bServerInventoryOpen ||
		!IsValid(ControlledCharacter) || !Building || !Catalog)
	{
		return false;
	}

	const FBuildEntityHandle Entity = Building->FindEntity(WorldEntityId);
	FBox DoorBounds(ForceInit);
	const double FocusDistance = ControlledCharacter->GetFocusDistance();
	if (!Building->IsEntityAlive(Entity) || FocusDistance <= 0.0 ||
		!Building->GetSpatialIndex().TryGetBounds(Entity, DoorBounds) ||
		DoorBounds.ComputeSquaredDistanceToPoint(ControlledCharacter->GetActorLocation()) >=
			FMath::Square(FocusDistance))
	{
		return false;
	}

	return Catalog->RequestDoorInteraction(Entity);
}

bool AElementSandboxPlayerController::TryDismantleBuilding(
	const FWorldEntityId WorldEntityId)
{
	UWorld* World = GetWorld();
	AElementSandboxCharacter* ControlledCharacter =
		Cast<AElementSandboxCharacter>(GetPawn());
	UInventoryComponent* Inventory = GetInventoryComponent();
	UBuildingWorldSubsystem* Building = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!HasAuthority() || !World || !WorldEntityId.IsSet()
		|| !ControlledCharacter || !Inventory || !Building)
	{
		return false;
	}

	const EBuildingDismantleFailure GateFailure =
		FBuildingDismantleAuthorityService::TryBeginRequest(
			IsHealthDepleted(),
			bServerInventoryOpen,
			World->GetTimeSeconds(),
			LastBuildingDismantleRequestTime);
	return GateFailure == EBuildingDismantleFailure::None
		&& FBuildingDismantleAuthorityService::TryDismantle(
			*Building,
			*Inventory,
			*ControlledCharacter,
			WorldEntityId,
			ControlledCharacter->GetFocusDistance())
			== EBuildingDismantleFailure::None;
}

void AElementSandboxPlayerController::ServerPickupWorldObject_Implementation(const FWorldEntityId WorldEntityId)
{
	ClientWorldObjectPickupResult(WorldEntityId, TryPickupWorldObject(WorldEntityId));
}

void AElementSandboxPlayerController::ClientWorldObjectPickupResult_Implementation(
	const FWorldEntityId WorldEntityId, const EWorldObjectPickupFailure Failure)
{
	if (WorldObjectPickupComponent) WorldObjectPickupComponent->CompleteRequest(WorldEntityId, Failure);
}

EWorldObjectPickupFailure AElementSandboxPlayerController::TryPickupWorldObject(const FWorldEntityId WorldEntityId)
{
	UWorld* World = GetWorld();
	AElementSandboxCharacter* ControlledCharacter = Cast<AElementSandboxCharacter>(GetPawn());
	UInventoryComponent* Inventory = GetInventoryComponent();
	UWorldObjectWorldSubsystem* WorldObjects = World ? World->GetSubsystem<UWorldObjectWorldSubsystem>() : nullptr;
	UWorldObjectItemCatalogSubsystem* Catalog =
		World ? World->GetSubsystem<UWorldObjectItemCatalogSubsystem>() : nullptr;
	if (!HasAuthority() || IsHealthDepleted() || bServerInventoryOpen
		|| !IsValid(ControlledCharacter) || !Inventory || !WorldObjects || !Catalog)
	{
		return EWorldObjectPickupFailure::PlayerUnavailable;
	}
	return FWorldObjectPickupAuthorityService::TryPickup(*WorldObjects, *Catalog, *Inventory,
		*ControlledCharacter, WorldEntityId, ControlledCharacter->GetFocusDistance());
}

void AElementSandboxPlayerController::ToggleInventory()
{
	if (IsHealthDepleted())
	{
		return;
	}

	TryInitializeInventoryUI();
	if (!InventoryHUD)
	{
		return;
	}
	SetInventoryOpen(!InventoryHUD->IsBackpackOpen());
}

void AElementSandboxPlayerController::SetInventoryOpen(const bool bOpen)
{
	if (!InventoryHUD || (bOpen && IsHealthDepleted()))
	{
		return;
	}

	if (bOpen)
	{
		ReleaseUseEquippedItem();
		ReleaseInteract();
	}
	InventoryHUD->SetBackpackOpen(bOpen);
	if (HasAuthority())
	{
		bServerInventoryOpen = bOpen;
	}
	else
	{
		ServerSetInventoryOpen(bOpen);
	}
	RefreshLocalGameplayInputMode();
}

void AElementSandboxPlayerController::RefreshLocalGameplayInputMode()
{
	if (!IsLocalPlayerController())
	{
		return;
	}

	const bool bInventoryOpen = InventoryHUD && InventoryHUD->IsBackpackOpen();
	const bool bShouldBlockGameplayInput =
		bInventoryOpen ||
		(PlayerHealthCoordinatorComponent && PlayerHealthCoordinatorComponent->IsLocalDeathPresentationActive());
	if (bLocalUIOrDeathInputBlocked != bShouldBlockGameplayInput)
	{
		SetIgnoreMoveInput(bShouldBlockGameplayInput);
		SetIgnoreLookInput(bShouldBlockGameplayInput);
		bLocalUIOrDeathInputBlocked = bShouldBlockGameplayInput;
	}

	bShowMouseCursor = bInventoryOpen;
	bEnableClickEvents = bInventoryOpen;
	bEnableMouseOverEvents = bInventoryOpen;
	if (bInventoryOpen)
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(InventoryHUD->TakeWidget());
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		SetInputMode(InputMode);
	}
	else
	{
		FInputModeGameOnly InputMode;
		InputMode.SetConsumeCaptureMouseDown(false);
		SetInputMode(InputMode);
	}

	UE_LOG(LogTemp, Display, TEXT("本地输入模式：%s；Cursor=%d MoveIgnored=%d LookIgnored=%d。"),
		   bInventoryOpen ? TEXT("GameAndUI") : TEXT("GameOnly"), bShowMouseCursor, IsMoveInputIgnored(),
		   IsLookInputIgnored());
}

void AElementSandboxPlayerController::ServerSetInventoryOpen_Implementation(const bool bOpen)
{
	bServerInventoryOpen = bOpen;
}

void AElementSandboxPlayerController::TryInitializeInventoryUI()
{
	if (!IsLocalPlayerController())
	{
		return;
	}

	UInventoryComponent* Inventory = GetInventoryComponent();
	if (!Inventory)
	{
		return;
	}

	if (!InventoryHUD)
	{
		InventoryHUD = CreateWidget<UInventoryHUDWidget>(this, UInventoryHUDWidget::StaticClass());
		if (InventoryHUD)
		{
			InventoryHUD->AddToViewport();
		}
	}

	if (InventoryHUD)
	{
		InventoryHUD->SetInventory(Inventory);
	}
}

void AElementSandboxPlayerController::RequestRespawn()
{
	if (PlayerHealthCoordinatorComponent)
	{
		PlayerHealthCoordinatorComponent->RequestRespawn();
	}
}

void AElementSandboxPlayerController::ServerRequestRespawn_Implementation()
{
	if (PlayerHealthCoordinatorComponent)
	{
		PlayerHealthCoordinatorComponent->TryRespawnAtPlayerStart();
	}
}

UInventoryComponent* AElementSandboxPlayerController::GetInventoryComponent() const
{
	const AElementSandboxPlayerState* SandboxPlayerState = GetPlayerState<AElementSandboxPlayerState>();
	return SandboxPlayerState ? SandboxPlayerState->GetInventoryComponent() : nullptr;
}

UElementAbilitySystemComponent* AElementSandboxPlayerController::GetElementAbilitySystemComponent() const
{
	const AElementSandboxPlayerState* SandboxPlayerState = GetPlayerState<AElementSandboxPlayerState>();
	return SandboxPlayerState ? SandboxPlayerState->GetElementAbilitySystemComponent() : nullptr;
}

bool AElementSandboxPlayerController::IsHealthDepleted() const
{
	return PlayerHealthCoordinatorComponent && PlayerHealthCoordinatorComponent->IsHealthDepleted();
}
