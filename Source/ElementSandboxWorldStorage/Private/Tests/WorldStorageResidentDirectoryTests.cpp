#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Storage/WorldChunkCodec.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Network/WorldChunkActivationReadiness.h"
#include "WorldStorageSubsystem.h"

namespace ElementSandbox::WorldStorage::Tests
{
	class FTestWorldStorageAdapter final : public IWorldStorageDomainAdapter
	{
	public:
		explicit FTestWorldStorageAdapter(const EWorldEntityDomain InDomain)
			: Domain(InDomain)
		{
		}

		virtual EWorldEntityDomain GetDomain() const override { return Domain; }
		virtual EWorldStorageRestorePhase GetRestorePhase() const override
		{
			return EWorldStorageRestorePhase::Primary;
		}

                virtual bool CaptureBatch(
			const TConstArrayView<FWorldEntityId> EntityIds,
			TArray<FWorldPersistentEntityRecord>& OutRecords,
			FString& OutError) const override
                {
						++CaptureBatchCalls;
			OutRecords.Reset();
			for (const FWorldEntityId EntityId : EntityIds)
			{
				const FWorldPersistentEntityRecord* Record = Records.Find(EntityId);
				if (!Record)
				{
					OutError = TEXT("测试 Adapter 缺少 Capture 记录。");
					return false;
				}
				OutRecords.Add(*Record);
			}
			return true;
		}

		virtual bool RestoreBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldPersistentEntityRecord> InRecords,
			FString& OutError) override
		{
			++RestoreBatchCalls;
			RestoreBatchSizes.Add(InRecords.Num());
			for (const FWorldPersistentEntityRecord& Record : InRecords)
			{
				if (Record.Domain != Domain
					|| FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != HomeChunk)
				{
					OutError = TEXT("测试 Adapter 收到错误领域或 HomeChunk。");
					return false;
				}
			}
			for (const FWorldPersistentEntityRecord& Record : InRecords)
			{
				Records.Add(Record.EntityId, Record);
			}
			if (OnRestore)
			{
				OnRestore();
			}
			return true;
		}

