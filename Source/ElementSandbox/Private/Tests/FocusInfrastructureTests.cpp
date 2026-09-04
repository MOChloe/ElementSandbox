#include "Tests/FocusTestTypes.h"

#include "Focus/FocusInteractionPrompt.h"
#include "Focus/FocusQueryTypes.h"

namespace
{
	const FFocusTestTarget* GetTestTarget(const FFocusQueryHit& Hit)
	{
		return Hit.Target.GetPtr<FFocusTestTarget>();
	}
}

bool UFocusTestHandler::IsSameTargetImpl(
	const FFocusQueryHit& Left,
	const FFocusQueryHit& Right) const
{
	const FFocusTestTarget* LeftTarget = GetTestTarget(Left);
	const FFocusTestTarget* RightTarget = GetTestTarget(Right);
	return LeftTarget && RightTarget && LeftTarget->Value == RightTarget->Value;
}

void UFocusTestHandler::HandleFocusGainedImpl(const FFocusQueryHit& Hit)
{
	if (const FFocusTestTarget* Target = GetTestTarget(Hit))
	{
		++FocusGainedCount;
		LastGainedValue = Target->Value;
	}
}

void UFocusTestHandler::HandleFocusLostImpl(const FFocusQueryHit& Hit)
{
	if (const FFocusTestTarget* Target = GetTestTarget(Hit))
	{
		++FocusLostCount;
		LastLostValue = Target->Value;
	}
}

bool UFocusTestHandler::TryResolvePromptImpl(
	const FFocusQueryHit& Hit,
	FFocusInteractionPrompt& OutPrompt) const
{
	++PromptResolveCount;
	if (!bProvidePrompt || !GetTestTarget(Hit))
	{
		return false;
	}
	OutPrompt.Text = PromptText;
	return true;
}

