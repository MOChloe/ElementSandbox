#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Presentation/WoodProductInstanceStore.h"
#include "Presentation/WoodProductBatchSubmitter.h"
#include "Presentation/DeferredHISMComponent.h"
#include "WorldObjects/WoodProductFlightMaterialSet.h"
#include "WorldObjects/WoodProductPresentationSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "HAL/PlatformProcess.h"
#include "Storage/WorldChunkCodec.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif

namespace
{
	FWoodProductFlight Flight(uint64 Id = 71)
	{
		FWoodProductFlight F;
		F.WorldEntityId = FWorldEntityId(Id); F.DefinitionId = TEXT("WorldObject.WoodBlock");
		F.BurstId = 8; F.BatchId = 1; F.Revision = 1;
		F.RestTransform = FWorldChunkCodec::QuantizeTransform(FTransform(
			FRotator(0, 37, 0), FVector(-4000, -5000, 20), FVector::OneVector));
		F.StartOffset = FVector3f(-1000, 0, 1000); F.ImpactOffset = FVector3f(0, 0, 25);
		F.Velocity = FVector3f(200, 0, 100); F.Acceleration = FVector3f(0, 0, -980);
		F.AngularVelocityDegrees = FVector3f(45, 20, 30); F.ImpactSeconds = 3; F.SettlingSeconds = 0.5f;
		F.LiftHeight = 80; F.Radius = 90; F.LocalStartTime = 1;
		return F;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoodFlightOwnershipTest, "ElementSandbox.WoodProducts.Flight.OwnershipAndInterest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoodFlightOwnershipTest::RunTest(const FString&)
{
	FWoodProductInstanceStore Store;
	auto F = Flight();
	TestTrue(TEXT("准备接受最终身份"), Store.AcceptFlight(F));
	F.Phase = EWoodProductFlightPhase::Active;
	TestTrue(TEXT("激活同一目录行"), Store.AcceptFlight(F));
	Store.Upsert(F.WorldEntityId, F.DefinitionId, F.RestTransform);
	TestFalse(TEXT("Upsert 后迟到激活不重新起飞"), Store.AcceptFlight(F));
	Store.Remove(F.WorldEntityId);
	TestFalse(TEXT("拾取后迟到页不复活"), Store.AcceptFlight(F));
	Store.Upsert(F.WorldEntityId, F.DefinitionId, F.RestTransform);
	TestFalse(TEXT("缓存重入普通对象后仍不能起飞"), Store.AcceptFlight(F));
	TestFalse(TEXT("缓存重入清除移除状态"), Store.Entries.FindChecked(F.WorldEntityId).bRemoved);

	auto Canceled = Flight(72); Canceled.Phase = EWoodProductFlightPhase::Canceled;
	Store.AcceptFlight(Canceled); Canceled.Phase = EWoodProductFlightPhase::Prepared;
	TestFalse(TEXT("取消先到时不能创建"), Store.AcceptFlight(Canceled));
	auto Settled = Flight(73); Settled.Phase = EWoodProductFlightPhase::Settled; Settled.Radius = 0;
	TestTrue(TEXT("Settlement 可先于 Payload"), Store.AcceptFlight(Settled));
	TestFalse(TEXT("只收到 Settlement 不凭空创建实例参数"), Store.FindFlight(Store.Entries.FindChecked(Settled.WorldEntityId)) != nullptr);
	Settled = Flight(73); Store.AcceptFlight(Settled);
	TestTrue(TEXT("后到 Payload 保持已落地"), Store.FindFlight(Store.Entries.FindChecked(Settled.WorldEntityId))->Phase == EWoodProductFlightPhase::Settled);

	Store.SetRetentionBoxes({FWorldChunkBox::Centered({-1, -1, 0}, 4), FWorldChunkBox::Centered({2000, 2000, 0}, 4)});
	auto Near = Flight(74); Store.AcceptFlight(Near);
	TestFalse(TEXT("负坐标兴趣保留准备对象"), Store.Entries.FindChecked(Near.WorldEntityId).bRemoved);
	Store.SetRetentionBoxes({FWorldChunkBox::Centered({2000, 2000, 0}, 4)});
	TestTrue(TEXT("传送离开轨迹包络后释放临时对象"), Store.Entries.FindChecked(Near.WorldEntityId).bRemoved);
	TestFalse(TEXT("普通对象交由正式生命周期管理"), Store.Entries.FindChecked(F.WorldEntityId).bRemoved);
	TestFalse(TEXT("传送后的迟到页不能复活"), Store.AcceptFlight(Near));
	Store.RetireBurst(8);
	TestFalse(TEXT("过期 Burst 永久拒绝"), Store.AcceptFlight(Flight(99)));
	TestEqual(TEXT("退休后不累积历史逐实例墓碑"), Store.TerminalFlights.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoodFlightNativeInstanceTest, "ElementSandbox.WoodProducts.Flight.NativeInstanceContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoodFlightNativeInstanceTest::RunTest(const FString&)
{
	auto* Materials = LoadObject<UWoodProductFlightMaterialSet>(nullptr, TEXT("/Game/WorldObjects/WoodBlock/DA_WoodProductFlightMaterials.DA_WoodProductFlightMaterials"));
	if (!TestNotNull(TEXT("生成的原生材质集"), Materials)) return false;
#if WITH_EDITOR
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
#endif
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("WoodFlightNativeTest"), nullptr, true);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Host = World->SpawnActor<AActor>();
	auto* Root = NewObject<USceneComponent>(Host); Host->AddInstanceComponent(Root); Host->SetRootComponent(Root); Root->RegisterComponent();
	auto* Settings = NewObject<UWoodProductPresentationSettings>(World);
	Settings->InstanceApplyTargetMilliseconds = 100; Settings->TreeBuildQuietSeconds = 0;
	FWoodProductInstanceStore Store; FWoodProductBatchSubmitter Submit;
	auto A = Flight(81), B = Flight(82), C = Flight(83);
	Store.AcceptFlight(A); Store.AcceptFlight(B); Store.AcceptFlight(C);
	Submit.Tick(Store, *Host, *Materials, *Settings, 0);
	auto& Entry = Store.Entries.FindChecked(A.WorldEntityId);
	auto* Component = Store.Groups.FindChecked(Entry.Group)->Component.Get();
	const int32 Index = Entry.Index;
	TestEqual(TEXT("Prepare 已实际创建隐藏实例"), Component->GetInstanceCount(), 3);
	TestFalse(TEXT("全 Prepared 组不提交整组件绘制"), Component->IsVisible());
	TestEqual(TEXT("准备参数保持隐藏"), Component->PerInstanceSMCustomData[Index * FWoodProductFlight::CustomFloatCount + FWoodProductFlight::PhaseIndex], 0.0f);
	FTransform InitialTransform; Component->GetInstanceTransform(Index, InitialTransform, true);
	TestTrue(TEXT("实例从准备起就在最终姿态"), InitialTransform.Equals(A.RestTransform, 0.01));
	const uint64 BuildsBefore = Component->GetTreeBuildCount();
	Component->BuildTreeIfOutdated(true, true); // 模拟 Register/材质等路径发出的平台时钟请求。
	Settings->TreeBuildQuietSeconds = 0.01;
	FPlatformProcess::Sleep(0.02f);
	Submit.Tick(Store, *Host, *Materials, *Settings, 0.01);
	TestTrue(TEXT("游戏时间与平台时间不同也会启动原生建树"), Component->GetTreeBuildCount() > BuildsBefore);
	A.Phase = EWoodProductFlightPhase::Active;
	Store.AcceptFlight(A);
	// 模拟维护已耗尽软预算：已到达的激活至少推进一个有界批次，不能无限停在隐藏态。
	Settings->InstanceApplyTargetMilliseconds = 0.000001;
	Submit.Tick(Store, *Host, *Materials, *Settings, 1);
	TestTrue(TEXT("首个成员激活恢复整组绘制"), Component->IsVisible());
	TestEqual(TEXT("预算耗尽时仍提交激活参数"),
		Component->PerInstanceSMCustomData[Index * FWoodProductFlight::CustomFloatCount + FWoodProductFlight::PhaseIndex], 1.0f);
	Settings->InstanceApplyTargetMilliseconds = 100;
	TestEqual(TEXT("混合组的 Prepared 成员继续由 Shader 隐藏"),
		Component->PerInstanceSMCustomData[Store.Entries.FindChecked(B.WorldEntityId).Index * FWoodProductFlight::CustomFloatCount + FWoodProductFlight::PhaseIndex], 0.0f);
	B.Phase = C.Phase = EWoodProductFlightPhase::Active;
	Store.AcceptFlight(B); Store.AcceptFlight(C);
	Submit.Tick(Store, *Host, *Materials, *Settings, 1.01);
	for (int32 Step = 1; Step < 20; ++Step) Submit.Tick(Store, *Host, *Materials, *Settings, 1 + Step * 0.1);
	TestEqual(TEXT("正常飞行零 CPU Transform 更新"), Store.TotalTransformUpdates, uint64(0));
	FWorldChunkData Snapshot;
	Snapshot.Coord = FWorldChunkCoord::FromWorldLocation(A.RestTransform.GetLocation()); Snapshot.Revision = 1;
	FWorldPersistentEntityRecord Record;
	Record.EntityId = A.WorldEntityId; Record.Domain = EWorldEntityDomain::WorldObject;
	Record.DefinitionId = A.DefinitionId; Record.WorldTransform = A.RestTransform;
	Snapshot.Records.Add(Record);
	TArray<uint8> Bytes; FString Error; FWorldChunkData Decoded;
	TestTrue(TEXT("落地 Snapshot 编码"), FWorldChunkCodec::Encode(Snapshot, Bytes, Error));
	TestTrue(TEXT("落地 Snapshot 解码"), FWorldChunkCodec::Decode(Bytes, Decoded, Error));
	if (!Decoded.Records.IsEmpty()) Store.Upsert(A.WorldEntityId, A.DefinitionId, Decoded.Records[0].WorldTransform);
	Store.Upsert(B.WorldEntityId, B.DefinitionId, B.RestTransform);
	Store.Upsert(C.WorldEntityId, C.DefinitionId, C.RestTransform);
	Submit.Tick(Store, *Host, *Materials, *Settings, 5);
	Submit.Tick(Store, *Host, *Materials, *Settings, 5.1);
	TestTrue(TEXT("落地认领保留原组件"), Store.Groups.FindChecked(Entry.Group)->Component.Get() == Component);
	TestEqual(TEXT("落地认领保留原槽位"), Entry.Index, Index);
	TestEqual(TEXT("全程只创建一次"), Store.TotalAdds, uint64(3));
	TestEqual(TEXT("落地没有 Remove"), Store.TotalRemoves, uint64(0));
	TestEqual(TEXT("量化 Snapshot 认领也不更新 Transform"), Store.TotalTransformUpdates, uint64(0));
	TestEqual(TEXT("飞行结束释放自定义数据"), Component->NumCustomDataFloats, 0);
	TestTrue(TEXT("同组件换回静态木材"), Component->GetMaterial(0) == Materials->StaticWood);
	TestTrue(TEXT("落地认领后仍可见"), Component->IsVisible());
	TestFalse(TEXT("飞行参数释放"), Store.FindFlight(Entry) != nullptr);
	TestEqual(TEXT("临时参数的连续存储实际释放内存"), Store.FlightData.GetAllocatedSize(), SIZE_T(0));
	Store.Remove(B.WorldEntityId); Submit.Tick(Store, *Host, *Materials, *Settings, 6);
	TestEqual(TEXT("拾取精确移除一个槽位"), Component->GetInstanceCount(), 2);
	const auto& Moved = Store.Entries.FindChecked(C.WorldEntityId);
	TestTrue(TEXT("SwapRemove 更新身份目录"), Store.Groups.FindChecked(Moved.Group)->Owners[Moved.Index] == C.WorldEntityId);

	// 已收回 WPO 的物理组接收迟到 Prepared 时，原有普通实例不能随之隐藏。
	auto Late = Flight(84);
	Store.AcceptFlight(Late); Submit.Tick(Store, *Host, *Materials, *Settings, 6.1);
	TestTrue(TEXT("迟到同 Cell 成员继续使用原组件"), Store.Groups.FindChecked(Store.Entries.FindChecked(Late.WorldEntityId).Group)->Component.Get() == Component);
	TestTrue(TEXT("静态组重新打开 WPO 后保留普通实例可见性"), Component->IsVisible());
	Late.Phase = EWoodProductFlightPhase::Canceled;
	Store.AcceptFlight(Late); Submit.Tick(Store, *Host, *Materials, *Settings, 6.2);

	auto D = Flight(85), E = Flight(86);
	D.BurstId = 9; D.BatchId = 2;
	E.BurstId = 10; E.BatchId = 3;
	E.StartOffset = FVector3f(-7000, 0, 1000);
	const int32 DTier = Materials->FindTier(D.GetDisplacementExtent());
	const int32 ETier = Materials->FindTier(E.GetDisplacementExtent());
	TestTrue(TEXT("测试轨迹确实跨越不同 WPO 档位"), DTier != INDEX_NONE && ETier > DTier);
	Store.AcceptFlight(D); Store.AcceptFlight(E);
	Submit.Tick(Store, *Host, *Materials, *Settings, 6.3);
	const auto OtherKey = Store.Entries.FindChecked(E.WorldEntityId).Group;
	auto* OtherComponent = Store.Groups.FindChecked(OtherKey)->Component.Get();
	const auto& OtherGroup = *Store.Groups.FindChecked(OtherKey);
	TestTrue(TEXT("Burst/Page/WPO 档位均不拆分物理 HISM"), OtherComponent == Component);
	TestEqual(TEXT("不同协议批次和 WPO 档位仍只有一个渲染组"), Store.Groups.Num(), 1);
	TestEqual(TEXT("共享组原地升级到所需最高 WPO 档位"), OtherGroup.FlightMaterialTier, ETier);
	TestTrue(TEXT("共享组件使用最高 WPO 档位材质"), OtherComponent->GetMaterial(0) == Materials->GetMaterial(false, ETier));
	TestTrue(TEXT("已落地成员使共享组保持可见"), OtherComponent->IsVisible());
	TestEqual(TEXT("共享组中的 Prepared 成员由 Shader 隐藏"),
		OtherComponent->PerInstanceSMCustomData[
			Store.Entries.FindChecked(E.WorldEntityId).Index * FWoodProductFlight::CustomFloatCount
			+ FWoodProductFlight::PhaseIndex], 0.0f);
	D.Phase = EWoodProductFlightPhase::Active; D.LocalStartTime = 7;
	Store.AcceptFlight(D); Submit.Tick(Store, *Host, *Materials, *Settings, 7);
	TestTrue(TEXT("混合组激活可见"), OtherComponent->IsVisible());
	D.Phase = EWoodProductFlightPhase::Canceled;
	Store.AcceptFlight(D); Submit.Tick(Store, *Host, *Materials, *Settings, 7.1);
	TestTrue(TEXT("移除飞行成员不得隐藏已落地成员"), OtherComponent->IsVisible());
	TestEqual(TEXT("取消只移除目标，已落地与另一准备成员都保留"), OtherComponent->GetInstanceCount(), 3);
	Store.Upsert(E.WorldEntityId, E.DefinitionId, E.RestTransform);
	Submit.Tick(Store, *Host, *Materials, *Settings, 7.2);
	TestTrue(TEXT("普通 Upsert 直接认领 Prepared 时恢复可见"), OtherComponent->IsVisible());
	TestTrue(TEXT("认领未更换组件"), Store.Groups.FindChecked(Store.Entries.FindChecked(E.WorldEntityId).Group)->Component.Get() == OtherComponent);
	Store.Remove(A.WorldEntityId); Store.Remove(C.WorldEntityId); Store.Remove(E.WorldEntityId);
	Submit.Tick(Store, *Host, *Materials, *Settings, 7.3);
	Submit.Tick(Store, *Host, *Materials, *Settings, 7.4);
	TestFalse(TEXT("空组回收组件"), Store.Groups.Contains(OtherKey));
	GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoodFlightTierTest, "ElementSandbox.WoodProducts.Flight.DisplacementTiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoodFlightTierTest::RunTest(const FString&)
{
	TestEqual(TEXT("32m 使用首档"), UWoodProductFlightMaterialSet::ComputeTier(3200), 0);
	TestEqual(TEXT("超过32m进入64m档"), UWoodProductFlightMaterialSet::ComputeTier(3201), 1);
	auto F = Flight();
	F.StartOffset = FVector3f(0, 0, 100); F.Velocity = FVector3f(0, 0, 3000);
	F.ImpactSeconds = 6;
	const float Envelope = F.GetDisplacementExtent();
	TestTrue(TEXT("轨迹包络覆盖中途最高点"), Envelope > 4600);
	for (int32 Step = 0; Step <= 120; ++Step)
	{
		const float T = F.ImpactSeconds * Step / 120.0f;
		const FVector3f Center = F.StartOffset + F.Velocity * T + F.Acceleration * (0.5f * T * T);
		TestTrue(TEXT("全部飞行位置和任意旋转均在 WPO 档内"), Center.GetAbsMax() + 2 * F.Radius <= Envelope);
	}
	return true;
}
#endif
