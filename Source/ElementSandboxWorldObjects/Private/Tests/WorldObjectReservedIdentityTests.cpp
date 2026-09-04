#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Definition/WorldObjectDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectReservedIdentityTest, "ElementSandbox.WorldObjects.Lifecycle.ReservedIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWorldObjectReservedIdentityTest::RunTest(const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("WorldObjectReservedIdentity"), nullptr, true);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Storage = World->GetSubsystem<UWorldStorageSubsystem>();
	auto* Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	const auto Before = Storage->GetRuntimeStats();
	const auto Canceled = Storage->AllocateEntityId();
	const auto Reserved = Storage->AllocateEntityId();
	TestTrue(TEXT("预分配单调递增且取消后不复用"), Reserved.GetValue() > Canceled.GetValue());
	TestFalse(TEXT("预留身份不创建 Resident"), Storage->IsResident(Reserved));
	TestFalse(TEXT("预留身份不创建 ECS"), Objects->FindEntity(Reserved).IsSet());
	TestEqual(TEXT("预留身份不新增存档实体"), Storage->GetRuntimeStats().DirtyEntityCount, Before.DirtyEntityCount);
	auto* Definition = NewObject<UWorldObjectDefinition>(World);
	Definition->DefinitionId = TEXT("Test.ReservedDormant"); Definition->SpatialClass = EWorldObjectSpatialClass::Portable;
	Definition->InteractionLocalBounds = FBox(FVector(-20), FVector(20));
	FWorldObjectCreateDesc Desc; Desc.Definition = Definition; Desc.ReservedWorldEntityId = Reserved;
	Desc.WorldTransform = FTransform(FVector(-500, -500, 30)); Desc.MotionState = EWorldObjectMotionState::Dormant;
	FWorldObjectStagedCreateBatch Batch; TArray<FWorldObjectEntityHandle> Entities;
	TestTrue(TEXT("以预留身份 Stage"), Objects->StageCreateEntities(MakeArrayView(&Desc, 1), Batch));
	TestTrue(TEXT("以预留身份 Commit"), Objects->CommitStagedCreateEntities(Batch, Entities));
	if (!Entities.IsEmpty()) TestTrue(TEXT("持久身份与轨迹身份一致"), Objects->GetWorldEntityId(Entities[0]) == Reserved);
	TestFalse(TEXT("重复创建同一预留身份被拒绝"), Objects->CreateEntity(Desc).IsSet());
	Desc.ReservedWorldEntityId = {};
	const auto Ordinary = Objects->CreateEntity(Desc);
	TestTrue(TEXT("普通创建继续使用统一分配器"), Objects->GetWorldEntityId(Ordinary).GetValue() > Reserved.GetValue());
	Desc.ReservedWorldEntityId = Storage->AllocateEntityId();
	TestTrue(TEXT("另一预留身份可 Stage"), Objects->StageCreateEntities(MakeArrayView(&Desc, 1), Batch));
	Objects->RollbackStagedCreateEntities(Batch);
	TestFalse(TEXT("取消 Stage 不残留 Resident"), Storage->IsResident(Desc.ReservedWorldEntityId));
	TestFalse(TEXT("取消 Stage 不残留 ECS"), Objects->FindEntity(Desc.ReservedWorldEntityId).IsSet());
	TestTrue(TEXT("下次预留不复用取消身份"), Storage->AllocateEntityId().GetValue() > Desc.ReservedWorldEntityId.GetValue());
	GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}
#endif
