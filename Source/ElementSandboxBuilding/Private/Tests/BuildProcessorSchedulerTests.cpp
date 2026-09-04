#if WITH_DEV_AUTOMATION_TESTS

#include "BuildingWorldSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Processing/BuildProcessor.h"
#include "WorldStorageSubsystem.h"

namespace ElementSandbox::Building::ProcessorTests
{
	class FScriptedBuildProcessor final : public FBuildProcessor
	{
	public:
		explicit FScriptedBuildProcessor(const int32 InId)
			: FBuildProcessor(*FString::Printf(TEXT("Scripted-%d"), InId))
		{
		}

		bool Wake()
		{
			return RequestExecution();
		}

		EBuildProcessorRunResult NextResult = EBuildProcessorRunResult::Done;
		int32 ExecuteCount = 0;
		bool bWakeDuringExecute = false;

	private:
		virtual EBuildProcessorRunResult Execute(FBuildProcessorContext& Context) override
		{
			++ExecuteCount;
			if (bWakeDuringExecute)
			{
				RequestExecution();
				bWakeDuringExecute = false;
			}
			return NextResult;
		}
	};

	struct FProcessorTestWorld final
	{
		explicit FProcessorTestWorld(const TCHAR* Name)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Subsystem = World->GetSubsystem<UBuildingWorldSubsystem>();
		}

		~FProcessorTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		void RunFrame()
		{
			FWorldDelegates::OnWorldPostActorTick.Broadcast(
				World,
				LEVELTICK_All,
				static_cast<float>(UWorldStorageSubsystem::AuthorityTickIntervalSeconds));
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Subsystem = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildProcessorReadySchedulingTest,
	"ElementSandbox.Building.Processor.ReadyScheduling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildProcessorReadySchedulingTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::ProcessorTests;
	FProcessorTestWorld Harness(TEXT("BuildProcessorReady"));
	TArray<FBuildProcessorRegistrationHandle> Registrations;
	TArray<FScriptedBuildProcessor*> ProcessorPointers;
	Registrations.Reserve(1000);
	ProcessorPointers.Reserve(1000);
	FScriptedBuildProcessor* Target = nullptr;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		TUniquePtr<FScriptedBuildProcessor> Processor =
			MakeUnique<FScriptedBuildProcessor>(Index);
		if (Index == 437)
		{
			Target = Processor.Get();
		}
		ProcessorPointers.Add(Processor.Get());
		Registrations.Add(Harness.Subsystem->RegisterProcessor(MoveTemp(Processor)));
	}
	bool bAllRegistered = true;
	for (const FBuildProcessorRegistrationHandle Registration : Registrations)
	{
		bAllRegistered &= Registration.IsSet();
	}
	TestTrue(TEXT("1000 个轻 Processor 均注册成功"), bAllRegistered);

	Harness.RunFrame();
	FBuildProcessorStats TargetStats;
	TestTrue(TEXT("可读取目标 Processor 统计"),
		Harness.Subsystem->TryGetProcessorStats(Registrations[437], TargetStats));
	TestEqual(TEXT("全部空闲时没有 Processor 被执行"),
		TargetStats.ExecutionCount,
		static_cast<uint64>(0));
	int32 TotalExecuteCount = 0;
	for (const FScriptedBuildProcessor* Processor : ProcessorPointers)
	{
		TotalExecuteCount += Processor ? Processor->ExecuteCount : 0;
	}
	TestEqual(TEXT("1000 个空闲 Processor 的业务 Execute 总数为零"),
		TotalExecuteCount, 0);

	TestTrue(TEXT("目标 Processor 首次唤醒"), Target && Target->Wake());
	TestTrue(TEXT("同帧重复唤醒被 Ready 位合并"), Target && Target->Wake());
	Harness.RunFrame();
	Harness.Subsystem->TryGetProcessorStats(Registrations[437], TargetStats);
	TestEqual(TEXT("一帧只执行目标 Processor 一次"),
		TargetStats.ExecutionCount,
		static_cast<uint64>(1));
	TestEqual(TEXT("目标 Processor 业务执行一次"), Target ? Target->ExecuteCount : 0, 1);
	TotalExecuteCount = 0;
	for (const FScriptedBuildProcessor* Processor : ProcessorPointers)
	{
		TotalExecuteCount += Processor ? Processor->ExecuteCount : 0;
	}
	TestEqual(TEXT("唤醒一个 Processor 时其余 999 个 Execute 数仍为零"),
		TotalExecuteCount, 1);