bool UFocusTestHandler::HandleInteractImpl(const FFocusQueryHit& Hit)
{
	const FFocusTestTarget* Target = GetTestTarget(Hit);
	if (!Target)
	{
		return false;
	}

	++InteractCount;
	LastInteractValue = Target->Value;
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "BuildingWorldSubsystem.h"
#include "BuildingCatalogWorldSubsystem.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Definition/WorldObjectDefinition.h"
#include "Door/DoorBuildingDefinition.h"
#include "Door/DoorInteractionResolver.h"
#include "Door/DoorStateFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildTransformFragment.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "FirePile/FirePileBuildingDefinition.h"
#include "Focus/FocusHighlightActor.h"
#include "Focus/BuildingFocusHighlightPresenterComponent.h"
#include "Focus/BuildingFocusHandler.h"
#include "Focus/BuildingFocusQueryComponent.h"
#include "Focus/BuildingFocusTarget.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/FocusPromptPresenterComponent.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "Focus/WorldObjectFocusQueryComponent.h"
#include "Focus/WorldObjectFocusHandler.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Item/ItemDefinition.h"
#include "Spatial/BuildSpatialIndex.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/StickWorldObjectDefinition.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"

namespace ElementSandbox::Focus::Tests
{
	FFocusQueryHit MakeHit(const double Distance, const int32 Value)
	{
		FFocusTestTarget Target;
		Target.Value = Value;

		FFocusQueryHit Hit;
		Hit.HitDistance = Distance;
		Hit.HitLocation = FVector(Distance, 0.0, 0.0);
		Hit.Target = FInstancedStruct::Make<FFocusTestTarget>(Target);
		return Hit;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFocusNoRegistrationTest,
	"ElementSandbox.Focus.NoRegistrationProducesNoData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFocusNoRegistrationTest::RunTest(const FString& Parameters)
{
	UFocusHostComponent* Host = NewObject<UFocusHostComponent>();
	FFocusQueryContext Context;
	Host->EvaluateFocus(Context);

	TestNull(TEXT("没有注册项时没有 Focus"), Host->GetFocusedHit());
	TestFalse(TEXT("没有 Focus 时 Interact 不会被处理"), Host->HandleInteract());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFocusNearestRegisteredHitTest,
	"ElementSandbox.Focus.NearestHitKeepsItsHandler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFocusNearestRegisteredHitTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Focus::Tests;

	UFocusHostComponent* Host = NewObject<UFocusHostComponent>();
	UFocusTestHandler* FarHandler = NewObject<UFocusTestHandler>(Host);
	UFocusTestHandler* NearHandler = NewObject<UFocusTestHandler>(Host);

	const FFocusQueryRegistrationHandle FarRegistration = Host->RegisterQuery(
		*FarHandler,
		FFocusQueryDelegate::CreateLambda(
			[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
			{
				Hits.Add(MakeHit(600.0, 6));
				Hits.Add(MakeHit(300.0, 3));
			}),
		*FarHandler);
	const FFocusQueryRegistrationHandle NearRegistration = Host->RegisterQuery(
		*NearHandler,
		FFocusQueryDelegate::CreateLambda(
			[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
			{
				Hits.Add(MakeHit(120.0, 12));
			}),
		*NearHandler);

	TestTrue(TEXT("远端 Query 注册成功"), FarRegistration.IsSet());
	TestTrue(TEXT("近端 Query 注册成功"), NearRegistration.IsSet());

	FFocusQueryContext Context;
	Host->EvaluateFocus(Context);
	const FFocusQueryHit* FocusedHit = Host->GetFocusedHit();
	TestNotNull(TEXT("多个 Query 合并后产生 Focus"), FocusedHit);
	if (FocusedHit)
	{
		TestEqual(TEXT("只保留全局最近距离"), FocusedHit->HitDistance, 120.0);
		const FFocusTestTarget* Target = FocusedHit->Target.GetPtr<FFocusTestTarget>();
		TestTrue(TEXT("最近 Hit 保留自己的 Target"), Target && Target->Value == 12);
	}

	TestTrue(TEXT("Interact 交给最近 Hit 绑定的 Handler"), Host->HandleInteract());
	TestEqual(TEXT("近端 Handler 收到一次 Focus Gained"), NearHandler->FocusGainedCount, 1);
	TestEqual(TEXT("近端 Handler 收到自己的聚焦 Target"), NearHandler->LastGainedValue, 12);
	TestEqual(TEXT("近端 Handler 处理一次 Interact"), NearHandler->InteractCount, 1);
	TestEqual(TEXT("Interact 保留当前 Target"), NearHandler->LastInteractValue, 12);
	TestEqual(TEXT("远端 Handler 不会获得 Focus"), FarHandler->FocusGainedCount, 0);

	Host->EvaluateFocus(Context);
	TestEqual(TEXT("连续命中同一目标不会重复发送 Gained"), NearHandler->FocusGainedCount, 1);
	TestEqual(TEXT("连续命中同一目标不会发送 Lost"), NearHandler->FocusLostCount, 0);

	UFocusTestHandler* TieHandler = NewObject<UFocusTestHandler>(Host);
	const FFocusQueryRegistrationHandle TieRegistration = Host->RegisterQuery(
		*TieHandler,
		FFocusQueryDelegate::CreateLambda(
			[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
			{
				Hits.Add(MakeHit(120.0, 99));
			}),
		*TieHandler);
	TestTrue(TEXT("同距 Query 注册成功"), TieRegistration.IsSet());
	Host->EvaluateFocus(Context);
	TestEqual(TEXT("同距时保持先注册 Query 的稳定顺序"),
		NearHandler->FocusGainedCount, 1);
	TestEqual(TEXT("后注册的同距 Query 不抢占 Focus"),
		TieHandler->FocusGainedCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFocusPromptUsesCurrentHandlerTest,
	"ElementSandbox.Focus.PromptUsesCurrentHandler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFocusPromptUsesCurrentHandlerTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Focus::Tests;

	UFocusHostComponent* Host = NewObject<UFocusHostComponent>();
	UFocusTestHandler* FarHandler = NewObject<UFocusTestHandler>(Host);
	UFocusTestHandler* NearHandler = NewObject<UFocusTestHandler>(Host);
	FarHandler->bProvidePrompt = true;
	FarHandler->PromptText = FText::FromString(TEXT("远目标"));
	NearHandler->bProvidePrompt = true;
	NearHandler->PromptText = FText::FromString(TEXT("近目标"));

	Host->RegisterQuery(
		*FarHandler,
		FFocusQueryDelegate::CreateLambda(
			[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
			{
				Hits.Add(MakeHit(300.0, 3));
			}),
		*FarHandler);
	Host->RegisterQuery(
		*NearHandler,
		FFocusQueryDelegate::CreateLambda(
			[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
			{
				Hits.Add(MakeHit(100.0, 1));
			}),
		*NearHandler);

	FFocusQueryContext Context;
	Host->EvaluateFocus(Context);
	FFocusInteractionPrompt Prompt;
	TestTrue(TEXT("当前 Focus 的 Handler 可以解析提示"),
		Host->TryResolveFocusedPrompt(Prompt));
	TestEqual(TEXT("只读取最近目标 Handler 的文字"),
		Prompt.Text.ToString(), FString(TEXT("近目标")));
	TestEqual(TEXT("最近 Handler 解析一次"), NearHandler->PromptResolveCount, 1);
	TestEqual(TEXT("未聚焦 Handler 不参与提示解析"), FarHandler->PromptResolveCount, 0);

	NearHandler->bProvidePrompt = false;
	TestFalse(TEXT("当前目标状态失效后提示立即消失"),
		Host->TryResolveFocusedPrompt(Prompt));
	Host->ClearFocus();
	TestFalse(TEXT("没有 Focus 时不产生提示"),
		Host->TryResolveFocusedPrompt(Prompt));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFocusRegistrationLifecycleTest,
	"ElementSandbox.Focus.RegistrationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFocusRegistrationLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Focus::Tests;

	UFocusHostComponent* Host = NewObject<UFocusHostComponent>();
	UFocusTestHandler* Handler = NewObject<UFocusTestHandler>(Host);
	const FFocusQueryRegistrationHandle Registration = Host->RegisterQuery(
		*Handler,
		FFocusQueryDelegate::CreateLambda(
			[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
			{
				Hits.Add(MakeHit(50.0, 5));
			}),
		*Handler);

	FFocusQueryContext Context;
	Host->EvaluateFocus(Context);
	TestNotNull(TEXT("注销前存在 Focus"), Host->GetFocusedHit());
	TestEqual(TEXT("注销前收到一次 Gained"), Handler->FocusGainedCount, 1);
	TestTrue(TEXT("可以显式注销 Query"), Host->UnregisterQuery(Registration));
	TestNull(TEXT("注销当前来源时立即清空 Focus"), Host->GetFocusedHit());
	TestEqual(TEXT("注销当前来源时发送一次 Lost"), Handler->FocusLostCount, 1);
	TestFalse(TEXT("同一 Handle 不能重复注销"), Host->UnregisterQuery(Registration));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFocusTargetTransitionTest,
	"ElementSandbox.Focus.TargetTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFocusTargetTransitionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Focus::Tests;

	UFocusHostComponent* Host = NewObject<UFocusHostComponent>();
	UFocusTestHandler* Handler = NewObject<UFocusTestHandler>(Host);
	int32 CurrentTarget = 1;
	bool bProduceHit = true;
	const FFocusQueryRegistrationHandle Registration = Host->RegisterQuery(
		*Handler,
		FFocusQueryDelegate::CreateLambda(
			[&CurrentTarget, &bProduceHit](
				const FFocusQueryContext&,
				TArray<FFocusQueryHit>& Hits)
			{
				if (bProduceHit)
				{
					Hits.Add(MakeHit(40.0, CurrentTarget));
				}
			}),
		*Handler);
	TestTrue(TEXT("状态切换 Query 注册成功"), Registration.IsSet());

	FFocusQueryContext Context;
	Host->EvaluateFocus(Context);
	Host->EvaluateFocus(Context);
	TestEqual(TEXT("持续聚焦同一目标只发送一次 Gained"), Handler->FocusGainedCount, 1);
	TestEqual(TEXT("持续聚焦同一目标不发送 Lost"), Handler->FocusLostCount, 0);

	CurrentTarget = 2;
	Host->EvaluateFocus(Context);
	TestEqual(TEXT("切换目标先让旧目标 Lost"), Handler->FocusLostCount, 1);
	TestEqual(TEXT("旧目标身份正确"), Handler->LastLostValue, 1);
	TestEqual(TEXT("切换目标让新目标 Gained"), Handler->FocusGainedCount, 2);
	TestEqual(TEXT("新目标身份正确"), Handler->LastGainedValue, 2);

	bProduceHit = false;
	Host->EvaluateFocus(Context);
	TestNull(TEXT("没有候选时清空 Focus"), Host->GetFocusedHit());
	TestEqual(TEXT("离开全部目标时再发送一次 Lost"), Handler->FocusLostCount, 2);
	TestEqual(TEXT("最后失焦目标身份正确"), Handler->LastLostValue, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFocusControllerAssemblyTest,
	"ElementSandbox.Focus.ControllerAssembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFocusControllerAssemblyTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	AElementSandboxPlayerController* Controller = World->SpawnActor<AElementSandboxPlayerController>();

	UFocusHostComponent* Host = Controller
		? Controller->FindComponentByClass<UFocusHostComponent>()
		: nullptr;
	UBuildingFocusQueryComponent* BuildingQuery = Controller
		? Controller->FindComponentByClass<UBuildingFocusQueryComponent>()
		: nullptr;
	UBuildingFocusHighlightPresenterComponent* BuildingHighlight = Controller
		? Controller->FindComponentByClass<UBuildingFocusHighlightPresenterComponent>()
		: nullptr;
	UWorldObjectFocusQueryComponent* WorldObjectQuery = Controller
		? Controller->FindComponentByClass<UWorldObjectFocusQueryComponent>()
		: nullptr;
	UFocusPromptPresenterComponent* PromptPresenter = Controller
		? Controller->FindComponentByClass<UFocusPromptPresenterComponent>()
		: nullptr;
	TestNotNull(TEXT("PlayerController 装配 Focus Host"), Host);
	TestNotNull(TEXT("PlayerController 装配 Building ECS Query"), BuildingQuery);
	TestNotNull(TEXT("PlayerController 装配 Building Focus 高亮 Presenter"), BuildingHighlight);
	TestNotNull(TEXT("PlayerController 装配 WorldObject ECS Query"), WorldObjectQuery);
	TestNotNull(TEXT("PlayerController 装配唯一通用提示 Presenter"), PromptPresenter);
	TestTrue(TEXT("Building Query 持有聚焦生命周期 Handler"),
		BuildingQuery && IsValid(BuildingQuery->GetHandler()));
	TestTrue(TEXT("WorldObject Query 持有拾取 Handler"),
		WorldObjectQuery && IsValid(WorldObjectQuery->GetHandler()));

	if (Controller && !Controller->HasActorBegunPlay())
	{
		Controller->DispatchBeginPlay();
	}
	TestEqual(TEXT("本地开局自动注册 Building 与 WorldObject 两个领域 Query"),
		Host ? Host->GetRegisteredQueryCount() : 0, 2);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingFocusQueryTest,
	"ElementSandbox.Focus.BuildingSpatialQueryReturnsEntity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingFocusQueryTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("BuildingFocusQuery"),
		nullptr,
		true);
	TestNotNull(TEXT("创建 Building Focus 测试 World"), World);
	if (!World)
	{
		return false;
	}

	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* BuildingSubsystem =
		World->GetSubsystem<UBuildingWorldSubsystem>();
	UDoorBuildingDefinition* DoorDefinition =
		BuildingSubsystem
			? Cast<UDoorBuildingDefinition>(
				BuildingSubsystem->FindDefinition(TEXT("Door")))
			: nullptr;
	const FBuildEntityHandle DoorEntity = BuildingSubsystem
		? BuildingSubsystem->CreateEntity(
			*DoorDefinition,
			FTransform(FRotator::ZeroRotator, FVector(200.0, 0.0, 0.0)))
		: FBuildEntityHandle();
	TestTrue(TEXT("Door 原子注册到 Building Runtime"), DoorEntity.IsSet());
	UFirePileBuildingDefinition* FirePileDefinition =
		BuildingSubsystem
			? Cast<UFirePileBuildingDefinition>(
				BuildingSubsystem->FindDefinition(TEXT("FirePile")))
			: nullptr;
	const FBuildEntityHandle FirePileEntity = BuildingSubsystem
		? BuildingSubsystem->CreateEntity(
			*FirePileDefinition,
			FTransform(FRotator::ZeroRotator, FVector(100.0, 0.0, 0.0)))
		: FBuildEntityHandle();
	TestTrue(TEXT("更近的不可交互 Fire Pile 注册成功"), FirePileEntity.IsSet());

	AElementSandboxPlayerController* Controller =
		World->SpawnActor<AElementSandboxPlayerController>();
	AElementSandboxCharacter* Character =
		World->SpawnActor<AElementSandboxCharacter>();
	TestNotNull(TEXT("创建带聚焦距离配置的角色"), Character);
	if (Controller && Character)
	{
		Controller->Possess(Character);
	}
	if (Controller && !Controller->HasActorBegunPlay())
	{
		Controller->DispatchBeginPlay();
	}
	UFocusHostComponent* Host = Controller
		? Controller->FindComponentByClass<UFocusHostComponent>()
		: nullptr;
	UBuildingFocusHighlightPresenterComponent* HighlightPresenter = Controller
		? Controller->FindComponentByClass<UBuildingFocusHighlightPresenterComponent>()
		: nullptr;
	TestNotNull(TEXT("测试 Controller 拥有 Focus Host"), Host);
	TestNotNull(TEXT("测试 Controller 拥有 Building Focus 高亮 Presenter"), HighlightPresenter);
	TestEqual(TEXT("角色默认聚焦距离是 3 米"),
		Character ? Character->GetFocusDistance() : 0.0,
		300.0);

	FFocusQueryContext Context;
	Context.ViewOrigin = FVector(0.0, 0.0, 50.0);
	Context.ViewDirection = FVector::ForwardVector;
	FBuildSpatialQueryScratch BuildingQueryScratch;
	TArray<FBuildSpatialRayHit> BuildingBroadphaseHits;
	if (BuildingSubsystem)
	{
		BuildingSubsystem->GetSpatialIndex().QueryRay(
			Context.ViewOrigin,
			Context.ViewDirection,
			300.0,
			BuildingQueryScratch,
			BuildingBroadphaseHits);
	}
	TestTrue(TEXT("更近的 Fire Pile 确实位于同一 Building Broad Phase 射线"),
		BuildingBroadphaseHits.ContainsByPredicate(
			[FirePileEntity](const FBuildSpatialRayHit& RayHit)
			{
				return RayHit.Entity == FirePileEntity;
			}));
	if (Host)
	{
		Host->EvaluateFocus(Context);
	}
	if (HighlightPresenter)
	{
		HighlightPresenter->RefreshHighlight();
	}

	const FFocusQueryHit* FocusedHit = Host ? Host->GetFocusedHit() : nullptr;
	TestNotNull(TEXT("Building BVH Query 命中 Door"), FocusedHit);
	const FBuildingFocusTarget* Target = FocusedHit
		? FocusedHit->Target.GetPtr<FBuildingFocusTarget>()
		: nullptr;
	TestTrue(TEXT("Focus Payload 直接保存 Building Entity"),
		Target && Target->Entity == DoorEntity);
	TestTrue(TEXT("射线命中 Door 的有效 Mesh Part"),
		Target && Target->PartId != INDEX_NONE);
	TestTrue(TEXT("更近的不可交互 Fire Pile 不抢占 Door Focus"),
		Target && Target->Entity != FirePileEntity);
	TestTrue(TEXT("当前命中 Building Part 生成本地高亮外壳"),
		HighlightPresenter
		&& HighlightPresenter->IsHighlightVisible()
		&& HighlightPresenter->GetHighlightedEntity() == DoorEntity
		&& Target
		&& HighlightPresenter->GetHighlightedPartId() == Target->PartId);
	TestTrue(TEXT("高亮使用不继承 PlayerController 隐藏状态的独立渲染 Actor"),
		HighlightPresenter
		&& IsValid(HighlightPresenter->GetHighlightActor())
		&& HighlightPresenter->GetHighlightActor()->GetOwner() != Controller
		&& !HighlightPresenter->GetHighlightActor()->IsHidden());
	FFocusInteractionPrompt Prompt;
	TestTrue(TEXT("关闭 Door 解析通用提示"),
		Host && Host->TryResolveFocusedPrompt(Prompt));
	TestEqual(TEXT("关闭状态提示开门"),
		Prompt.Text.ToString(), FString(TEXT("按 E 开门")));
	TestTrue(TEXT("聚焦且门稳定关闭时 E 提交互动请求"),
		Host && Host->HandleInteract());
	TestFalse(TEXT("同一 Door 已在活跃队列时拒绝重复互动"),
		Host && Host->HandleInteract());
	if (BuildingSubsystem)
	{
		FBuildEntityRegistry& Registry = BuildingSubsystem->GetRegistry();
			FBuildDoorStateFragment* DoorState =
				Registry.FindMutableFragment<FBuildDoorStateFragment>(DoorEntity);
			if (DoorState)
		{
			DoorState->State = EBuildDoorState::Open;
		}
		TestTrue(TEXT("打开 Door 解析通用提示"),
			Host && Host->TryResolveFocusedPrompt(Prompt));
		TestEqual(TEXT("打开状态提示关门"),
			Prompt.Text.ToString(), FString(TEXT("按 E 关门")));
		if (DoorState)
		{
			DoorState->State = EBuildDoorState::Opening;
		}
		TestFalse(TEXT("Opening 状态不产生提示"),
			Host && Host->TryResolveFocusedPrompt(Prompt));
		TestFalse(TEXT("Opening 状态拒绝 E 互动"),
			Host && Host->HandleInteract());
		if (DoorState)
		{
			DoorState->State = EBuildDoorState::Closing;
		}
		TestFalse(TEXT("Closing 状态不产生提示"),
			Host && Host->TryResolveFocusedPrompt(Prompt));
		TestFalse(TEXT("Closing 状态拒绝 E 互动"),
			Host && Host->HandleInteract());
		if (DoorState)
		{
			DoorState->State = EBuildDoorState::Closed;
		}
			TestTrue(TEXT("Door 的元素表现状态不参与交互提示"),
				Host && Host->TryResolveFocusedPrompt(Prompt));
			TestEqual(TEXT("关闭 Door 仍提示开门"),
				Prompt.Text.ToString(), FString(TEXT("按 E 开门")));
	}

	if (Character && Host)
	{
		Character->SetActorLocation(FVector(-400.0, 0.0, 0.0));
		Host->EvaluateFocus(Context);
	}
	if (HighlightPresenter)
	{
		HighlightPresenter->RefreshHighlight();
	}
	TestNull(TEXT("角色距离超过 3 米后失去 Building Focus"),
		Host ? Host->GetFocusedHit() : nullptr);
	TestFalse(TEXT("失去 Focus 后不再产生提示"),
		Host && Host->TryResolveFocusedPrompt(Prompt));
	TestFalse(TEXT("失去 Focus 后立即隐藏高亮外壳"),
		HighlightPresenter && HighlightPresenter->IsHighlightVisible());

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDoorInteractionResolverTest,
	"ElementSandbox.Focus.DoorInteractionResolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDoorInteractionResolverTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle Door = Registry.CreateEntity();
	TestTrue(TEXT("Door 添加状态 Fragment"),
		Registry.AddFragment(Door, FBuildDoorStateFragment()));
	EBuildDoorInteractionIntent Intent = EBuildDoorInteractionIntent::None;
	TestTrue(TEXT("Closed Door 解析为 Open 意图"),
		TryResolveBuildDoorInteraction(Registry, Door, Intent));
	TestTrue(TEXT("关闭门意图正确"), Intent == EBuildDoorInteractionIntent::Open);

	FBuildDoorStateFragment* DoorState =
		Registry.FindMutableFragment<FBuildDoorStateFragment>(Door);
	TestNotNull(TEXT("取得可写 DoorState"), DoorState);
	if (DoorState)
	{
		DoorState->State = EBuildDoorState::Open;
		TestTrue(TEXT("Open Door 解析为 Close 意图"),
			TryResolveBuildDoorInteraction(Registry, Door, Intent));
		TestTrue(TEXT("打开门意图正确"), Intent == EBuildDoorInteractionIntent::Close);

		DoorState->State = EBuildDoorState::Opening;
		TestFalse(TEXT("Opening 不产生意图"),
			TryResolveBuildDoorInteraction(Registry, Door, Intent));
		DoorState->State = EBuildDoorState::Closing;
		TestFalse(TEXT("Closing 不产生意图"),
			TryResolveBuildDoorInteraction(Registry, Door, Intent));
		DoorState->State = EBuildDoorState::Closed;
	}
	TestTrue(TEXT("元素表现状态不参与 Door 交互意图"),
		TryResolveBuildDoorInteraction(Registry, Door, Intent));
	TestTrue(TEXT("关闭门仍解析为 Open 意图"),
		Intent == EBuildDoorInteractionIntent::Open);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectPickupPromptTest,
	"ElementSandbox.Focus.WorldObjectPickupPrompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPickupPromptTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("WorldObjectPickupPrompt"),
		nullptr,
		true);
	if (!TestNotNull(TEXT("创建 WorldObject Prompt 测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);

	UWorldObjectWorldSubsystem* WorldObjects =
		World->GetSubsystem<UWorldObjectWorldSubsystem>();
	UWorldObjectItemCatalogSubsystem* Catalog =
		World->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
	UItemDefinition* StickItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Stick.DA_Stick"));
	UWorldObjectDefinition* StickDefinition = Catalog && StickItem
		? Catalog->FindWorldObjectDefinition(StickItem)
		: nullptr;
	if (!TestTrue(TEXT("木棍拾取 Catalog 测试环境有效"),
		WorldObjects && Catalog && Catalog->IsReady() && StickItem && StickDefinition))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	FWorldObjectCreateDesc StickDesc;
	StickDesc.Definition = StickDefinition;
	StickDesc.WorldTransform.SetLocation(FVector(150.0, 0.0, 50.0));
	StickDesc.MotionState = EWorldObjectMotionState::Dormant;
	const FWorldObjectEntityHandle StickEntity = WorldObjects->CreateEntity(StickDesc);
	const FWorldEntityId StickWorldEntityId = WorldObjects->GetWorldEntityId(StickEntity);
	TestTrue(TEXT("创建 Dormant 木棍"),
		StickEntity.IsSet() && StickWorldEntityId.IsSet());

	AElementSandboxPlayerController* Controller =
		World->SpawnActor<AElementSandboxPlayerController>();
	AElementSandboxCharacter* Character =
		World->SpawnActor<AElementSandboxCharacter>();
	if (Controller && Character)
	{
		Controller->Possess(Character);
	}
	if (Controller && !Controller->HasActorBegunPlay())
	{
		Controller->DispatchBeginPlay();
	}
	UWorldObjectFocusQueryComponent* Query = Controller
		? Controller->FindComponentByClass<UWorldObjectFocusQueryComponent>()
		: nullptr;
	UWorldObjectFocusHandler* Handler = Query ? Query->GetHandler() : nullptr;
	UFocusHostComponent* Host = Controller
		? Controller->FindComponentByClass<UFocusHostComponent>()
		: nullptr;
	if (!TestTrue(TEXT("WorldObject Prompt Handler 装配有效"),
		Controller && Character && Handler && Host))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	FWorldObjectFocusTarget Target;
	Target.WorldEntityId = StickWorldEntityId;
	FFocusQueryHit Hit;
	Hit.HitDistance = 150.0;
	Hit.HitLocation = FVector(150.0, 0.0, 50.0);
	Hit.Target = FInstancedStruct::Make<FWorldObjectFocusTarget>(Target);
	FFocusInteractionPrompt Prompt;
	TestTrue(TEXT("木棍 Catalog 回退产生拾取提示"),
		Handler->TryResolvePrompt(Hit, Prompt));
	TestTrue(TEXT("拾取提示包含物件名称和长按操作"),
		Prompt.Text.ToString().Contains(TEXT("木棍")) && Prompt.Text.ToString().Contains(TEXT("按住")));

	FWorldObjectInstanceInteractionBoundsFragment InstanceBounds;
	InstanceBounds.InteractionLocalBounds = FBox(
		FVector(-20.0, -10.0, -5.0),
		FVector(20.0, 10.0, 15.0));
	TestTrue(TEXT("添加实例 Bounds"),
		WorldObjects->GetRegistry().AddFragment(StickEntity, InstanceBounds));
	TestTrue(TEXT("实例 Bounds 不改变固定提示资格"),
		Handler->TryResolvePrompt(Hit, Prompt));

	FWorldObjectMotionFragment* Motion =
		WorldObjects->GetRegistry().FindMutableFragment<
			FWorldObjectMotionFragment>(StickEntity);
	if (Motion)
	{
		Motion->State = EWorldObjectMotionState::Physics;
	}
	TestTrue(TEXT("Physics WorldObject 保持拾取提示"),
		Handler->TryResolvePrompt(Hit, Prompt));
	if (Motion)
	{
		Motion->State = EWorldObjectMotionState::Dormant;
	}

	UStickWorldObjectDefinition* NonPickupDefinition =
		NewObject<UStickWorldObjectDefinition>(WorldObjects);
	NonPickupDefinition->DefinitionId = TEXT("Test.NonPickupWorldObject");
	TestTrue(TEXT("注册无 Item 映射的 Portable Definition"),
		WorldObjects->RegisterDefinition(*NonPickupDefinition));
	FWorldObjectCreateDesc NonPickupDesc;
	NonPickupDesc.Definition = NonPickupDefinition;
	NonPickupDesc.WorldTransform.SetLocation(FVector(80.0, 0.0, 50.0));
	NonPickupDesc.MotionState = EWorldObjectMotionState::Dormant;
	const FWorldObjectEntityHandle NonPickupEntity =
		WorldObjects->CreateEntity(NonPickupDesc);
	const FWorldEntityId NonPickupNetId =
		WorldObjects->GetWorldEntityId(NonPickupEntity);
	FWorldObjectFocusTarget NonPickupTarget;
	NonPickupTarget.WorldEntityId = NonPickupNetId;
	FFocusQueryHit NonPickupHit;
	NonPickupHit.HitDistance = 80.0;
	NonPickupHit.HitLocation = FVector(80.0, 0.0, 50.0);
	NonPickupHit.Target =
		FInstancedStruct::Make<FWorldObjectFocusTarget>(NonPickupTarget);
	TestFalse(TEXT("无 Catalog 映射的 WorldObject 不提示"),
		Handler->TryResolvePrompt(NonPickupHit, Prompt));

	FFocusQueryContext Context;
	Context.ViewOrigin = FVector(0.0, 0.0, 50.0);
	Context.ViewDirection = FVector::ForwardVector;
	Host->EvaluateFocus(Context);
	const FFocusQueryHit* FocusedHit = Host->GetFocusedHit();
	const FWorldObjectFocusTarget* FocusedTarget = FocusedHit
		? FocusedHit->Target.GetPtr<FWorldObjectFocusTarget>()
		: nullptr;
	TestTrue(TEXT("更近的不可拾取 WorldObject 不抢占后方木棍"),
		FocusedTarget && FocusedTarget->WorldEntityId == StickWorldEntityId);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
