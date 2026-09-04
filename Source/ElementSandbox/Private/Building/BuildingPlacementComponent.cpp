#include "Building/BuildingPlacementComponent.h"

#include "Building/BuildPlacementPreviewActor.h"
#include "Building/BuildingItemFeature.h"
#include "Building/BuildingPlacementResolver.h"
#include "BuildingWorldSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Definition/BuildingDefinition.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Focus/FocusHostComponent.h"
#include "Game/ElementSandboxPlayerController.h"
#include "GameFramework/Pawn.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryTypes.h"
#include "Interaction/InteractionPromptWidget.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemInstance.h"
#include "Spatial/BuildSpatialIndex.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr double PlacementViewTraceDistance = 1500.0;
	constexpr double AuthorityFailureDisplaySeconds = 1.5;
	constexpr int32 PlacementInputPriority = 10;

	FText DescribePlacementState(const EBuildPlacementFailure Failure)
	{
		switch (Failure)
		{
		case EBuildPlacementFailure::None:
			return NSLOCTEXT(
				"BuildingPlacement",
				"PlacementAllowed",
				"蓝色：可以放置");
		case EBuildPlacementFailure::NoSurface:
			return NSLOCTEXT(
				"BuildingPlacement",
				"NoSurface",
				"红色：准星下没有可用地面");
		case EBuildPlacementFailure::SurfaceTooSteep:
			return NSLOCTEXT(
				"BuildingPlacement",
				"SurfaceTooSteep",
				"红色：地面坡度超过 45°");
		case EBuildPlacementFailure::OutOfRange:
			return NSLOCTEXT(
				"BuildingPlacement",
				"OutOfRange",
				"红色：超过 500cm 建造距离");
		case EBuildPlacementFailure::BlockedByBuilding:
			return NSLOCTEXT(
				"BuildingPlacement",
				"BlockedByBuilding",
				"红色：与已有建筑重叠");
		case EBuildPlacementFailure::BlockedByWorld:
			return NSLOCTEXT(
				"BuildingPlacement",
				"BlockedByWorld",
				"红色：被角色或场景物体阻挡");
		case EBuildPlacementFailure::StreamingNotReady:
			return NSLOCTEXT(
				"BuildingPlacement",
				"StreamingNotReady",
				"红色：该区域正在后台加载");
		case EBuildPlacementFailure::NoBuildItem:
			return NSLOCTEXT(
				"BuildingPlacement",
				"NoBuildItem",
				"红色：当前槽位不是建造物品");
		case EBuildPlacementFailure::MissingDefinition:
			return NSLOCTEXT(
				"BuildingPlacement",
				"MissingDefinition",
				"红色：建筑配置不存在");
		case EBuildPlacementFailure::InventoryChanged:
			return NSLOCTEXT(
				"BuildingPlacement",
				"InventoryChanged",
				"红色：物品数量已经变化");
		case EBuildPlacementFailure::PlayerUnavailable:
			return NSLOCTEXT(
				"BuildingPlacement",
				"PlayerUnavailable",
				"红色：当前角色不能建造");
		case EBuildPlacementFailure::InventoryOpen:
			return NSLOCTEXT(
				"BuildingPlacement",
				"InventoryOpen",
				"红色：请先关闭背包");
		case EBuildPlacementFailure::RateLimited:
			return NSLOCTEXT(
				"BuildingPlacement",
				"RateLimited",
				"红色：放置操作过快");
		case EBuildPlacementFailure::CreateFailed:
			return NSLOCTEXT(
				"BuildingPlacement",
				"CreateFailed",
				"红色：服务器创建建筑失败");
		case EBuildPlacementFailure::InvalidTransform:
		default:
			return NSLOCTEXT(
				"BuildingPlacement",
				"InvalidTransform",
				"红色：摆放位置无效");
		}
	}
}

