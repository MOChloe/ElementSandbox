// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/ElementSandboxGameMode.h"

#include "Characters/ElementSandboxCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Game/ElementSandboxDemoFallbackGround.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameFramework/PlayerStart.h"
#include "Inventory/InventoryComponent.h"
#include "Item/ItemDefinition.h"
#include "Items/AxeItemDefinition.h"
#include "Items/DemolitionToolItemDefinition.h"
#include "Items/FireballItemDefinition.h"
#include "Items/MeteorStrikeItemDefinition.h"
#include "UObject/ConstructorHelpers.h"

AElementSandboxGameMode::AElementSandboxGameMode()
{
	DefaultPawnClass = AElementSandboxCharacter::StaticClass();
	PlayerControllerClass = AElementSandboxPlayerController::StaticClass();
	PlayerStateClass = AElementSandboxPlayerState::StaticClass();

	static ConstructorHelpers::FObjectFinder<UItemDefinition> StickDefinition(TEXT("/Game/Items/DA_Stick.DA_Stick"));
	DefaultStickDefinition = StickDefinition.Object;
	DefaultFireballDefinition = GetMutableDefault<UFireballItemDefinition>();
	DefaultDemolitionToolDefinition = GetMutableDefault<UDemolitionToolItemDefinition>();
	DefaultAxeDefinition = GetMutableDefault<UAxeItemDefinition>();
	DefaultMeteorDefinition = GetMutableDefault<UMeteorStrikeItemDefinition>();

	static const TCHAR* BuildingItemPaths[] = { TEXT("/Game/Items/DA_WoodWall.DA_WoodWall"),
		TEXT("/Game/Items/DA_WoodFloor.DA_WoodFloor"), TEXT("/Game/Items/DA_WoodPillar.DA_WoodPillar") };
	DefaultBuildingItemDefinitions.Reserve(UE_ARRAY_COUNT(BuildingItemPaths));
	for (const TCHAR* ItemPath : BuildingItemPaths)
	{
		ConstructorHelpers::FObjectFinder<UItemDefinition> BuildingItem(ItemPath);
		DefaultBuildingItemDefinitions.Add(BuildingItem.Object);
	}
}

void AElementSandboxGameMode::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}

	APlayerStart* PlayerStart = nullptr;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		PlayerStart = *It;
		break;
	}
	if (!PlayerStart)
	{
		return;
	}

	const FVector StartLocation = PlayerStart->GetActorLocation();
	FHitResult GroundHit;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ElementSandboxFallbackGround), false);
	if (World->LineTraceSingleByChannel(
		GroundHit,
		StartLocation + FVector(0.0, 0.0, 100.0),
		StartLocation - FVector(0.0, 0.0, 5000.0),
		ECC_Pawn,
		QueryParams))
	{
		return;
	}

	const AElementSandboxCharacter* CharacterCDO = GetDefault<AElementSandboxCharacter>();
	const UCapsuleComponent* Capsule = CharacterCDO
		? CharacterCDO->GetCapsuleComponent() : nullptr;
	const double CapsuleHalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.0;
	const double GroundTopZ = StartLocation.Z - CapsuleHalfHeight;
	const FTransform GroundTransform(
		FRotator::ZeroRotator,
		FVector(StartLocation.X, StartLocation.Y, GroundTopZ - 5.0),
		// Engine Cube 为 100cm；20km 见方覆盖完整的 15km Retention Box，厚度只保留 10cm。
		FVector(20000.0, 20000.0, 0.1));
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = TEXT("ElementSandboxDemoFallbackGround");
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AElementSandboxDemoFallbackGround* Ground =
		World->SpawnActor<AElementSandboxDemoFallbackGround>(
			AElementSandboxDemoFallbackGround::StaticClass(),
			GroundTransform,
			SpawnParameters);
	UStaticMeshComponent* Mesh = Ground ? Ground->GetStaticMeshComponent() : nullptr;
	if (!Ground || !Mesh || !IsValid(Mesh->GetStaticMesh()))
	{
		UE_LOG(LogTemp, Error,
			TEXT("测试地图缺少地面，且专用复制兜底地面创建失败。"));
		if (Ground)
		{
			Ground->Destroy();
		}
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("PlayerStart 下方没有可阻挡地面；已为测试地图创建 20km 复制兜底地面，TopZ=%.0f。"),
		GroundTopZ);
}

void AElementSandboxGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	if (!IsValid(NewPlayer))
	{
		return;
	}

	if (AElementSandboxPlayerState* SandboxPlayerState = NewPlayer->GetPlayerState<AElementSandboxPlayerState>())
	{
		if (UInventoryComponent* Inventory = SandboxPlayerState->GetInventoryComponent())
		{
			if (IsValid(DefaultStickDefinition))
			{
				Inventory->GrantItemToQuickbar(DefaultStickDefinition, 0);
			}
			for (int32 ItemIndex = 0; ItemIndex < DefaultBuildingItemDefinitions.Num(); ++ItemIndex)
			{
				if (IsValid(DefaultBuildingItemDefinitions[ItemIndex]))
				{
					Inventory->GrantItemToQuickbar(DefaultBuildingItemDefinitions[ItemIndex], ItemIndex + 1, 10);
				}
			}
				if (IsValid(DefaultFireballDefinition))
				{
				Inventory->GrantItemToQuickbar(
					DefaultFireballDefinition,
					DefaultFireballQuickbarIndex,
						DefaultFireballQuantity);
				}
				if (IsValid(DefaultDemolitionToolDefinition))
				{
					Inventory->GrantItemToQuickbar(
						DefaultDemolitionToolDefinition,
						DefaultDemolitionToolQuickbarIndex);
				}
					if (IsValid(DefaultAxeDefinition))
				{
					Inventory->GrantItemToQuickbar(
						DefaultAxeDefinition,
						DefaultAxeQuickbarIndex);
					}
					if (IsValid(DefaultMeteorDefinition))
					{
						Inventory->GrantItemToQuickbar(
							DefaultMeteorDefinition,
							DefaultMeteorQuickbarIndex,
							DefaultMeteorQuantity);
					}
			}
	}
}
