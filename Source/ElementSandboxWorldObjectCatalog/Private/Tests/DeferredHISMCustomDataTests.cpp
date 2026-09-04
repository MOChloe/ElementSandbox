#if WITH_DEV_AUTOMATION_TESTS

#include "Presentation/DeferredHISMComponent.h"
#include "WorldObjects/WoodProductFlight.h"
#include "WorldObjects/WoodProductFlightMaterialSet.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "InstancedStaticMesh/ISMInstanceDataSceneProxy.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif

namespace
{
	/** 建树完成通过 Core Ticker 回到 GameThread，必须跨帧等待，不能阻塞测试帧。 */
	class FDeferredHISMCustomDataCommand final : public IAutomationLatentCommand
	{
	public:
		explicit FDeferredHISMCustomDataCommand(FAutomationTestBase& InTest) : Test(InTest) {}

		virtual ~FDeferredHISMCustomDataCommand() override
		{
			if (BuildDelay) BuildDelay->Set(PreviousDelay, ECVF_SetByCode);
			if (World)
			{
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
				FlushRenderingCommands();
			}
		}

		virtual bool Update() override
		{
			if (Phase == EPhase::Setup) return !Initialize();
			if (Component->IsAsyncBuilding())
			{
				if (FPlatformTime::Seconds() < Deadline) return false;
				Test.AddError(TEXT("原生异步建树未在十秒内完成"));
				return true;
			}

			Component->NotifyAsyncBuildObservedComplete();
			World->SendAllEndOfFrameUpdates();
			FlushRenderingCommands();
			if (Phase == EPhase::InitialTree)
			{
				BeginScenario();
			}
			else if (Phase == EPhase::InitialLayout)
			{
				StartTreeBuild();
				if (!Test.TestTrue(TEXT("原生建树已经捕获旧布局"), Component->IsAsyncBuilding())) return true;
				if (ScenarioIndex >= 2)
				{
					TArray<int32> RemovedIndices;
					for (int32 Offset = 0; Offset < 4; ++Offset)
					{
						// 删除中部槽位，确保执行尾部实例回填，而非仅缩短数组。
						RemovedIndices.Add(13 - Offset * 4);
					}
					Component->BeginBulkEdit();
					const bool bRemoved = Component->RemoveInstances(RemovedIndices, true);
					Component->EndBulkEdit(FPlatformTime::Seconds(), bRemoved);
					if (!Test.TestTrue(TEXT("布局变化前的 RemoveSwap 成功"), bRemoved)) return true;
					ExpectedInstanceCount -= RemovedIndices.Num();
				}
				Component->SetNumCustomDataFloats(FinalCount());
				Component->SetMaterial(0, Materials->GetMaterial(false, FinalCount() == 0 ? INDEX_NONE : 0));
				// 先提交新布局，再让后续帧处理旧快照，防止同帧增量掩盖过期结果。
				World->SendAllEndOfFrameUpdates();
				FlushRenderingCommands();
				if (!Test.TestTrue(TEXT("新布局先于旧建树结果提交"), Component->IsAsyncBuilding())) return true;
				Phase = EPhase::FinalLayout;
			}
			else
			{
				const auto Proxy = Component->GetInstanceDataSceneProxy();
				if (!Test.TestTrue(TEXT("实际渲染实例代理可用"), Proxy.IsValid())) return true;
				if (FInstanceDataUpdateTaskInfo* UpdateTaskInfo = Proxy->GetUpdateTaskInfo())
				{
					UpdateTaskInfo->WaitForUpdateCompletion();
				}
				const FInstanceSceneDataBuffers& Buffer = Proxy->GetData();
				Test.TestEqual(
					*FString::Printf(TEXT("%s时异步结果不得恢复 %d -> %d 之前的参数布局"),
						ScenarioIndex >= 2 ? TEXT("带删除") : TEXT("无删除"), InitialCount(), FinalCount()),
					Buffer.GetNumCustomDataFloats(), FinalCount());
				Test.TestEqual(TEXT("参数布局变化保留剩余实例"), Buffer.GetNumInstances(), ExpectedInstanceCount);
				if (++ScenarioIndex == 4) return true;
				BeginScenario();
			}
			return false;
		}