UBuildingPlacementComponent::UBuildingPlacementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MappingFinder(
		TEXT("/Game/Input/Building/IMC_Building.IMC_Building"));
	PlacementMappingContext = MappingFinder.Object;
	static ConstructorHelpers::FObjectFinder<UInputAction> ConfirmFinder(
		TEXT("/Game/Input/Building/Actions/IA_BuildConfirm.IA_BuildConfirm"));
	ConfirmAction = ConfirmFinder.Object;
	static ConstructorHelpers::FObjectFinder<UInputAction> CancelFinder(
		TEXT("/Game/Input/Building/Actions/IA_BuildCancel.IA_BuildCancel"));
	CancelAction = CancelFinder.Object;
	static ConstructorHelpers::FObjectFinder<UInputAction> RotateFinder(
		TEXT("/Game/Input/Building/Actions/IA_BuildRotate.IA_BuildRotate"));
	RotateAction = RotateFinder.Object;
}

void UBuildingPlacementComponent::BindInput(UInputComponent& InputComponent)
{
	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(&InputComponent);
	if (!EnhancedInput)
	{
		return;
	}
	if (ConfirmAction)
	{
		EnhancedInput->BindAction(
			ConfirmAction,
			ETriggerEvent::Started,
			this,
			&UBuildingPlacementComponent::HandleConfirm);
	}
	if (CancelAction)
	{
		EnhancedInput->BindAction(
			CancelAction,
			ETriggerEvent::Started,
			this,
			&UBuildingPlacementComponent::HandleCancel);
	}
	if (RotateAction)
	{
		EnhancedInput->BindAction(
			RotateAction,
			ETriggerEvent::Triggered,
			this,
			&UBuildingPlacementComponent::HandleRotate);
	}
}

void UBuildingPlacementComponent::HandlePlacementResult(
	const uint16 RequestId,
	const EBuildPlacementFailure Failure)
{
	if (RequestId == 0 || RequestId != PendingRequestId)
	{
		return;
	}
	PendingRequestId = 0;
	if (Failure == EBuildPlacementFailure::None)
	{
		LastAuthorityFailure = EBuildPlacementFailure::None;
		LastAuthorityFailureExpiryTime = -DBL_MAX;
	}
	else
	{
		LastAuthorityFailure = Failure;
		LastAuthorityFailureExpiryTime = GetWorld()
			? GetWorld()->GetTimeSeconds() + AuthorityFailureDisplaySeconds
			: AuthorityFailureDisplaySeconds;
	}
	bPlacementPromptSnapshotValid = false;
	RefreshPlacementMode();
	RefreshPlacementPrompt();
}

void UBuildingPlacementComponent::BeginPlay()
{
	Super::BeginPlay();
	const AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	const bool bCanRunLocally = PlayerController
		&& PlayerController->IsLocalController()
		&& GetNetMode() != NM_DedicatedServer
		&& !IsRunningCommandlet();
	SetComponentTickEnabled(bCanRunLocally);
	if (!bCanRunLocally)
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = GetOwner();
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	PreviewActor = GetWorld()->SpawnActor<ABuildPlacementPreviewActor>(SpawnParameters);
	EnsurePlacementPromptWidget();
	TryBindInventory();
	RefreshPlacementMode();
}

void UBuildingPlacementComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	ExitPlacementMode();
	UnbindInventory();
	if (IsValid(PreviewActor))
	{
		PreviewActor->Destroy();
	}
	PreviewActor = nullptr;
	if (PlacementPromptWidget)
	{
		PlacementPromptWidget->RemoveFromParent();
	}
	PlacementPromptWidget = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UBuildingPlacementComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	TryBindInventory();
	RefreshPlacementMode();
	if (bPlacementActive)
	{
		RefreshPreview();
	}
}

void UBuildingPlacementComponent::TryBindInventory()
{
	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	UInventoryComponent* Inventory = PlayerController
		? PlayerController->GetInventoryComponentForGameplay()
		: nullptr;
	if (BoundInventory.Get() == Inventory)
	{
		return;
	}
	UnbindInventory();
	if (Inventory)
	{
		BoundInventory = Inventory;
		InventoryChangedHandle = Inventory->OnInventoryChanged().AddUObject(
			this,
			&UBuildingPlacementComponent::HandleInventoryChanged);
	}
}

void UBuildingPlacementComponent::UnbindInventory()
{
	if (UInventoryComponent* Inventory = BoundInventory.Get())
	{
		if (InventoryChangedHandle.IsValid())
		{
			Inventory->OnInventoryChanged().Remove(InventoryChangedHandle);
		}
	}
	InventoryChangedHandle.Reset();
	BoundInventory.Reset();
}

