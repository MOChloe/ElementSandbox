#include "Storage/ElementWorldStorageAdapter.h"

#include "Runtime/ElementFireDomain.h"
#include "Runtime/ElementFireRuntimeTypes.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Storage/ElementFirePersistenceTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "BuildingWorldSubsystem.h"
#include "Definition/BuildingDefinition.h"
#include "ElementGameplayWorldSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "WorldStorageSubsystem.h"
#endif

namespace
{
	constexpr uint32 ElementPayloadMagic = 0x314d4c45; // ELM1
	constexpr uint16 ElementPayloadVersion = 1;
	constexpr uint16 MaximumNumericChannels = 32;
	constexpr uint16 MaximumStateChannels = 8;
	constexpr uint16 MaximumWakes = 8;
	const FName ElementFireDefinitionId(TEXT("Element.Fire.HostThermal"));

	bool WriteName(FArchive& Archive, const FName Name)
	{
		FString Value = Name.ToString();
		Archive << Value;
		return !Archive.IsError();
	}

	bool ReadName(FArchive& Archive, FName& OutName)
	{
		FString Value;
		Archive << Value;
		OutName = FName(*Value);
		return !Archive.IsError() && !OutName.IsNone();
	}

	bool EncodeElementPayload(
		const FElementFirePersistentRecord& Source,
		TArray<uint8>& OutPayload,
		FString& OutError)
	{
		OutPayload.Reset();
		if (!Source.IsValid()
			|| Source.AuthorityState.NumericValues.Num() > MaximumNumericChannels
			|| Source.AuthorityState.StateValues.Num() > MaximumStateChannels
			|| Source.AuthorityState.Wakes.Num() > MaximumWakes)
		{
			OutError = TEXT("Element 持久化纯值非法或超过有界 Channel 数量。");
			return false;
		}
		FMemoryWriter Writer(OutPayload, true);
		uint32 Magic = ElementPayloadMagic;
		uint16 Version = ElementPayloadVersion;
		uint8 HostDomain = static_cast<uint8>(Source.HostDomain);
		uint8 Flags = Source.Source.IsSet() ? 0x01 : 0x00;
		uint64 HostId = Source.HostId.GetValue();
		Writer << Magic << Version << HostDomain << Flags << HostId;
		uint64 AuthorityRevision = Source.AuthorityState.StateRevision;
		int64 SettlementTime = Source.AuthorityState.LastSettlementMilliseconds;
		Writer << AuthorityRevision << SettlementTime;

		uint16 NumericCount = static_cast<uint16>(Source.AuthorityState.NumericValues.Num());
		Writer << NumericCount;
		for (const FElementNumericValue& Value : Source.AuthorityState.NumericValues)
		{
			if (!WriteName(Writer, Value.Channel)) break;
			double Number = Value.Value;
			Writer << Number;
		}

		uint16 StateCount = static_cast<uint16>(Source.AuthorityState.StateValues.Num());
		Writer << StateCount;
		for (const FElementStateValue& State : Source.AuthorityState.StateValues)
		{
			if (!WriteName(Writer, State.SchemaId)) break;
			uint64 Revision = State.Revision;
			uint8 ValueCount = State.Payload.Count;
			Writer << Revision << ValueCount;
			for (uint8 Index = 0; Index < ValueCount; ++Index)
			{
				double Number = State.Payload.Values[Index];
				Writer << Number;
			}
		}

		uint16 WakeCount = static_cast<uint16>(Source.AuthorityState.Wakes.Num());
		Writer << WakeCount;
		for (const FElementPersistentWake& Wake : Source.AuthorityState.Wakes)
		{
			if (!WriteName(Writer, Wake.ProcessorId)) break;
			int64 DueTime = Wake.DueTimeMilliseconds;
			Writer << DueTime;
		}

		if (Source.Source.IsSet())
		{
			double Intensity = Source.Source->Intensity;
			double Range = Source.Source->RangeCentimeters;
			uint8 Policy = Source.Source->Policy;
			int64 ExpireTime = Source.Source->ExpireTimeMilliseconds;
			Writer << Intensity << Range << Policy << ExpireTime;
		}
		if (Writer.IsError())
		{
			OutPayload.Reset();
			OutError = TEXT("Element Payload 编码失败。");
			return false;
		}
		return true;
	}

