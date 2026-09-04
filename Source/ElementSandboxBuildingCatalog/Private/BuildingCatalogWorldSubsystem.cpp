#include "BuildingCatalogWorldSubsystem.h"

#include "BuildingWorldSubsystem.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Door/DoorBuildingDefinition.h"
#include "Door/DoorInteractionResolver.h"
#include "Door/DoorProcessor.h"
#include "Door/DoorStateFragment.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "FirePile/FirePileBuildingDefinition.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Storage/BuildingPersistenceExtension.h"
#include "Subsystems/SubsystemCollection.h"
#include "Torch/TorchFixtureBuildingDefinition.h"
#include "Wood/WoodBuildingDefinition.h"
#include "WorldStorageSubsystem.h"

namespace
{
	constexpr uint32 CatalogPayloadMagic = 0x33544143; // CAT3

}

class FBuildingCatalogPersistenceExtension final : public IBuildingPersistenceExtension
{
public:
	explicit FBuildingCatalogPersistenceExtension(UBuildingCatalogWorldSubsystem& InOwner)
		: Owner(&InOwner)
	{
	}

	virtual FName GetSectionId() const override { return TEXT("BuildingCatalog.State"); }
	virtual uint16 GetSectionVersion() const override { return 3; }

	virtual bool Capture(
		const FBuildEntityRegistry& Registry,
		const FBuildEntityHandle Entity,
		TArray<uint8>& OutPayload,
		FString& OutError) const override
	{
		OutPayload.Reset();
		const FBuildDoorStateFragment* Door = Registry.FindFragment<FBuildDoorStateFragment>(Entity);
		if (!ValidateDoorState(Door, OutError))
		{
			return false;
		}
		if (!Door || (Door->State == EBuildDoorState::Closed
			&& FMath::IsNearlyZero(Door->TransitionStartServerTimeSeconds)))
		{
			return true;
		}

		FMemoryWriter Writer(OutPayload, true);
		uint32 Magic = CatalogPayloadMagic;
		uint8 State = static_cast<uint8>(Door->State);
		double TransitionStart = Door->TransitionStartServerTimeSeconds;
		Writer << Magic << State << TransitionStart;
		if (Writer.IsError())
		{
			OutError = TEXT("BuildingCatalog Door Payload 编码失败。");
			return false;
		}
		return true;
	}

	virtual bool ValidateRemovalState(
		const FBuildEntityRegistry& Registry,
		const FBuildEntityHandle Entity,
		FString& OutError) const override
	{
		return ValidateDoorState(Registry.FindFragment<FBuildDoorStateFragment>(Entity), OutError);
	}

	virtual bool Restore(
		FBuildEntityRegistry& Registry,
		const FBuildEntityHandle Entity,
		const uint16 SectionVersion,
		const TConstArrayView<uint8> Payload,
		FString& OutError) const override
	{
		if (SectionVersion != GetSectionVersion())
		{
			OutError = TEXT("BuildingCatalog Section 版本不匹配；旧版 Fire 状态存档不支持迁移。");
			return false;
		}
		FBuildDoorStateFragment* Door = Registry.FindMutableFragment<FBuildDoorStateFragment>(Entity);
		if (Door)
		{
			Door->State = EBuildDoorState::Closed;
			Door->TransitionStartServerTimeSeconds = 0.0;
		}
		if (!Payload.IsEmpty())
		{
			TArray<uint8> Bytes(Payload);
			FMemoryReader Reader(Bytes, true);
			uint32 Magic = 0;
			uint8 State = 0;
			double TransitionStart = 0.0;
			Reader << Magic << State << TransitionStart;
			if (!Door || Magic != CatalogPayloadMagic
				|| State > static_cast<uint8>(EBuildDoorState::Closing)
				|| !FMath::IsFinite(TransitionStart) || TransitionStart < 0.0
				|| Reader.IsError() || Reader.Tell() != Reader.TotalSize())
			{
				OutError = TEXT("BuildingCatalog Door Payload 非法或与 Definition 不兼容。");
				return false;
			}
			Door->State = static_cast<EBuildDoorState>(State);
			Door->TransitionStartServerTimeSeconds = TransitionStart;
		}

		UBuildingCatalogWorldSubsystem* Catalog = Owner.Get();
		if (Door && Catalog && Catalog->DoorProcessor
			&& !Catalog->DoorProcessor->NotifyRestoredState(Entity))
		{
			OutError = TEXT("Door Restore 无法排入派生状态重建。");
			return false;
		}
		return true;
	}