void UBuildingPlacementComponent::HandleInventoryChanged()
{
	RefreshPlacementMode();
}

void UBuildingPlacementComponent::RefreshPlacementMode()
{
	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	if (CancelledItem.IsValid())
	{
		UInventoryComponent* Inventory = BoundInventory.Get();
		const int32 SelectedIndex = Inventory
			? Inventory->GetSelectedQuickbarIndex()
			: INDEX_NONE;
		UItemInstance* SelectedItem = Inventory
			? Inventory->GetItem(FInventorySlotAddress(
				EInventoryContainer::Quickbar,
				SelectedIndex))
			: nullptr;
		if (SelectedItem != CancelledItem.Get())
		{
			CancelledItem.Reset();
		}
	}
	UItemInstance* Item = nullptr;
	UBuildingDefinition* Definition = nullptr;
	FTransform PlacementShapeTransform = FTransform::Identity;
	int32 QuickbarIndex = INDEX_NONE;
	const bool bCanEnter = PlayerController
		&& PlayerController->CanUseBuildingPlacement()
		&& TryGetSelectedBuildItem(
			Item,
			QuickbarIndex,
			Definition,
			PlacementShapeTransform)
		&& CancelledItem.Get() != Item;
	if (!bCanEnter)
	{
		ExitPlacementMode();
		return;
	}

	if (!bPlacementActive
		|| ActiveItem.Get() != Item
		|| ActiveDefinition.Get() != Definition
		|| ActiveQuickbarIndex != QuickbarIndex
		|| !ActivePlacementShapeTransform.Equals(PlacementShapeTransform))
	{
		EnterPlacementMode(
			*Item,
			QuickbarIndex,
			*Definition,
			PlacementShapeTransform);
	}
}

void UBuildingPlacementComponent::EnterPlacementMode(
	UItemInstance& Item,
	const int32 QuickbarIndex,
	UBuildingDefinition& Definition,
	const FTransform& PlacementShapeTransform)
{
	if (CancelledItem.Get() != &Item)
	{
		CancelledItem.Reset();
	}
	ActiveItem = &Item;
	ActiveDefinition = &Definition;
	ActivePlacementShapeTransform = PlacementShapeTransform;
	ActiveQuickbarIndex = QuickbarIndex;
	YawQuarterTurns = 0;
	PendingRequestId = 0;
	LastAuthorityFailure = EBuildPlacementFailure::None;
	LastAuthorityFailureExpiryTime = -DBL_MAX;
	bPlacementActive = IsValid(PreviewActor)
		&& PreviewActor->SetDefinition(&Definition);
	SetPlacementInputEnabled(bPlacementActive);
	SuspendFocus(bPlacementActive);
	RefreshPlacementPrompt();
	if (!bPlacementActive)
	{
		ActiveItem.Reset();
		ActiveDefinition.Reset();
		ActivePlacementShapeTransform = FTransform::Identity;
		ActiveQuickbarIndex = INDEX_NONE;
	}
}

void UBuildingPlacementComponent::ExitPlacementMode()
{
	if (PreviewActor)
	{
		PreviewActor->SetPreviewVisible(false);
	}
	SetPlacementInputEnabled(false);
	SuspendFocus(false);
	bPlacementActive = false;
	ActiveItem.Reset();
	ActiveDefinition.Reset();
	ActivePlacementShapeTransform = FTransform::Identity;
	ActiveQuickbarIndex = INDEX_NONE;
	PendingRequestId = 0;
	LastAuthorityFailure = EBuildPlacementFailure::None;
	LastAuthorityFailureExpiryTime = -DBL_MAX;
	CurrentEvaluation = {};
	CurrentExpectedLocation = FVector::ZeroVector;
	bHasCurrentExpectedLocation = false;
	bHasCachedEvaluation = false;
	bPlacementPromptSnapshotValid = false;
	PromptedItem.Reset();
	HidePlacementPrompt();
}