	bool DecodeElementPayload(
		const TArray<uint8>& Payload,
		FElementFirePersistentRecord& OutRecord,
		FString& OutError)
	{
		OutRecord = {};
		if (Payload.IsEmpty())
		{
			OutError = TEXT("Element Payload 不能为空。");
			return false;
		}
		FMemoryReader Reader(const_cast<TArray<uint8>&>(Payload), true);
		uint32 Magic = 0;
		uint16 Version = 0;
		uint8 HostDomain = 0;
		uint8 Flags = 0;
		uint64 HostId = 0;
		Reader << Magic << Version << HostDomain << Flags << HostId;
		if (Magic != ElementPayloadMagic || Version != ElementPayloadVersion
			|| (HostDomain != static_cast<uint8>(EElementFirePersistentHostDomain::Building)
				&& HostDomain != static_cast<uint8>(EElementFirePersistentHostDomain::WorldObject))
			|| (Flags & ~uint8(0x01)) != 0 || HostId == 0)
		{
			OutError = TEXT("Element Payload 格式或版本不匹配；旧 Element/Fire Payload 不受支持。");
			return false;
		}
		OutRecord.HostDomain = static_cast<EElementFirePersistentHostDomain>(HostDomain);
		OutRecord.HostId = FWorldEntityId(HostId);
		Reader << OutRecord.AuthorityState.StateRevision
			<< OutRecord.AuthorityState.LastSettlementMilliseconds;
		if (OutRecord.AuthorityState.StateRevision == 0
			|| OutRecord.AuthorityState.LastSettlementMilliseconds < 0)
		{
			OutError = TEXT("Element Payload 的状态 Revision 或结算时间非法。");
			return false;
		}

		uint16 NumericCount = 0;
		Reader << NumericCount;
		if (NumericCount > MaximumNumericChannels)
		{
			OutError = TEXT("Element Payload 的 Numeric Channel 数量越界。");
			return false;
		}
		TSet<FName> NumericNames;
		for (uint16 Index = 0; Index < NumericCount; ++Index)
		{
			FElementNumericValue& Value = OutRecord.AuthorityState.NumericValues.AddDefaulted_GetRef();
			if (!ReadName(Reader, Value.Channel))
			{
				OutError = TEXT("Element Payload 的 Numeric Channel 名称非法。");
				return false;
			}
			Reader << Value.Value;
			if (!FMath::IsFinite(Value.Value) || NumericNames.Contains(Value.Channel))
			{
				OutError = TEXT("Element Payload 含非法或重复 Numeric Channel。");
				return false;
			}
			NumericNames.Add(Value.Channel);
		}

		uint16 StateCount = 0;
		Reader << StateCount;
		if (StateCount > MaximumStateChannels)
		{
			OutError = TEXT("Element Payload 的 State Channel 数量越界。");
			return false;
		}
		TSet<FName> StateNames;
		for (uint16 Index = 0; Index < StateCount; ++Index)
		{
			FElementStateValue& State = OutRecord.AuthorityState.StateValues.AddDefaulted_GetRef();
			uint8 ValueCount = 0;
			if (!ReadName(Reader, State.SchemaId))
			{
				OutError = TEXT("Element Payload 的 State Channel 名称非法。");
				return false;
			}
			Reader << State.Revision << ValueCount;
			if (State.Revision == 0 || ValueCount > FElementValuePayload::MaximumValues
				|| StateNames.Contains(State.SchemaId))
			{
				OutError = TEXT("Element Payload 含非法或重复 State Channel。");
				return false;
			}
			StateNames.Add(State.SchemaId);
			State.Payload.Count = ValueCount;
			for (uint8 ValueIndex = 0; ValueIndex < ValueCount; ++ValueIndex)
			{
				Reader << State.Payload.Values[ValueIndex];
				if (!FMath::IsFinite(State.Payload.Values[ValueIndex]))
				{
					OutError = TEXT("Element Payload 的 State 数值非法。");
					return false;
				}
			}
		}

		uint16 WakeCount = 0;
		Reader << WakeCount;
		if (WakeCount > MaximumWakes)
		{
			OutError = TEXT("Element Payload 的 Wake 数量越界。");
			return false;
		}
		TSet<FName> WakeNames;
		for (uint16 Index = 0; Index < WakeCount; ++Index)
		{
			FElementPersistentWake& Wake = OutRecord.AuthorityState.Wakes.AddDefaulted_GetRef();
			if (!ReadName(Reader, Wake.ProcessorId))
			{
				OutError = TEXT("Element Payload 的 Wake Processor 名称非法。");
				return false;
			}
			Reader << Wake.DueTimeMilliseconds;
			if (!Wake.IsValid() || WakeNames.Contains(Wake.ProcessorId))
			{
				OutError = TEXT("Element Payload 含非法或重复 Wake。");
				return false;
			}
			WakeNames.Add(Wake.ProcessorId);
		}

		if ((Flags & 0x01) != 0)
		{
			FElementFirePersistentSource& Source = OutRecord.Source.Emplace();
			Reader << Source.Intensity << Source.RangeCentimeters << Source.Policy << Source.ExpireTimeMilliseconds;
			if (!Source.IsValid())
			{
				OutError = TEXT("Element Payload 的 Fire Source 纯值非法。");
				return false;
			}
		}
		if (Reader.IsError() || Reader.Tell() != Reader.TotalSize())
		{
			OutError = TEXT("Element Payload 截断或存在尾随字节。");
			return false;
		}
		return true;
	}