	if (Target)
	{
		Target->NextResult = EBuildProcessorRunResult::RetryNextFrame;
		Target->Wake();
	}
	Harness.RunFrame();
	Harness.RunFrame();
	Harness.Subsystem->TryGetProcessorStats(Registrations[437], TargetStats);
	TestEqual(TEXT("RetryNextFrame 每帧执行一次"),
		TargetStats.ExecutionCount,
		static_cast<uint64>(3));

	if (Target)
	{
		Target->NextResult = EBuildProcessorRunResult::Done;
		Target->bWakeDuringExecute = true;
	}
	Harness.RunFrame();
	TestEqual(TEXT("执行中唤醒不会在同一帧重入"), Target ? Target->ExecuteCount : 0, 4);
	Harness.RunFrame();
	TestEqual(TEXT("执行中唤醒留到下一帧"), Target ? Target->ExecuteCount : 0, 5);

	if (Target)
	{
		Target->NextResult = EBuildProcessorRunResult::Failed;
		Target->Wake();
	}
	AddExpectedError(
		TEXT("Building Processor Scripted-437 failed"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Harness.RunFrame();
	Harness.RunFrame();
	Harness.Subsystem->TryGetProcessorStats(Registrations[437], TargetStats);
	TestEqual(TEXT("Failed 保留工作并逐帧重试"),
		TargetStats.FailureCount, static_cast<uint64>(2));
	TestEqual(TEXT("连续 Failed 计数累积"),
		TargetStats.ConsecutiveFailureCount, static_cast<uint32>(2));

	if (Target)
	{
		Target->NextResult = EBuildProcessorRunResult::Done;
	}
	Harness.RunFrame();
	Harness.Subsystem->TryGetProcessorStats(Registrations[437], TargetStats);
	TestEqual(TEXT("成功执行会清除连续错误计数"),
		TargetStats.ConsecutiveFailureCount, static_cast<uint32>(0));
	TestFalse(TEXT("Done 后退出 Ready"), TargetStats.bReady);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildProcessorHandleIsolationTest,
	"ElementSandbox.Building.Processor.HandleIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildProcessorHandleIsolationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::ProcessorTests;
	FProcessorTestWorld First(TEXT("BuildProcessorFirst"));
	FProcessorTestWorld Second(TEXT("BuildProcessorSecond"));
	TUniquePtr<FScriptedBuildProcessor> Processor = MakeUnique<FScriptedBuildProcessor>(1);
	FScriptedBuildProcessor* ProcessorPointer = Processor.Get();
	const FBuildProcessorRegistrationHandle OldHandle =
		First.Subsystem->RegisterProcessor(MoveTemp(Processor));
	FBuildProcessorStats Stats;
	TestFalse(TEXT("跨 World Registration Handle 被拒绝"),
		Second.Subsystem->TryGetProcessorStats(OldHandle, Stats));
	TestTrue(TEXT("注销前 Processor 可进入 Ready Queue"),
		ProcessorPointer && ProcessorPointer->Wake());
	TestTrue(TEXT("注销有效 Registration"), First.Subsystem->UnregisterProcessor(OldHandle));
	TestFalse(TEXT("旧 Registration 注销后失效"),
		First.Subsystem->TryGetProcessorStats(OldHandle, Stats));
	First.RunFrame();

	TUniquePtr<FScriptedBuildProcessor> Replacement = MakeUnique<FScriptedBuildProcessor>(2);
	const FBuildProcessorRegistrationHandle NewHandle =
		First.Subsystem->RegisterProcessor(MoveTemp(Replacement));
	TestTrue(TEXT("注销 Slot 可重新注册"), NewHandle.IsSet());
	TestTrue(TEXT("复用 Slot 的新 Registration 使用新 Generation"),
		OldHandle != NewHandle);
	return true;
}

#endif