void UBuildingPlacementComponent::RefreshPreview()
{
	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	UBuildingDefinition* Definition = ActiveDefinition.Get();
	APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	UWorld* World = GetWorld();
	UBuildingWorldSubsystem* BuildingSubsystem = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	FVector ExpectedLocation = FVector::ZeroVector;
	if (!PlayerController || !Definition || !Pawn || !BuildingSubsystem
		|| !TryFindViewSurface(*BuildingSubsystem, ExpectedLocation))
	{
		CurrentEvaluation = {};
		CurrentEvaluation.Failure = EBuildPlacementFailure::NoSurface;
		bHasCurrentExpectedLocation = false;
		bHasCachedEvaluation = false;
		if (PreviewActor)
		{
			PreviewActor->SetPreviewVisible(false);
		}
		RefreshPlacementPrompt();
		return;
	}
	bHasCurrentExpectedLocation = true;
	CurrentExpectedLocation = ExpectedLocation;

	FBuildPlacementEvaluation CandidateEvaluation;
	const bool bResolvedCandidate = FBuildingPlacementResolver::ResolveCandidateTransform(
		*World,
		*BuildingSubsystem,
		*Definition,
		ExpectedLocation,
		ActivePlacementShapeTransform,
		YawQuarterTurns,
		Pawn,
		CandidateEvaluation);
	if (!bResolvedCandidate || !CandidateEvaluation.IsAllowed())
	{
		CurrentEvaluation = CandidateEvaluation;
		bHasCachedEvaluation = false;
	}
	else
	{
		const uint64 SpatialRevision =
			BuildingSubsystem->GetSpatialIndex().GetQueryRevision();
		constexpr double DynamicWorldCacheLifetimeSeconds = 0.05;
		const double CurrentTime = World->GetTimeSeconds();
		const bool bCanReuse = bHasCachedEvaluation
			&& CachedDefinition.Get() == Definition
			&& CachedYawQuarterTurns == YawQuarterTurns
			&& CachedSpatialRevision == SpatialRevision
			&& CachedCandidateTransform.Equals(
				CandidateEvaluation.ResolvedTransform,
				0.01)
			&& CurrentTime - CachedEvaluationTime
				<= DynamicWorldCacheLifetimeSeconds;
		if (bCanReuse)
		{
			CurrentEvaluation = CachedEvaluation;
		}
		else
		{
			BuildingSubsystem->EvaluatePlacement(
				*Definition,
				CandidateEvaluation.ResolvedTransform,
				Pawn->GetActorLocation(),
				FBuildingPlacementResolver::MaximumDistance,
				CurrentEvaluation);
			CachedEvaluation = CurrentEvaluation;
			CachedCandidateTransform = CandidateEvaluation.ResolvedTransform;
			CachedDefinition = Definition;
			CachedSpatialRevision = SpatialRevision;
			CachedEvaluationTime = CurrentTime;
			CachedYawQuarterTurns = YawQuarterTurns;
			bHasCachedEvaluation = true;
		}
	}
	if (PreviewActor)
	{
		PreviewActor->SetPlacementState(
			CurrentEvaluation.ResolvedTransform,
			CurrentEvaluation.IsAllowed());
		PreviewActor->SetPreviewVisible(
			CurrentEvaluation.Failure != EBuildPlacementFailure::NoSurface);
	}
	RefreshPlacementPrompt();
}

void UBuildingPlacementComponent::SetPlacementInputEnabled(const bool bEnabled)
{
	if (bPlacementInputEnabled == bEnabled)
	{
		return;
	}
	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = PlayerController
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(
			PlayerController->GetLocalPlayer())
		: nullptr;
	if (!InputSubsystem || !PlacementMappingContext)
	{
		bPlacementInputEnabled = false;
		return;
	}
	if (bEnabled)
	{
		InputSubsystem->AddMappingContext(
			PlacementMappingContext,
			PlacementInputPriority);
	}
	else
	{
		InputSubsystem->RemoveMappingContext(PlacementMappingContext);
	}
	bPlacementInputEnabled = bEnabled;
}

void UBuildingPlacementComponent::SuspendFocus(const bool bSuspend)
{
	if (UFocusHostComponent* FocusHost = GetOwner()
		? GetOwner()->FindComponentByClass<UFocusHostComponent>()
		: nullptr)
	{
		if (bSuspend)
		{
			FocusHost->ClearFocus();
		}
		FocusHost->SetComponentTickEnabled(!bSuspend);
	}
}

