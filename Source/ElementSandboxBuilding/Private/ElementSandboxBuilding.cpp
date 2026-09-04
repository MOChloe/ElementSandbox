#include "ElementSandboxBuilding.h"

#include "Audit/BuildRegistrationAudit.h"
#include "BuildingWorldSubsystem.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogElementSandboxBuilding);
CSV_DEFINE_CATEGORY_MODULE(ELEMENTSANDBOXBUILDING_API, ElementSandboxBuilding, true);

namespace
{
	void AuditBuildRenderPartRegistration(const TArray<FString>& Args, UWorld* World)
	{
		uint64 WorldEntityValue = 0;
		int32 MeshPartId = INDEX_NONE;
		if (Args.Num() != 2
			|| !LexTryParseString(WorldEntityValue, *Args[0])
			|| WorldEntityValue == 0
			|| !LexTryParseString(MeshPartId, *Args[1])
			|| MeshPartId < 0)
		{
			UE_LOG(LogElementSandboxBuilding, Warning,
				TEXT("Usage: Building.AuditRenderPartRegistration <WorldEntityId> <MeshPartId>"));
			return;
		}

		UBuildingWorldSubsystem* Building = World
			? World->GetSubsystem<UBuildingWorldSubsystem>()
			: nullptr;
		const FBuildEntityHandle Entity = Building
			? Building->FindEntity(FWorldEntityId(WorldEntityValue))
			: FBuildEntityHandle();
		if (!Entity.IsSet())
		{
			UE_LOG(LogElementSandboxBuilding, Warning,
				TEXT("No live Building entity found for WorldEntityId=%llu in World=%s."),
				WorldEntityValue,
				World ? *World->GetName() : TEXT("None"));
			return;
		}

		const FBuildRenderPartShapeAudit Shape =
			Building->AuditRenderPartShape(Entity, MeshPartId);
		const FBuildRenderPartCollisionAudit Collision =
			Building->AuditRenderPartCollision(Entity, MeshPartId);
		UE_LOG(LogElementSandboxBuilding, Display, TEXT("[World=%s] %s"),
			*World->GetName(),
			*FormatBuildRenderPartRegistrationAudit(Shape, Collision));
	}

	FAutoConsoleCommandWithWorldAndArgs GBuildRenderPartRegistrationAuditCommand(
		TEXT("Building.AuditRenderPartRegistration"),
		TEXT("Audit Render Part -> Shape and Render Part -> Collision Part -> Required -> Body/Failure. ")
		TEXT("Args: <WorldEntityId> <MeshPartId>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AuditBuildRenderPartRegistration));
}

IMPLEMENT_MODULE(FDefaultModuleImpl, ElementSandboxBuilding)