	virtual bool RegisterFragmentPersistence(UWorldStorageSubsystem& WorldStorage) const override
	{
		return WorldStorage.RegisterFragmentPersistence(
			EWorldEntityDomain::Building,
			*FBuildDoorStateFragment::StaticStruct(),
			EWorldFragmentPersistence::Persistent);
	}

private:
	static bool ValidateDoorState(const FBuildDoorStateFragment* Door, FString& OutError)
	{
		if (!Door)
		{
			return true;
		}
		if (Door->State > EBuildDoorState::Closing
			|| !FMath::IsFinite(Door->TransitionStartServerTimeSeconds)
			|| Door->TransitionStartServerTimeSeconds < 0.0)
		{
			OutError = TEXT("BuildingCatalog Door 状态非法。");
			return false;
		}
		return true;
	}

	TWeakObjectPtr<UBuildingCatalogWorldSubsystem> Owner;
};

void UBuildingCatalogWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UBuildingWorldSubsystem>();

	UBuildingWorldSubsystem* Building = GetWorld()
		? GetWorld()->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	BuildingSubsystem = Building;
	if (Building)
	{
		DoorDefinition = NewObject<UDoorBuildingDefinition>(this);
		SettlementDoorDefinition = NewObject<UDoorBuildingDefinition>(this);
		FirePileDefinition = NewObject<UFirePileBuildingDefinition>(this);
		MountedTorchDefinition = NewObject<UTorchFixtureBuildingDefinition>(this);
		WoodWallDefinition = NewObject<UWoodBuildingDefinition>(this);
		WoodFloorDefinition = NewObject<UWoodBuildingDefinition>(this);
		WoodPillarDefinition = NewObject<UWoodBuildingDefinition>(this);
		if (!IsValid(DoorDefinition) || !Building->RegisterDefinition(*DoorDefinition))
		{
			DoorDefinition = nullptr;
		}
		if (!IsValid(SettlementDoorDefinition)
			|| !SettlementDoorDefinition->InitializeAsSettlementCompanion()
			|| !Building->RegisterDefinition(*SettlementDoorDefinition))
		{
			SettlementDoorDefinition = nullptr;
		}
		if (!IsValid(FirePileDefinition) || !Building->RegisterDefinition(*FirePileDefinition))
		{
			FirePileDefinition = nullptr;
		}
		if (!IsValid(MountedTorchDefinition)
			|| !Building->RegisterDefinition(*MountedTorchDefinition))
		{
			MountedTorchDefinition = nullptr;
		}
		if (!IsValid(WoodWallDefinition)
			|| !WoodWallDefinition->Initialize(TEXT("WoodWall"), FVector(20.0, 400.0, 300.0))
			|| !Building->RegisterDefinition(*WoodWallDefinition))
		{
			WoodWallDefinition = nullptr;
		}
		if (!IsValid(WoodFloorDefinition)
			|| !WoodFloorDefinition->Initialize(TEXT("WoodFloor"), FVector(400.0, 400.0, 20.0))
			|| !Building->RegisterDefinition(*WoodFloorDefinition))
		{
			WoodFloorDefinition = nullptr;
		}
		if (!IsValid(WoodPillarDefinition)
			|| !WoodPillarDefinition->Initialize(TEXT("WoodPillar"), FVector(20.0, 20.0, 300.0))
			|| !Building->RegisterDefinition(*WoodPillarDefinition))
		{
			WoodPillarDefinition = nullptr;
		}

		CityBuildingPieceDefinitions.Reset();
		CityBuildingPieceDefinitions.Reserve(
			GetDefaultCityPrimitivePieceKinds().Num()
			* GetDefaultCityPieceSurfaceProfileIds().Num());
		for (const ECityBuildingPieceKind Kind : GetDefaultCityPrimitivePieceKinds())
		{
			for (const FName SurfaceProfileId : GetDefaultCityPieceSurfaceProfileIds())
			{
				UCityBuildingPieceDefinition* Definition = NewObject<UCityBuildingPieceDefinition>(this);
				if (IsValid(Definition) && Definition->Initialize(Kind, SurfaceProfileId)
					&& Building->RegisterDefinition(*Definition))
				{
					CityBuildingPieceDefinitions.Add(Definition);
				}
			}
		}
	}

	if (!Building) return;

	const bool bAuthority = GetWorld()->GetNetMode() != NM_Client;
	FBuildDoorStateChangedDelegate DoorStateChanged;
	if (bAuthority)
	{
		DoorStateChanged.BindUObject(
			this, &UBuildingCatalogWorldSubsystem::HandleAuthorityDoorStateChanged);
	}
	TUniquePtr<FBuildDoorProcessor> Door =
		MakeUnique<FBuildDoorProcessor>(bAuthority, MoveTemp(DoorStateChanged));
	DoorProcessor = Door.Get();
	DoorRegistration = Building->RegisterProcessor(MoveTemp(Door));
	if (!DoorRegistration.IsSet()) DoorProcessor = nullptr;

	const TSharedRef<FBuildingCatalogPersistenceExtension> Extension =
		MakeShared<FBuildingCatalogPersistenceExtension>(*this);
	if (Building->RegisterPersistenceExtension(Extension))
	{
		PersistenceExtension = Extension;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("BuildingCatalog 持久化 Section 注册失败。"));
	}

}

