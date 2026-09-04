#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/BuildFragment.h"
#include "Entity/WorldObjectFragment.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectIterator.h"
#include "WorldStorageSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageProductionFragmentClassificationTest,
	"ElementSandbox.WorldStorage.Fragments.AllProductionFragmentsClassified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageProductionFragmentClassificationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("WorldStorageFragmentClassification"),
		nullptr,
		true);
	if (!TestNotNull(TEXT("测试 World 创建成功"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UWorldStorageSubsystem* Storage = World->GetSubsystem<UWorldStorageSubsystem>();
	TestNotNull(TEXT("WorldStorage Subsystem 已初始化"), Storage);

	int32 ProductionFragmentCount = 0;
	if (Storage)
	{
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			UScriptStruct* FragmentType = *It;
			if (!FragmentType || FragmentType->HasMetaData(TEXT("WorldStorageTestFragment")))
			{
				continue;
			}

			EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
			if (FragmentType != FBuildFragment::StaticStruct()
				&& FragmentType->IsChildOf(FBuildFragment::StaticStruct()))
			{
				Domain = EWorldEntityDomain::Building;
			}
			else if (FragmentType != FWorldObjectFragment::StaticStruct()
				&& FragmentType->IsChildOf(FWorldObjectFragment::StaticStruct()))
			{
				Domain = EWorldEntityDomain::WorldObject;
			}
			if (Domain == EWorldEntityDomain::Invalid)
			{
				continue;
			}

			++ProductionFragmentCount;
			TestTrue(
				*FString::Printf(
					TEXT("%s 必须显式分类为 Persistent、Derived 或 RuntimeOnly"),
					*FragmentType->GetPathName()),
				Storage->FindFragmentPersistence(Domain, *FragmentType).IsSet());
		}
	}
	TestTrue(TEXT("反射审计至少发现一个生产 Fragment"), ProductionFragmentCount > 0);

	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