	class FElementWorldStorageAdapter final : public IWorldStorageDomainAdapter
	{
	public:
		explicit FElementWorldStorageAdapter(FElementFireDomain& InRuntime) : Runtime(&InRuntime) {}

		virtual EWorldEntityDomain GetDomain() const override { return EWorldEntityDomain::Element; }
		virtual EWorldStorageRestorePhase GetRestorePhase() const override
		{
			return EWorldStorageRestorePhase::Dependent;
		}

		virtual bool CaptureBatch(
			const TConstArrayView<FWorldEntityId> EntityIds,
			TArray<FWorldPersistentEntityRecord>& OutRecords,
			FString& OutError) const override
		{
			if (!Runtime)
			{
				OutError = TEXT("Element Runtime 不可用。");
				return false;
			}
			TArray<FElementFirePersistentRecord> States;
			if (!Runtime->CapturePersistentState(EntityIds, States, OutError) || States.Num() != EntityIds.Num())
			{
				return false;
			}
			OutRecords.Reserve(OutRecords.Num() + States.Num());
			for (const FElementFirePersistentRecord& State : States)
			{
				FWorldPersistentEntityRecord& Record = OutRecords.AddDefaulted_GetRef();
				Record.EntityId = State.ElementId;
				Record.Domain = EWorldEntityDomain::Element;
				Record.DefinitionId = ElementFireDefinitionId;
				Record.WorldTransform = FTransform(State.WorldLocation);
				Record.StateRevision = State.StateRevision;
				if (!EncodeElementPayload(State, Record.Payload, OutError)) return false;
			}
			return true;
		}

		virtual bool RestoreBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldPersistentEntityRecord> Records,
			FString& OutError) override
		{
			if (!Runtime)
			{
				OutError = TEXT("Element Runtime 不可用。");
				return false;
			}
			TArray<FElementFirePersistentRecord> Decoded;
			Decoded.Reserve(Records.Num());
			TSet<FWorldEntityId> Seen;
			for (const FWorldPersistentEntityRecord& Record : Records)
			{
				if (!Record.IsValid() || Record.Domain != EWorldEntityDomain::Element
					|| Record.DefinitionId != ElementFireDefinitionId || Seen.Contains(Record.EntityId)
					|| FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != HomeChunk)
				{
					OutError = TEXT("Element Restore 全批预检发现非法、重复记录或错误 HomeChunk。");
					return false;
				}
				Seen.Add(Record.EntityId);
				FElementFirePersistentRecord& State = Decoded.AddDefaulted_GetRef();
				if (!DecodeElementPayload(Record.Payload, State, OutError)) return false;
				State.ElementId = Record.EntityId;
				State.WorldLocation = Record.WorldTransform.GetLocation();
				State.StateRevision = Record.StateRevision;
			}
			return Runtime->RestorePersistentState(HomeChunk, Decoded, OutError);
		}