bool UBuildingPlacementComponent::TryRotateClockwise()
{
	if (!bPlacementActive)
	{
		return false;
	}
	ApplyRotationStep(1);
	return true;
}

void UBuildingPlacementComponent::ApplyRotationStep(const int32 Direction)
{
	if (!bPlacementActive || Direction == 0)
	{
		return;
	}
	YawQuarterTurns = static_cast<uint8>(
		(static_cast<int32>(YawQuarterTurns) + FMath::Sign(Direction) + 4) % 4);
	bHasCachedEvaluation = false;
	bPlacementPromptSnapshotValid = false;
	RefreshPreview();
}

bool UBuildingPlacementComponent::EnsurePlacementPromptWidget()
{
	if (PlacementPromptWidget)
	{
		return true;
	}

	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	if (!PlayerController || !PlayerController->GetLocalPlayer())
	{
		return false;
	}

	PlacementPromptWidget = CreateWidget<UInteractionPromptWidget>(
		PlayerController,
		UInteractionPromptWidget::StaticClass());
	if (!PlacementPromptWidget)
	{
		return false;
	}

	PlacementPromptWidget->SetAlignmentInViewport(FVector2D(0.5f, 1.0f));
	PlacementPromptWidget->SetVisibility(ESlateVisibility::Collapsed);
	PlacementPromptWidget->AddToViewport(25);
	return true;
}

void UBuildingPlacementComponent::RefreshPlacementPrompt()
{
	if (!bPlacementActive || !EnsurePlacementPromptWidget())
	{
		HidePlacementPrompt();
		return;
	}
	const FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(this);
	PlacementPromptWidget->SetPositionInViewport(
		FVector2D(ViewportSize.X * 0.5f, ViewportSize.Y - 92.0f),
		true);
	EBuildPlacementFailure DisplayFailure = CurrentEvaluation.Failure;
	const UWorld* World = GetWorld();
	if (LastAuthorityFailure != EBuildPlacementFailure::None
		&& World
		&& World->GetTimeSeconds() <= LastAuthorityFailureExpiryTime)
	{
		DisplayFailure = LastAuthorityFailure;
	}
	else if (LastAuthorityFailure != EBuildPlacementFailure::None)
	{
		LastAuthorityFailure = EBuildPlacementFailure::None;
		LastAuthorityFailureExpiryTime = -DBL_MAX;
	}

	if (bPlacementPromptSnapshotValid
		&& PromptedItem.Get() == ActiveItem.Get()
		&& PromptedYawQuarterTurns == YawQuarterTurns
		&& PromptedFailure == DisplayFailure)
	{
		PlacementPromptWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
		return;
	}

	FText ItemName = ActiveDefinition.IsValid()
		? FText::FromName(ActiveDefinition->DefinitionId)
		: NSLOCTEXT("BuildingPlacement", "UnknownBuilding", "未知建筑");
	if (const UItemDisplayFeature* Display = ActiveItem.IsValid()
		? ActiveItem->FindFeature<UItemDisplayFeature>()
		: nullptr;
		Display && !Display->DisplayName.IsEmpty())
	{
		ItemName = Display->DisplayName;
	}

	FFormatNamedArguments Arguments;
	Arguments.Add(TEXT("Item"), ItemName);
	Arguments.Add(
		TEXT("Yaw"),
		FText::AsNumber(static_cast<int32>(YawQuarterTurns % 4) * 90));
	Arguments.Add(TEXT("State"), DescribePlacementState(DisplayFailure));
	PlacementPromptWidget->SetPromptText(FText::Format(
		NSLOCTEXT(
			"BuildingPlacement",
			"PlacementControls",
			"{Item}  |  朝向 {Yaw}°\n{State}\n左键：放置    R / 滚轮：旋转 90°    右键：取消"),
		Arguments));
	PlacementPromptWidget->SetVisibility(ESlateVisibility::HitTestInvisible);

	PromptedItem = ActiveItem;
	PromptedYawQuarterTurns = YawQuarterTurns;
	PromptedFailure = DisplayFailure;
	bPlacementPromptSnapshotValid = true;
}