void UBuildingCatalogWorldSubsystem::Deinitialize()
{
	if (PersistenceExtension.IsValid())
	{
		if (UBuildingWorldSubsystem* Building = BuildingSubsystem.Get())
		{
			Building->UnregisterPersistenceExtension(
				PersistenceExtension->GetSectionId(), *PersistenceExtension);
		}
		PersistenceExtension.Reset();
	}
	if (UBuildingWorldSubsystem* Building = BuildingSubsystem.Get())
	{
		if (DoorRegistration.IsSet()) Building->UnregisterProcessor(DoorRegistration);
	}
	DoorProcessor = nullptr;
	DoorRegistration = {};
	DoorDefinition = nullptr;
	SettlementDoorDefinition = nullptr;
	FirePileDefinition = nullptr;
	MountedTorchDefinition = nullptr;
	WoodWallDefinition = nullptr;
	WoodFloorDefinition = nullptr;
	WoodPillarDefinition = nullptr;
	CityBuildingPieceDefinitions.Reset();
	BuildingSubsystem.Reset();
	Super::Deinitialize();
}

bool UBuildingCatalogWorldSubsystem::RequestDoorInteraction(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	UBuildingWorldSubsystem* Building = BuildingSubsystem.Get();
	if (!Building || !GetWorld() || GetWorld()->GetNetMode() == NM_Client
		|| !DoorProcessor || !DoorRegistration.IsSet())
	{
		return false;
	}
	EBuildDoorInteractionIntent Intent = EBuildDoorInteractionIntent::None;
	return TryResolveBuildDoorInteraction(Building->GetRegistry(), Entity, Intent)
		&& DoorProcessor->RequestInteraction(Entity);
}

bool UBuildingCatalogWorldSubsystem::HasAuthorityDoorProcessor() const
{
	check(IsInGameThread());
	return GetWorld() && GetWorld()->GetNetMode() != NM_Client
		&& DoorProcessor && DoorRegistration.IsSet();
}

void UBuildingCatalogWorldSubsystem::HandleAuthorityDoorStateChanged(
	const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (UBuildingWorldSubsystem* Building = BuildingSubsystem.Get())
	{
		Building->CommitPersistentStateChange(Entity);
	}
}

UBuildingDefinition* UBuildingCatalogWorldSubsystem::FindBuildingDefinition(
	const FName DefinitionId) const
{
	const UBuildingWorldSubsystem* Building = BuildingSubsystem.Get();
	return Building ? Building->FindDefinition(DefinitionId) : nullptr;
}

UCityBuildingPieceDefinition* UBuildingCatalogWorldSubsystem::GetCityBuildingPieceDefinition(
	const ECityBuildingPieceKind Kind,
	const FName SurfaceProfileId) const
{
	for (UCityBuildingPieceDefinition* Definition : CityBuildingPieceDefinitions)
	{
		if (IsValid(Definition) && Definition->GetPieceKind() == Kind
			&& Definition->GetSurfaceProfileId() == SurfaceProfileId)
		{
			return Definition;
		}
	}
	return nullptr;
}

bool UBuildingCatalogWorldSubsystem::TryGetDoorProcessorStats(
	FBuildProcessorStats& OutStats) const
{
	check(IsInGameThread());
	const UBuildingWorldSubsystem* Building = BuildingSubsystem.Get();
	return Building && DoorRegistration.IsSet()
		&& Building->TryGetProcessorStats(DoorRegistration, OutStats);
}

bool UBuildingCatalogWorldSubsystem::DoesSupportWorldType(
	const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