		virtual bool RuntimeEvictBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return Runtime && Runtime->RemovePersistentState(
				HomeChunk, EntityIds, EElementPersistentRemovalSemantic::RuntimeEvict, OutError);
		}

		virtual bool GameplayDestroyBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return Runtime && Runtime->RemovePersistentState(
				HomeChunk, EntityIds, EElementPersistentRemovalSemantic::GameplayDestroy, OutError);
		}

		virtual bool LeaveInterestBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return Runtime && Runtime->RemovePersistentState(
				HomeChunk, EntityIds, EElementPersistentRemovalSemantic::LeaveInterest, OutError);
		}

		virtual bool RollbackRestoreBatch(
			const FWorldChunkCoord& HomeChunk,
			const TConstArrayView<FWorldEntityId> EntityIds,
			FString& OutError) override
		{
			return Runtime && Runtime->RemovePersistentState(
				HomeChunk, EntityIds, EElementPersistentRemovalSemantic::FailedRestoreRollback, OutError);
		}

		virtual bool CanRuntimeEvict(const FWorldEntityId EntityId) const override
		{
			return Runtime && Runtime->CanRuntimeEvictPersistentState(EntityId);
		}

	private:
		FElementFireDomain* Runtime = nullptr;
	};
}

TSharedRef<IWorldStorageDomainAdapter> MakeElementWorldStorageAdapter(FElementFireDomain& Runtime)
{
        return MakeShared<FElementWorldStorageAdapter>(Runtime);
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace ElementSandbox::ElementPersistence::Tests
{
	struct FClientTombstoneTestWorld final
	{
		explicit FClientTombstoneTestWorld(const FName WorldName, const ENetMode NetMode)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true)
				.ShouldSimulatePhysics(false)
				.EnableTraceCollision(true)
				.CreateNavigation(false)
				.CreateAISystem(false);
			World = UWorld::CreateWorld(
				EWorldType::PIE, false, WorldName, nullptr, true, ERHIFeatureLevel::Num, &Values, true);
			if (!World) return;
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NetMode);
			World->InitWorld(Values);
			World->UpdateWorldComponents(true, false);
			Storage = World->GetSubsystem<UWorldStorageSubsystem>();
			Buildings = World->GetSubsystem<UBuildingWorldSubsystem>();
			ElementGameplay = World->GetSubsystem<UElementGameplayWorldSubsystem>();
		}

		~FClientTombstoneTestWorld()
		{
			if (!World) return;
			if (Storage && ResidencySource.IsSet()) Storage->UnregisterResidencySource(ResidencySource);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}

		bool IsValid() const
		{
			return World && Storage && Buildings && ElementGameplay
				&& ElementGameplay->IsRuntimeAssemblyActive();
		}

		bool AddResidencySource()
		{
			ResidencySource = Storage ? Storage->RegisterResidencySource(FVector::ZeroVector)
				: FWorldResidencySourceHandle();
			return ResidencySource.IsSet();
		}

		UWorld* World = nullptr;
		UWorldStorageSubsystem* Storage = nullptr;
		UBuildingWorldSubsystem* Buildings = nullptr;
		UElementGameplayWorldSubsystem* ElementGameplay = nullptr;
		FWorldResidencySourceHandle ResidencySource;
	};

	FElementFirePersistentRecord MakeRecord()
	{
		FElementFirePersistentRecord Record;
		Record.ElementId = FWorldEntityId(9001);
		Record.HostId = FWorldEntityId(7001);
		Record.HostDomain = EElementFirePersistentHostDomain::Building;
		Record.WorldLocation = FVector(-15050.0, 225.0, -11000.0);
		Record.StateRevision = 7;
		Record.AuthorityState.Target.Domain = EElementTargetDomain::Building;
		Record.AuthorityState.Target.WorldEntityId = Record.HostId;
		Record.AuthorityState.Target.RegistryId = 11;
		Record.AuthorityState.Target.Slot = 3;
		Record.AuthorityState.Target.Generation = 4;
		Record.AuthorityState.StateRevision = 7;
		Record.AuthorityState.LastSettlementMilliseconds = 123456;
		Record.AuthorityState.NumericValues.Add({TEXT("Element.Thermal"), 42.5});
		FElementStateValue& State = Record.AuthorityState.StateValues.AddDefaulted_GetRef();
		State.SchemaId = TEXT("Element.Burning");
		State.Revision = 5;
		State.Payload.Count = 1;
		State.Payload.Values[0] = 1.0;
		Record.AuthorityState.Wakes.Add({TEXT("Element.Thermal.State"), 130000});
		FElementFirePersistentSource& Source = Record.Source.Emplace();
		Source.Intensity = 2.5;
		Source.RangeCentimeters = 175.0;
		Source.Policy = 1;
		Source.ExpireTimeMilliseconds = 140000;
		return Record;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPayloadStrictCodecTest,
	"ElementSandbox.ElementRuntime.Persistence.StrictPayloadRoundTripAndVersionRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPayloadStrictCodecTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPersistence::Tests;
	const FElementFirePersistentRecord Source = MakeRecord();
	TArray<uint8> Payload;
	FString Error;
	TestTrue(TEXT("当前唯一 Element Payload 可编码"), EncodeElementPayload(Source, Payload, Error));
	FElementFirePersistentRecord Decoded;
	TestTrue(TEXT("当前 Payload 可严格解码"), DecodeElementPayload(Payload, Decoded, Error));
	TestTrue(TEXT("Host Key 往返"), Decoded.HostId == Source.HostId);
	TestEqual(TEXT("Numeric Channel 往返"), Decoded.AuthorityState.NumericValues[0].Value, 42.5);
	TestEqual(TEXT("绝对唤醒时间往返"),
		Decoded.AuthorityState.Wakes[0].DueTimeMilliseconds, 130000ll);
	TestTrue(TEXT("Fire Source 纯值往返"), Decoded.Source.IsSet()
		&& FMath::IsNearlyEqual(Decoded.Source->RangeCentimeters, 175.0));

	TArray<uint8> WrongVersion = Payload;
	check(WrongVersion.Num() >= 6);
	WrongVersion[4] = 2;
	WrongVersion[5] = 0;
	TestFalse(TEXT("旧或未知 Element Payload 版本直接拒绝"),
		DecodeElementPayload(WrongVersion, Decoded, Error));
	TArray<uint8> Trailing = Payload;
	Trailing.Add(0x7f);
	TestFalse(TEXT("尾随旧字段不会被兼容读取"), DecodeElementPayload(Trailing, Decoded, Error));
	TArray<uint8> Truncated = Payload;
	Truncated.SetNum(Truncated.Num() / 2);
	TestFalse(TEXT("截断 Payload 被原子预检拒绝"), DecodeElementPayload(Truncated, Decoded, Error));
	TestTrue(TEXT("负坐标 Element 位于数学向下取整的 Chunk"),
		FWorldChunkCoord::FromWorldLocation(Source.WorldLocation) == FWorldChunkCoord(-2, 0, -2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementClientHostTombstoneOrderingTest,
	"ElementSandbox.ElementRuntime.Persistence.ClientHostTombstoneOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementClientHostTombstoneOrderingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPersistence::Tests;
	FClientTombstoneTestWorld Authority(TEXT("ElementClientTombstoneAuthority"), NM_DedicatedServer);
	FClientTombstoneTestWorld Client(TEXT("ElementClientTombstoneProjection"), NM_Client);
	if (!TestTrue(TEXT("Authority 与 Client Element 装配可用"), Authority.IsValid() && Client.IsValid())
		|| !TestTrue(TEXT("Authority 建立 Host Residency"), Authority.AddResidencySource()))
	{
		return false;
	}
	Authority.World->BeginPlay();
	Client.World->BeginPlay();

	UBuildingDefinition* Definition = Authority.Buildings->FindDefinition(TEXT("WoodPillar"));
	TestNotNull(TEXT("Authority 找到 WoodPillar Definition"), Definition);
	const FBuildEntityHandle Host = Definition
		? Authority.Buildings->CreateEntity(*Definition, FTransform::Identity)
		: FBuildEntityHandle();
	TestTrue(TEXT("Authority 创建 Primary Host Handle"), Host.IsSet());
	const FWorldEntityId HostId = Authority.Buildings->GetWorldEntityId(Host);
	if (!TestTrue(TEXT("Authority Primary Host 获得 WorldEntityId"), HostId.IsSet()))
	{
		return false;
	}
	FWorldPersistentEntityRecord HostRecord;
	FString Error;
	if (!TestTrue(TEXT("Authority 捕获 Primary Host 网络记录"),
		Authority.Storage->CaptureResidentRecord(HostId, HostRecord, Error)))
	{
		AddError(Error);
		return false;
	}

	FElementFirePersistentRecord ElementState;
	ElementState.ElementId = Authority.Storage->AllocateEntityId();
	ElementState.HostId = HostId;
	ElementState.HostDomain = EElementFirePersistentHostDomain::Building;
	ElementState.WorldLocation = FVector::ZeroVector;
	ElementState.StateRevision = 1;
	ElementState.AuthorityState.Target.Domain = EElementTargetDomain::Building;
	ElementState.AuthorityState.Target.WorldEntityId = HostId;
	ElementState.AuthorityState.Target.RegistryId = 1;
	ElementState.AuthorityState.Target.Slot = 0;
	ElementState.AuthorityState.Target.Generation = 1;
	ElementState.AuthorityState.StateRevision = 1;
	ElementState.AuthorityState.LastSettlementMilliseconds = 0;
	FElementStateValue& Thermal = ElementState.AuthorityState.StateValues.AddDefaulted_GetRef();
	Thermal.SchemaId = ElementFireRuntimeNames::ThermalState;
	Thermal.Revision = 1;
	Thermal.Payload.Count = 8;
	Thermal.Payload.Values[0] = static_cast<double>(EFireCombustionPhase::Cold);
	Thermal.Payload.Values[4] = 0.0;

	FWorldPersistentEntityRecord ElementRecord;
	ElementRecord.EntityId = ElementState.ElementId;
	ElementRecord.Domain = EWorldEntityDomain::Element;
	ElementRecord.DefinitionId = ElementFireDefinitionId;
	ElementRecord.WorldTransform = FTransform::Identity;
	ElementRecord.StateRevision = ElementState.StateRevision;
	if (!TestTrue(TEXT("构造合法 Dependent Element 网络记录"),
		EncodeElementPayload(ElementState, ElementRecord.Payload, Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("Client 初始没有 Cold Building Fire Host"),
		Client.ElementGameplay->GetBuildingFireHostCountForTesting(), 0);
	TestTrue(TEXT("Client 先恢复 Primary Host"), Client.Storage->ApplyNetworkUpsert(HostRecord));
	TestEqual(TEXT("只恢复 Cold Primary Host 不建立 Fire Host"),
		Client.ElementGameplay->GetBuildingFireHostCountForTesting(), 0);
	TestTrue(TEXT("Client 再恢复 Dependent Element Binding"), Client.Storage->ApplyNetworkUpsert(ElementRecord));
	TestEqual(TEXT("Dependent Element 只精确物化自己的 Building Host"),
		Client.ElementGameplay->GetBuildingFireHostCountForTesting(), 1);
	TestTrue(TEXT("Client 接收 Host Tombstone 时不反向提交 Authority 销毁"),
		Client.Storage->ApplyNetworkRemove(HostId, HostRecord.StateRevision + 1, true));
	TestFalse(TEXT("Host Tombstone 已移除 Client Primary 投影"),
		Client.Buildings->FindEntity(HostId).IsSet());
	TestTrue(TEXT("随后到达的 Element Tombstone 清理暂留 Dependent Binding"),
		Client.Storage->ApplyNetworkRemove(ElementRecord.EntityId, ElementRecord.StateRevision + 1, true));
	TestEqual(TEXT("Post-Actor 提交前仍暂留按需物化的 Building Host"),
		Client.ElementGameplay->GetBuildingFireHostCountForTesting(), 1);
	Client.World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	TestEqual(TEXT("下一次预算 Pump 清理按需物化的 Building Host"),
		Client.ElementGameplay->GetBuildingFireHostCountForTesting(), 0);
	return true;
}

#endif