		virtual bool RuntimeEvictBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return RemoveBatch(HomeChunk, EntityIds, EvictedIds, OutError);
		}

		virtual bool GameplayDestroyBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return RemoveBatch(HomeChunk, EntityIds, GameplayDestroyedIds, OutError);
		}

		virtual bool LeaveInterestBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return RemoveBatch(HomeChunk, EntityIds, LeftInterestIds, OutError);
		}

		virtual bool RollbackRestoreBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return RemoveBatch(HomeChunk, EntityIds, RolledBackIds, OutError);
		}

		virtual bool CanRuntimeEvict(const FWorldEntityId EntityId) const override
		{
			return !PinnedIds.Contains(EntityId);
		}

		void Add(const FWorldEntityId EntityId, const FVector& Location, const uint32 Revision = 1)
		{
			FWorldPersistentEntityRecord Record;
			Record.EntityId = EntityId;
			Record.Domain = Domain;
			Record.DefinitionId = TEXT("Test.Residency");
			Record.WorldTransform = FTransform(Location);
			Record.StateRevision = Revision;
			Records.Add(EntityId, MoveTemp(Record));
		}

		EWorldEntityDomain Domain = EWorldEntityDomain::Invalid;
		TMap<FWorldEntityId, FWorldPersistentEntityRecord> Records;
		TSet<FWorldEntityId> PinnedIds;
		TSet<FWorldEntityId> EvictedIds;
		TSet<FWorldEntityId> GameplayDestroyedIds;
		TSet<FWorldEntityId> LeftInterestIds;
		TSet<FWorldEntityId> RolledBackIds;
				mutable int32 CaptureBatchCalls = 0;
		int32 RestoreBatchCalls = 0;
		TArray<int32> RestoreBatchSizes;
		TFunction<void()> OnRestore;

	private:
		bool RemoveBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			TSet<FWorldEntityId>& RemovedIds,
			FString& OutError)
		{
			for (const FWorldEntityId EntityId : EntityIds)
			{
				const FWorldPersistentEntityRecord* Record = Records.Find(EntityId);
				if (!Record
					|| FWorldChunkCoord::FromWorldLocation(Record->WorldTransform.GetLocation()) != HomeChunk)
				{
					OutError = TEXT("测试 Adapter 无法原子 Evict 记录。");
					return false;
				}
			}
			for (const FWorldEntityId EntityId : EntityIds)
			{
				Records.Remove(EntityId);
				RemovedIds.Add(EntityId);
			}
			return true;
		}
	};

	class FScopedResidentDirectoryWorld final
	{
	public:
		FScopedResidentDirectoryWorld()
		{
			World = UWorld::CreateWorld(
				EWorldType::Game,
				false,
				TEXT("WorldStorageResidentDirectory"),
				nullptr,
				true);
			if (World)
			{
				GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			}
		}

		~FScopedResidentDirectoryWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
	};

	class FScopedNetworkClientWorld final
	{
	public:
		FScopedNetworkClientWorld()
		{
			UWorld::InitializationValues InitializationValues;
			InitializationValues
				.CreatePhysicsScene(false)
				.ShouldSimulatePhysics(false)
				.EnableTraceCollision(false)
				.CreateNavigation(false)
				.CreateAISystem(false);
			World = UWorld::CreateWorld(
				EWorldType::PIE,
				false,
				TEXT("WorldStorageNetworkClientBatch"),
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&InitializationValues,
				true);
			if (World)
			{
				GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
				World->SetPlayInEditorInitialNetMode(NM_Client);
				World->InitWorld(InitializationValues);
				World->UpdateWorldComponents(true, false);
			}
		}

		~FScopedNetworkClientWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageNetworkUpsertUsesDomainBatchesTest,
	"ElementSandbox.WorldStorage.Network.LiveDeltaUpsertUsesDomainBatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageNetworkUpsertUsesDomainBatchesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedNetworkClientWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("Network Upsert Batch 客户端 WorldStorage 已初始化"), Storage))
	{
		return false;
	}
	TestFalse(TEXT("测试 WorldStorage 使用 Client 进程职责"), Storage->IsAuthorityStorage());
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::WorldObject);
	if (!TestTrue(TEXT("Network Upsert Batch Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter)))
	{
		return false;
	}

	auto MakeRecord = [](const uint64 Value, const FVector& Location, const uint32 Revision = 1)
	{
		FWorldPersistentEntityRecord Record;
		Record.EntityId = FWorldEntityId(Value);
		Record.Domain = EWorldEntityDomain::WorldObject;
		Record.DefinitionId = TEXT("Test.NetworkBatch");
		Record.WorldTransform = FTransform(Location);
		Record.StateRevision = Revision;
		return Record;
	};
	TArray<FWorldPersistentEntityRecord> Records;
	Records.Add(MakeRecord(12001, FVector(100.0, 100.0, 100.0)));
	Records.Add(MakeRecord(12002, FVector(200.0, 100.0, 100.0)));
	Records.Add(MakeRecord(12003, FVector(300.0, 100.0, 100.0)));
	Records.Add(MakeRecord(12004, FVector(100100.0, 100.0, 100.0)));
	TestTrue(TEXT("整批 Network Upsert 应用成功"), Storage->ApplyNetworkUpsertBatch(Records));
	TestEqual(TEXT("同一 Domain/HomeChunk 只调用一次 RestoreBatch"), Adapter->RestoreBatchCalls, 2);
	TestEqual(TEXT("第一个 Chunk 的三条记录保持为同一批"), Adapter->RestoreBatchSizes[0], 3);
	for (const FWorldPersistentEntityRecord& Record : Records)
	{
		TestTrue(TEXT("批量恢复后 Resident Directory 可命中"), Storage->IsResident(Record.EntityId));
	}

	TestTrue(TEXT("重复同 Revision Upsert 是幂等确认"), Storage->ApplyNetworkUpsertBatch(Records));
	TestEqual(TEXT("幂等确认不再次进入 Domain Adapter"), Adapter->RestoreBatchCalls, 2);

	TArray<FWorldPersistentEntityRecord> Duplicated = Records;
	Duplicated.Add(Records[0]);
	TestFalse(TEXT("同一网络批次中的重复 WorldEntityId 被整批拒绝"),
		Storage->ApplyNetworkUpsertBatch(Duplicated));
	TestEqual(TEXT("预检失败不会部分调用 Domain Adapter"), Adapter->RestoreBatchCalls, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageNetworkSnapshotJoinsInFlightRestoreTest,
	"ElementSandbox.WorldStorage.Network.DuplicateSnapshotWaitsForRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageNetworkSnapshotJoinsInFlightRestoreTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedNetworkClientWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("Snapshot 去重测试 Client WorldStorage 已初始化"), Storage)) return false;
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::WorldObject);
	if (!TestTrue(TEXT("Snapshot 去重 Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter))) return false;
	const FVector Location(-50.0, 50.0, 50.0);
	FWorldResidencySourceHandle Source = Storage->RegisterResidencySource(Location);
	FWorldChunkData Data;
	Data.Coord = FWorldChunkCoord::FromWorldLocation(Location);
	Data.Revision = 1;
	for (uint64 Index = 0; Index < 16; ++Index)
	{
		FWorldPersistentEntityRecord& Record = Data.Records.AddDefaulted_GetRef();
		Record.EntityId = FWorldEntityId(50000 + Index);
		Record.Domain = EWorldEntityDomain::WorldObject;
		Record.DefinitionId = TEXT("Test.SnapshotJoin");
		Record.StateRevision = 1;
		Record.WorldTransform = FTransform(Location + FVector(double(Index), 0.0, 0.0));
	}
	FWorldCompressedChunk Compressed;
	FString Error;
	if (!TestTrue(TEXT("测试 Chunk 可压缩"), FWorldChunkCodec::Compress(Data, Compressed, Error))) return false;
	const FGuid WorldId = FGuid::NewGuid();
	int32 CompletionCount = 0;
	const auto Completion = [&](const bool bSuccess, const FString&, FWorldCompressedChunk&& Chunk)
	{
		++CompletionCount;
		TestTrue(TEXT("重复请求都等待整份 Snapshot 成功"), bSuccess);
		TestEqual(TEXT("回调时全部实体已经完成 Restore"), Adapter->Records.Num(), Data.Records.Num());
		TestTrue(TEXT("每个等待者得到同一份可缓存快照"), Chunk.ContentHash == Compressed.ContentHash
			&& Chunk.Bytes == Compressed.Bytes);
	};
	bool bJoinedDuringInjection = false;
	Adapter->OnRestore = [&]
	{
		if (!bJoinedDuringInjection)
		{
			bJoinedDuringInjection = true;
			TestTrue(TEXT("PendingInjection 期间的重复请求加入当前工作"),
				Storage->SubmitNetworkChunk(WorldId, Compressed, Completion));
			TestEqual(TEXT("领域 Restore 尚未返回时不得提前 ACK"), CompletionCount, 0);
		}
	};
	TestTrue(TEXT("首个 Snapshot 被接收"), Storage->SubmitNetworkChunk(WorldId, Compressed, Completion));
	TestTrue(TEXT("Loading 期间的重复 Snapshot 不被拒绝"),
		Storage->SubmitNetworkChunk(WorldId, Compressed, Completion));
	TestEqual(TEXT("解码前无完成通知"), CompletionCount, 0);
	Data.Records[0].StateRevision = 2;
	FWorldCompressedChunk DifferentHash;
	if (!TestTrue(TEXT("不同内容的同 Revision 快照可压缩"),
		FWorldChunkCodec::Compress(Data, DifferentHash, Error))) return false;
	TestFalse(TEXT("同 Revision 不同 Hash 不能冒充重复请求"),
		Storage->SubmitNetworkChunk(WorldId, DifferentHash, {}));
	for (int32 Attempt = 0; Attempt < 400 && CompletionCount < 3; ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
	}
	TestTrue(TEXT("测试覆盖正在注入时重入"), bJoinedDuringInjection);
	TestEqual(TEXT("三个等待者各收到一次完成通知"), CompletionCount, 3);
	TestEqual(TEXT("同一快照只提交一次解码字节"), Storage->GetRuntimeStats().BytesReceived,
		uint64(Compressed.Bytes.Num()));
	const int32 RestoreCalls = Adapter->RestoreBatchCalls;
	TestTrue(TEXT("已 Resident 的快照立即幂等完成"),
		Storage->SubmitNetworkChunk(WorldId, Compressed, Completion));
	TestEqual(TEXT("Resident 快照不再次调用 Adapter"), Adapter->RestoreBatchCalls, RestoreCalls);
	TestEqual(TEXT("Resident 请求只回调一次"), CompletionCount, 4);
	Adapter->OnRestore = {};
	Storage->UnregisterResidencySource(Source);
	Storage->Tick(0.0f);
	TestEqual(TEXT("离开 Retention 后清理实体"), Adapter->Records.Num(), 0);
	Source = Storage->RegisterResidencySource(Location);
	TestTrue(TEXT("完成后的等待者记录不阻挡驱逐后重入"),
		Storage->SubmitNetworkChunk(WorldId, Compressed, Completion));
	for (int32 Attempt = 0; Attempt < 400 && CompletionCount < 5; ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
	}
	TestEqual(TEXT("重新进入后再次完整恢复"), CompletionCount, 5);
	Storage->UnregisterResidencySource(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageNetworkSnapshotFailureNotifiesAllWaitersTest,
	"ElementSandbox.WorldStorage.Network.DuplicateSnapshotSharesFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageNetworkSnapshotFailureNotifiesAllWaitersTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedNetworkClientWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("失败去重测试 Client WorldStorage 已初始化"), Storage)) return false;
	const FWorldResidencySourceHandle Source = Storage->RegisterResidencySource(FVector::ZeroVector);
	FWorldChunkData Data;
	FWorldCompressedChunk Good;
	FString Error;
	if (!TestTrue(TEXT("空快照压缩成功"), FWorldChunkCodec::Compress(Data, Good, Error))) return false;
	FWorldCompressedChunk Bad = Good;
	Bad.ContentHash.Low ^= 1;
	const FGuid WorldId = FGuid::NewGuid();
	int32 Failures = 0;
	const auto Failed = [&](const bool bSuccess, const FString& Reason, FWorldCompressedChunk&&)
	{
		++Failures;
		TestFalse(TEXT("损坏快照不得给任何等待者成功 ACK"), bSuccess);
		TestTrue(TEXT("失败通知包含解码错误"), Reason.Contains(TEXT("ContentHash")));
	};
	AddExpectedError(TEXT("加载失败"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("首个快照进入异步校验"), Storage->SubmitNetworkChunk(WorldId, Bad, Failed));
	TestTrue(TEXT("重复请求等待同一次校验"), Storage->SubmitNetworkChunk(WorldId, Bad, Failed));
	for (int32 Attempt = 0; Attempt < 400 && Failures < 2; ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
	}
	TestEqual(TEXT("两个等待者都收到唯一失败通知"), Failures, 2);
	TestFalse(TEXT("失败快照没有建立 Resident 基线"), Storage->IsChunkResident(Data.Coord));
	bool bRecovered = false;
	TestTrue(TEXT("失败完成后允许同 Revision 的正确快照重试"),
		Storage->SubmitNetworkChunk(WorldId, Good,
			[&](const bool bSuccess, const FString&, FWorldCompressedChunk&&) { bRecovered = bSuccess; }));
	for (int32 Attempt = 0; Attempt < 400 && !bRecovered; ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
	}
	TestTrue(TEXT("正确快照恢复成功，旧等待者没有残留"), bRecovered);
	TestEqual(TEXT("重试不会再次通知失败请求"), Failures, 2);
	Storage->UnregisterResidencySource(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageEmptyActivationCoreBaselineTest,
	"ElementSandbox.WorldStorage.Network.EmptyActivationCorePublishesBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageEmptyActivationCoreBaselineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::WorldStorage::Tests;
	using namespace UE::ElementSandbox::WorldStorage::Private;
	FScopedResidentDirectoryWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("空 Activation Core 测试 WorldStorage 已初始化"), Storage))
	{
		return false;
	}

	const FWorldResidencySourceHandle Source = Storage->RegisterResidencySource(FVector::ZeroVector);
	if (!TestTrue(TEXT("空 Activation Core 测试来源注册成功"), Source.IsSet()))
	{
		return false;
	}
	TArray<FWorldChunkCoord> Core;
	BuildDenseActivationCore(FWorldChunkCoord::FromWorldLocation(FVector::ZeroVector), Core);
	TArray<FWorldChunkOffer> Offers;
	TSet<FWorldChunkCoord> Relevant;
	Storage->GetRelevantChunkOffers(Source, Core, {}, Offers, Relevant);
	TestEqual(TEXT("即使 Archive 全空，协议相关集也包含全部 27 个 Core 坐标"), Relevant.Num(), 27);

	for (int32 Attempt = 0; Attempt < 400 && Offers.Num() != Core.Num(); ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
		Storage->GetRelevantChunkOffers(Source, Core, {}, Offers, Relevant);
	}
	TestEqual(TEXT("空 Core Chunk 也会发布可 ACK 的 Snapshot Offer"), Offers.Num(), Core.Num());
	for (const FWorldChunkCoord& Coord : Core)
	{
		TestTrue(TEXT("已知空 Core 在 Authority 端可进入激活基线"),
			Storage->IsAuthorityChunkReadyForActivation(Coord));
		const FWorldChunkOffer* Offer = Offers.FindByPredicate(
			[Coord](const FWorldChunkOffer& Candidate) { return Candidate.Coord == Coord; });
		TestTrue(TEXT("每个空 Core 坐标都有完整 Offer 元数据"),
			Offer && Offer->WorldId.IsValid() && Offer->Revision > 0 && Offer->ContentHash.IsSet() &&
				Offer->CompressedSize > 0 && Offer->UncompressedSize > 0);
	}
	Storage->UnregisterResidencySource(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageAcknowledgedChunkDoesNotPrepareSnapshotTest,
	"ElementSandbox.WorldStorage.Network.AcknowledgedChunkSkipsSnapshotPreparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageAcknowledgedChunkDoesNotPrepareSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedResidentDirectoryWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("Offer 测试 WorldStorage 已初始化"), Storage)) return false;
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::WorldObject);
	if (!TestTrue(TEXT("Offer 测试 Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter))) return false;

	const FWorldChunkCoord Coord(-1, 0, 0);
	const FVector Location(-50.0, 50.0, 50.0);
	const FWorldResidencySourceHandle Source = Storage->RegisterResidencySource(Location);
	const FWorldEntityId EntityId = Storage->AllocateEntityId();
	Adapter->Add(EntityId, Location);
	FWorldResidentEntityRegistration Registration;
	Registration.EntityId = EntityId;
	Registration.Domain = EWorldEntityDomain::WorldObject;
	Registration.HomeChunk = Coord;
	Registration.StateRevision = 1;
	TestEqual(TEXT("登记测试实体"), Storage->RegisterResidentEntity(Registration),
		EWorldResidentUpsertResult::Inserted);
	TestTrue(TEXT("测试 Chunk 已变脏"), Storage->MarkEntityDirty(EntityId, 1));
	const TArray<FWorldChunkCoord> Core{Coord};
	const TSet<FWorldChunkCoord> Acknowledged{Coord};
	TArray<FWorldChunkOffer> Offers;
	TSet<FWorldChunkCoord> Relevant;
	const int32 CapturesBeforeOffer = Adapter->CaptureBatchCalls;
	Storage->GetRelevantChunkOffers(Source, Core, Acknowledged, Offers, Relevant);
	TestEqual(TEXT("已 ACK 的 Dirty Chunk 不再同步 Capture 完整快照"),
		Adapter->CaptureBatchCalls, CapturesBeforeOffer);
	TestTrue(TEXT("已 ACK 的 Chunk 仍在完整兴趣集中，不能被误移除"), Relevant.Contains(Coord));
	TestTrue(TEXT("已 ACK 的 Chunk 不重复发送 Offer"), Offers.IsEmpty());

	// 同一个 Chunk 对另一条尚未建立基线的连接仍必须提供最新完整快照。
	Storage->GetRelevantChunkOffers(Source, Core, {}, Offers, Relevant);
	TestTrue(TEXT("未 ACK 的连接会 Capture 当前权威记录"),
		Adapter->CaptureBatchCalls > CapturesBeforeOffer);
	for (int32 Attempt = 0; Attempt < 400 && Offers.IsEmpty(); ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
		Storage->GetRelevantChunkOffers(Source, Core, {}, Offers, Relevant);
	}
	TestEqual(TEXT("未 ACK 的连接最终取得一个可验证的 Offer"), Offers.Num(), 1);
	if (Offers.Num() == 1)
	{
		TestTrue(TEXT("Offer 指向请求的负坐标 Chunk，并带当前 Hash"),
			Offers[0].Coord == Coord && Offers[0].ContentHash.IsSet());
	}
	Storage->UnregisterResidencySource(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageResidentDirectoryContractTest,
	"ElementSandbox.WorldStorage.Residency.ResidentDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageResidentDirectoryContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedResidentDirectoryWorld ScopedWorld;
	if (!TestNotNull(TEXT("测试 World 创建成功"), ScopedWorld.Get()))
	{
		return false;
	}
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>();
	if (!TestNotNull(TEXT("WorldStorage Subsystem 已初始化"), Storage))
	{
		return false;
	}

	const FWorldEntityId EntityId(MAX_uint64 - 100);
	FWorldResidentEntityRegistration Registration;
	Registration.EntityId = EntityId;
	Registration.Domain = EWorldEntityDomain::Building;
	Registration.HomeChunk = FWorldChunkCoord(-1, 0, 0);
	Registration.StateRevision = 7;
	TestEqual(TEXT("首次登记为 O(1) Insert"),
		Storage->RegisterResidentEntity(Registration), EWorldResidentUpsertResult::Inserted);
	TestTrue(TEXT("登记后可按统一 ID O(1) 命中"), Storage->IsResident(EntityId));
	FWorldChunkCoord CurrentShowcaseChunk;
	TestTrue(TEXT("存在 Resident 时地图展示查询优先返回运行场景 Chunk"),
		Storage->TryGetMostPopulatedChunk(CurrentShowcaseChunk));
	TestTrue(TEXT("地图展示查询返回已登记的 Resident Chunk"),
		CurrentShowcaseChunk == Registration.HomeChunk);
	TestEqual(TEXT("同 ID、同领域、同 Revision 直接跳过"),
		Storage->RegisterResidentEntity(Registration), EWorldResidentUpsertResult::SameRevision);

	FWorldResidentEntityRegistration Older = Registration;
	Older.StateRevision = 6;
	TestEqual(TEXT("旧 Revision 被拒绝"),
		Storage->RegisterResidentEntity(Older), EWorldResidentUpsertResult::RejectedOlderRevision);
	FWorldResidentEntityRegistration Collision = Registration;
	Collision.Domain = EWorldEntityDomain::WorldObject;
	Collision.StateRevision = 8;
	TestEqual(TEXT("跨领域复用统一 ID 被判定为损坏"),
		Storage->RegisterResidentEntity(Collision), EWorldResidentUpsertResult::RejectedTypeCollision);

	TestTrue(TEXT("跨 Pack 移动以同一 Revision 更新 HomeChunk"),
		Storage->UpdateEntityLocation(EntityId, FVector(100000.0, 50.0, 50.0), 8));
	const FWorldStorageRuntimeStats AfterMove = Storage->GetRuntimeStats();
	TestEqual(TEXT("迁移后 Resident Directory 仍只有一个实体"), AfterMove.ResidentEntityCount, 1);
	TestEqual(TEXT("空旧 Chunk 索引被清除，不会伪造第二个 Resident Chunk"), AfterMove.ResidentChunkCount, 1);
	TestEqual(TEXT("跨 Chunk Mutation 只形成一个 Dirty Entity"), AfterMove.DirtyEntityCount, 1);

	TestTrue(TEXT("GameplayDestroy 产生 Tombstone 并移除 ECS Resident 投影"),
		Storage->GameplayDestroy(EntityId, 9));
	TestFalse(TEXT("删除后不再 Resident"), Storage->IsResident(EntityId));
	TestEqual(TEXT("删除后 Resident Chunk 索引归零"),
		Storage->GetRuntimeStats().ResidentChunkCount, 0);
	Registration.StateRevision = 10;
	TestEqual(TEXT("WorldEntityId 永不复用，较新记录也不能越过 Gameplay Tombstone"),
		Storage->RegisterResidentEntity(Registration), EWorldResidentUpsertResult::RejectedTombstone);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageAsyncCheckpointKeepsLiveManifestMonotonicTest,
	"ElementSandbox.WorldStorage.Checkpoint.LiveManifestNeverRewinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageAsyncCheckpointKeepsLiveManifestMonotonicTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedResidentDirectoryWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("Checkpoint 测试 WorldStorage 已初始化"), Storage))
	{
		return false;
	}
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::Building);
	if (!TestTrue(TEXT("Checkpoint 测试 Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter)))
	{
		return false;
	}

	const FWorldEntityId DirtyEntity = Storage->AllocateEntityId();
	Adapter->Add(DirtyEntity, FVector::ZeroVector);
	FWorldResidentEntityRegistration Registration;
	Registration.EntityId = DirtyEntity;
	Registration.Domain = EWorldEntityDomain::Building;
	Registration.HomeChunk = FWorldChunkCoord(0, 0, 0);
	Registration.StateRevision = 1;
	TestEqual(TEXT("Checkpoint 前登记测试实体"),
		Storage->RegisterResidentEntity(Registration), EWorldResidentUpsertResult::Inserted);
	TestTrue(TEXT("Checkpoint 前标记测试实体"), Storage->MarkEntityDirty(DirtyEntity, 1));

	const FWorldStorageManifestInfo FrozenManifest = Storage->GetManifestInfo();
	TestTrue(TEXT("异步 Checkpoint 成功启动"), Storage->RequestCheckpoint());
	const FWorldEntityId AllocatedWhileInFlight = Storage->AllocateEntityId();
	TestTrue(TEXT("Checkpoint 飞行期间仍可分配身份"), AllocatedWhileInFlight.IsSet());

	// 先让 Authority 时钟在冻结值之后继续前进，再等待并排空异步完成信箱。
	Storage->Tick(0.25f);
	for (int32 Attempt = 0; Attempt < 400 && Storage->GetRuntimeStats().bCheckpointInFlight; ++Attempt)
	{
		FPlatformProcess::Sleep(0.005f);
		Storage->Tick(0.0f);
	}
	TestFalse(TEXT("异步 Checkpoint 在测试预算内完成"), Storage->GetRuntimeStats().bCheckpointInFlight);
	const FWorldStorageManifestInfo PublishedManifest = Storage->GetManifestInfo();
	TestTrue(TEXT("Checkpoint 完成不会把 Authority 时间倒退到冻结值"),
		PublishedManifest.WorldSimulationTimeMilliseconds
			> FrozenManifest.WorldSimulationTimeMilliseconds);
	TestTrue(TEXT("Checkpoint 完成不会把 NextEntityId 倒退到飞行前"),
		PublishedManifest.NextEntityId > AllocatedWhileInFlight.GetValue());
	const FWorldEntityId AllocatedAfterCompletion = Storage->AllocateEntityId();
	TestTrue(TEXT("完成后分配的新身份严格晚于飞行期间身份"),
		AllocatedAfterCompletion.GetValue() > AllocatedWhileInFlight.GetValue());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageMultiSourceResidencyContractTest,
	"ElementSandbox.WorldStorage.Residency.MultiSourceRefCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageMultiSourceResidencyContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedResidentDirectoryWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("多来源测试 WorldStorage 已初始化"), Storage))
	{
		return false;
	}
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::Building);
	if (!TestTrue(TEXT("测试 Building Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter)))
	{
		return false;
	}

	const FWorldEntityId SourceAOnly(10001);
	const FWorldEntityId Overlap(10002);
	const FWorldEntityId SourceBOnly(10003);
	const TArray<TPair<FWorldEntityId, FWorldChunkCoord>> Entities = {
		{ SourceAOnly, FWorldChunkCoord(-40, 0, 0) },
		{ Overlap, FWorldChunkCoord(40, 0, 0) },
		{ SourceBOnly, FWorldChunkCoord(120, 0, 0) }
	};
	for (const TPair<FWorldEntityId, FWorldChunkCoord>& Pair : Entities)
	{
		const FVector Location = Pair.Value.GetWorldMinimum() + FVector(50.0);
		Adapter->Add(Pair.Key, Location);
		FWorldResidentEntityRegistration Registration;
		Registration.EntityId = Pair.Key;
		Registration.Domain = EWorldEntityDomain::Building;
		Registration.HomeChunk = Pair.Value;
		Registration.StateRevision = 1;
		TestEqual(TEXT("测试 Entity 注册到 Resident Directory"),
			Storage->RegisterResidentEntity(Registration), EWorldResidentUpsertResult::Inserted);
	}

	const FWorldResidencySourceHandle SourceA = Storage->RegisterResidencySource(FVector::ZeroVector);
	const FWorldResidencySourceHandle SourceB = Storage->RegisterResidencySource(
		FWorldChunkCoord(80, 0, 0).GetWorldMinimum() + FVector(50.0));
	TestTrue(TEXT("两个玩家来源都有效"), SourceA.IsSet() && SourceB.IsSet());

	TestTrue(TEXT("移除玩家 A 来源"), Storage->UnregisterResidencySource(SourceA));
	Storage->Tick(0.0f);
	TestFalse(TEXT("只属于 A Retention 的 Entity 被 Evict"), Storage->IsResident(SourceAOnly));
	TestTrue(TEXT("重叠 Chunk 因玩家 B RefCount 仍 Resident"), Storage->IsResident(Overlap));
	TestTrue(TEXT("只属于 B 的 Entity 仍 Resident"), Storage->IsResident(SourceBOnly));

	TestTrue(TEXT("移除玩家 B 来源"), Storage->UnregisterResidencySource(SourceB));
	Storage->Tick(0.0f);
	TestFalse(TEXT("最后一个来源离开后重叠 Entity 才 Evict"), Storage->IsResident(Overlap));
	TestFalse(TEXT("B-only Entity 同时 Evict"), Storage->IsResident(SourceBOnly));
	TestEqual(TEXT("三条记录分别只 Evict 一次"), Adapter->EvictedIds.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageAwakePhysicsPinContractTest,
	"ElementSandbox.WorldStorage.Residency.AwakePhysicsPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageAwakePhysicsPinContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedResidentDirectoryWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("Awake Pin 测试 WorldStorage 已初始化"), Storage))
	{
		return false;
	}
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::WorldObject);
	if (!TestTrue(TEXT("测试 WorldObject Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter)))
	{
		return false;
	}

	const FWorldEntityId EntityId(11001);
	const FVector Location(50.0, 50.0, 50.0);
	Adapter->Add(EntityId, Location);
	Adapter->PinnedIds.Add(EntityId);
	FWorldResidentEntityRegistration Registration;
	Registration.EntityId = EntityId;
	Registration.Domain = EWorldEntityDomain::WorldObject;
	Registration.HomeChunk = FWorldChunkCoord::FromWorldLocation(Location);
	Registration.StateRevision = 1;
	TestEqual(TEXT("Awake WorldObject 注册成功"),
		Storage->RegisterResidentEntity(Registration), EWorldResidentUpsertResult::Inserted);

	const FWorldResidencySourceHandle Source = Storage->RegisterResidencySource(Location);
	TestTrue(TEXT("来源注册成功"), Source.IsSet());
	TestTrue(TEXT("来源离开"), Storage->UnregisterResidencySource(Source));
	Storage->Tick(0.0f);
	TestTrue(TEXT("Awake Physics 离开 Retention 仍 Resident"), Storage->IsResident(EntityId));
	TestEqual(TEXT("Awake Pin 进入诊断统计"),
		Storage->GetRuntimeStats().AwakePhysicsPinnedEntityCount, 1);

	Adapter->PinnedIds.Remove(EntityId);
	Storage->Tick(static_cast<float>(UWorldStorageSubsystem::ResidencyPollIntervalSeconds));
	TestFalse(TEXT("Sleep 后下一次维护允许 RuntimeEvict"), Storage->IsResident(EntityId));
	TestEqual(TEXT("Evict 后清除 Awake Pin 诊断"),
		Storage->GetRuntimeStats().AwakePhysicsPinnedEntityCount, 0);
        return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageDelayedMutationBatchContractTest,
	"ElementSandbox.WorldStorage.Checkpoint.DelayedMutationBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageDelayedMutationBatchContractTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldStorage::Tests;
	FScopedResidentDirectoryWorld ScopedWorld;
	UWorldStorageSubsystem* Storage = ScopedWorld.Get()
		? ScopedWorld.Get()->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("Delayed Batch 测试 WorldStorage 已初始化"), Storage)) return false;
	const TSharedRef<FTestWorldStorageAdapter> Adapter =
		MakeShared<FTestWorldStorageAdapter>(EWorldEntityDomain::Building);
	if (!TestTrue(TEXT("Delayed Batch Adapter 注册成功"),
		Storage->ReplaceDomainAdapterForAutomation(Adapter))) return false;

	const FWorldEntityId EntityId = Storage->AllocateEntityId();
	Adapter->Add(EntityId, FVector::ZeroVector);
	const FWorldResidentEntityRegistration Registration{
		EntityId, EWorldEntityDomain::Building, FWorldChunkCoord(0, 0, 0), 1};
        TestEqual(TEXT("事务实体登记成功"), Storage->RegisterResidentEntity(Registration),
                EWorldResidentUpsertResult::Inserted);
		TestTrue(TEXT("事务开始前的普通 Dirty 发布"), Storage->MarkEntityDirty(EntityId, 1));
        const FWorldStorageMutationBatchHandle Batch = Storage->BeginDelayedMutationBatch();
        TestTrue(TEXT("Delayed Mutation Batch 打开"), Batch.IsSet());
		for (int32 Attempt = 0; Attempt < 400 && !Storage->IsDelayedMutationBatchReady(Batch); ++Attempt)
		{
			FPlatformProcess::Sleep(0.002f);
			Storage->Tick(0.0f);
		}
		TestTrue(TEXT("既有 Dirty 异步封口后 Batch Ready"), Storage->IsDelayedMutationBatchReady(Batch));
		TestEqual(TEXT("异步基线只 Capture 一次"), Adapter->CaptureBatchCalls, 1);
	TestTrue(TEXT("Batch 作用域内的 Dirty 发布成功"),
		Storage->ExecuteInDelayedMutationBatch(Batch,
			[Storage, EntityId]() { return Storage->MarkEntityDirty(EntityId, 1); }));
	TestTrue(TEXT("Checkpoint 可在 Burst 飞行期间继续服务其他数据"), Storage->RequestCheckpoint());
	for (int32 Attempt = 0; Attempt < 400 && Storage->GetRuntimeStats().bCheckpointInFlight; ++Attempt)
	{
		FPlatformProcess::Sleep(0.002f);
		Storage->Tick(0.0f);
	}
		TestEqual(TEXT("未 Commit 的 Batch 不追加领域 Capture"), Adapter->CaptureBatchCalls, 1);
	TestTrue(TEXT("Commit Marker 原子释放整批 Mutation"), Storage->CommitDelayedMutationBatch(Batch));
	TestTrue(TEXT("Commit 后 Checkpoint 启动"), Storage->RequestCheckpoint());
	for (int32 Attempt = 0; Attempt < 400 && Storage->GetRuntimeStats().bCheckpointInFlight; ++Attempt)
	{
		FPlatformProcess::Sleep(0.002f);
		Storage->Tick(0.0f);
	}
		TestEqual(TEXT("Commit 后只追加一次领域 Capture"), Adapter->CaptureBatchCalls, 2);
	return true;
}

#endif