	private:
		enum class EPhase : uint8 { Setup, InitialTree, InitialLayout, FinalLayout };

		bool Initialize()
		{
			Materials.Reset(LoadObject<UWoodProductFlightMaterialSet>(nullptr,
				TEXT("/Game/WorldObjects/WoodBlock/DA_WoodProductFlightMaterials.DA_WoodProductFlightMaterials")));
			if (!Test.TestNotNull(TEXT("木块材质集可用"), Materials.Get())) return false;
#if WITH_EDITOR
			FStaticMeshCompilingManager::Get().FinishAllCompilation();
#endif
			BuildDelay = IConsoleManager::Get().FindConsoleVariable(TEXT("foliage.DebugBuildTreeAsyncDelayInSeconds"));
			if (!Test.TestNotNull(TEXT("原生异步建树延迟开关可用"), BuildDelay)) return false;
			PreviousDelay = BuildDelay->GetFloat();
			BuildDelay->Set(0.25f, ECVF_SetByCode);

			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("DeferredHISMCustomDataTest"), nullptr, true);
			if (!Test.TestNotNull(TEXT("测试 World 可用"), World)) return false;
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			AActor* Host = World->SpawnActor<AActor>();
			USceneComponent* Root = NewObject<USceneComponent>(Host);
			Host->AddInstanceComponent(Root);
			Host->SetRootComponent(Root);
			Root->RegisterComponent();
			Component = NewObject<UDeferredHISMComponent>(Host);
			Component->SetupAttachment(Root);
			Component->SetStaticMesh(Materials->Mesh);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Host->AddInstanceComponent(Component);
			Component->RegisterComponent();
			Component->PrewarmEmptyTree();

			TArray<FTransform> Instances;
			for (int32 Index = 0; Index < 64; ++Index)
			{
				Instances.Emplace(FVector(Index * 200.0, 0.0, 0.0));
			}
			Component->BeginBulkEdit();
			Component->AddInstances(Instances, false, false, false);
			Component->EndBulkEdit(FPlatformTime::Seconds(), true);
			Component->TryStartDeferredTreeBuild(FPlatformTime::Seconds(), 0.0, 0.0, true);
			Deadline = FPlatformTime::Seconds() + 10.0;
			Phase = EPhase::InitialTree;
			return true;
		}

		void StartTreeBuild()
		{
			Component->BuildTreeIfOutdated(true, true);
			Component->TryStartDeferredTreeBuild(FPlatformTime::Seconds(), 0.0, 0.0, true);
			Deadline = FPlatformTime::Seconds() + 10.0;
		}

		void BeginScenario()
		{
			// 覆盖飞行结束释放参数，以及迟到同物理组成员重新启用飞行参数。
			Component->SetNumCustomDataFloats(InitialCount());
			Component->SetMaterial(0, Materials->GetMaterial(false, InitialCount() == 0 ? INDEX_NONE : 0));
			StartTreeBuild();
			Phase = EPhase::InitialLayout;
		}

		int32 InitialCount() const { return (ScenarioIndex & 1) == 0 ? FWoodProductFlight::CustomFloatCount : 0; }
		int32 FinalCount() const { return (ScenarioIndex & 1) == 0 ? 0 : FWoodProductFlight::CustomFloatCount; }

		FAutomationTestBase& Test;
		TStrongObjectPtr<UWoodProductFlightMaterialSet> Materials;
		UWorld* World = nullptr;
		UDeferredHISMComponent* Component = nullptr;
		IConsoleVariable* BuildDelay = nullptr;
		float PreviousDelay = 0.0f;
		double Deadline = 0.0;
		int32 ScenarioIndex = 0;
		int32 ExpectedInstanceCount = 64;
		EPhase Phase = EPhase::Setup;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeferredHISMCustomDataDuringAsyncBuildTest,
	"ElementSandbox.WoodProducts.Rendering.CustomDataLayoutDuringAsyncBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDeferredHISMCustomDataDuringAsyncBuildTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	ADD_LATENT_AUTOMATION_COMMAND(FDeferredHISMCustomDataCommand(*this));
	return true;
}

#endif