void UBuildingPlacementComponent::HidePlacementPrompt()
{
	if (PlacementPromptWidget)
	{
		PlacementPromptWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UBuildingPlacementComponent::HandleConfirm()
{
	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	if (!bPlacementActive
		|| !PlayerController
		|| PendingRequestId != 0
		|| !CurrentEvaluation.IsAllowed()
		|| !bHasCurrentExpectedLocation
		|| !ActiveDefinition.IsValid()
		|| ActiveQuickbarIndex == INDEX_NONE)
	{
		return;
	}

	uint16 RequestId = NextRequestId++;
	if (RequestId == 0)
	{
		RequestId = NextRequestId++;
	}
	PendingRequestId = RequestId;
	LastAuthorityFailure = EBuildPlacementFailure::None;
	LastAuthorityFailureExpiryTime = -DBL_MAX;
	bPlacementPromptSnapshotValid = false;
	PlayerController->RequestBuildingPlacement(
		ActiveQuickbarIndex,
		CurrentExpectedLocation,
		CurrentEvaluation.ResolvedTransform.GetLocation(),
		YawQuarterTurns,
		RequestId);
}

void UBuildingPlacementComponent::HandleCancel()
{
	if (!bPlacementActive)
	{
		return;
	}
	CancelledItem = ActiveItem;
	ExitPlacementMode();
}

void UBuildingPlacementComponent::HandleRotate(const FInputActionValue& Value)
{
	if (!bPlacementActive)
	{
		return;
	}
	const float Axis = Value.Get<float>();
	if (FMath::IsNearlyZero(Axis))
	{
		return;
	}
	const int32 Direction = Axis > 0.0f ? 1 : -1;
	ApplyRotationStep(Direction);
}

bool UBuildingPlacementComponent::TryGetSelectedBuildItem(
	UItemInstance*& OutItem,
	int32& OutQuickbarIndex,
	UBuildingDefinition*& OutDefinition,
	FTransform& OutPlacementShapeTransform) const
{
	OutItem = nullptr;
	OutQuickbarIndex = INDEX_NONE;
	OutDefinition = nullptr;
	OutPlacementShapeTransform = FTransform::Identity;
	UInventoryComponent* Inventory = BoundInventory.Get();
	UWorld* World = GetWorld();
	UBuildingWorldSubsystem* BuildingSubsystem = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!Inventory || !BuildingSubsystem)
	{
		return false;
	}

	const int32 SelectedIndex = Inventory->GetSelectedQuickbarIndex();
	UItemInstance* Item = Inventory->GetItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, SelectedIndex));
	const UBuildingItemFeature* Feature = Item
		? Item->FindFeature<UBuildingItemFeature>()
		: nullptr;
	const UItemStackFeature* Stack = Item
		? Item->FindFeature<UItemStackFeature>()
		: nullptr;
	UBuildingDefinition* Definition = Feature
		? BuildingSubsystem->FindDefinition(Feature->GetBuildingDefinitionId())
		: nullptr;
	if (!Item || !Feature || !Definition || (Stack && Stack->GetQuantity() <= 0))
	{
		return false;
	}

	OutItem = Item;
	OutQuickbarIndex = SelectedIndex;
	OutDefinition = Definition;
	OutPlacementShapeTransform = Feature->GetPlacementShapeTransform();
	return true;
}

bool UBuildingPlacementComponent::TryFindViewSurface(
	UBuildingWorldSubsystem& BuildingSubsystem,
	FVector& OutExpectedLocation) const
{
	OutExpectedLocation = FVector::ZeroVector;
	const AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	if (!PlayerController)
	{
		return false;
	}
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
	FCollisionQueryParams QueryParams(TEXT("BuildingPlacementView"), false);
	if (const APawn* Pawn = PlayerController->GetPawn())
	{
		QueryParams.AddIgnoredActor(Pawn);
	}
	if (PreviewActor)
	{
		QueryParams.AddIgnoredActor(PreviewActor);
	}
	FBuildPlacementSurfaceHit Hit;
	if (!BuildingSubsystem.QueryPlacementSurface(
		ViewLocation,
		ViewLocation + ViewRotation.Vector() * PlacementViewTraceDistance,
		QueryParams,
		Hit))
	{
		return false;
	}
	OutExpectedLocation = Hit.Location;
	return true;
}
