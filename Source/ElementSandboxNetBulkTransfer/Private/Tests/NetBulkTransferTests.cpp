#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "NetBulkTransferScheduler.h"

namespace UE::ElementSandbox::NetBulk
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNetBulkSegmentationAndAckTest,
	"ElementSandbox.Meteor.NetBulk.SegmentationAndAck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNetBulkSegmentationAndAckTest::RunTest(const FString& Parameters)
{
	FConnectionScheduler Scheduler;
	TArray<uint8> Payload;
	Payload.SetNumUninitialized(DefaultSegmentBytes * 2 + 17);
	for (int32 Index = 0; Index < Payload.Num(); ++Index) Payload[Index] = static_cast<uint8>(Index);
	const FPayloadId Id{1, 42, 1};
	TestTrue(TEXT("Payload 可入队"), Scheduler.Enqueue(ETransferClass::MeteorUrgent, Id, Payload));
	TestFalse(TEXT("同 Revision 重复 Payload 被拒绝"),
		Scheduler.Enqueue(ETransferClass::MeteorUrgent, Id, Payload));

	FSegment First;
	FSegment Second;
	TestTrue(TEXT("第一段出队"), Scheduler.TryDequeue(First));
	TestTrue(TEXT("第二段出队"), Scheduler.TryDequeue(Second));
	TestEqual(TEXT("总段数正确"), First.SegmentCount, static_cast<uint16>(3));
	TestEqual(TEXT("所有段共用 Payload Hash"), First.PayloadHash, Second.PayloadHash);
	FSegment Third;
	TestTrue(TEXT("接收 ACK 不阻挡尾段，连接容量决定发送"), Scheduler.TryDequeue(Third));
	TestEqual(TEXT("三段都登记在途身份"), Scheduler.GetInFlightCount(), 3);
	TestEqual(TEXT("尾段字节数"), Third.Bytes.Num(), 17);
	TestTrue(TEXT("第一段 ACK"), Scheduler.Acknowledge(Id, First.SegmentIndex));
	TestFalse(TEXT("重复 ACK 不回收其他段"), Scheduler.Acknowledge(Id, First.SegmentIndex));
	TestFalse(TEXT("仍在途时不可重复入队"),
		Scheduler.Enqueue(ETransferClass::MeteorUrgent, Id, Payload));
	Scheduler.Cancel(Id);
	TestEqual(TEXT("Cancel 回收剩余在途身份"), Scheduler.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNetBulkControlPriorityTest,
	"ElementSandbox.Meteor.NetBulk.ControlPriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNetBulkControlPriorityTest::RunTest(const FString& Parameters)
{
	FConnectionScheduler Scheduler;
	const TArray<uint8> Payload = {1, 2, 3};
	TestTrue(TEXT("背景页入队"), Scheduler.Enqueue(
		ETransferClass::MeteorBackground, {1, 1, 1}, Payload));
	TestTrue(TEXT("WorldStorage 页入队"), Scheduler.Enqueue(
		ETransferClass::WorldStorage, {2, 1, 1}, Payload));
	TestTrue(TEXT("Gameplay 控制消息入队"), Scheduler.Enqueue(
		ETransferClass::GameplayControl, {3, 1, 1}, Payload));
	FSegment Segment;
	TestTrue(TEXT("控制消息可出队"), Scheduler.TryDequeue(Segment));
	TestEqual(TEXT("控制消息保留带宽优先"), Segment.TransferClass, ETransferClass::GameplayControl);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNetBulkTransportBackpressureTest,
	"ElementSandbox.Meteor.NetBulk.TransportBackpressure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNetBulkTransportBackpressureTest::RunTest(const FString& Parameters)
{
	FConnectionScheduler Scheduler;
	int32 TransportBytesAvailable = 10 * 1024;
	TArray<uint64> DispatchedIds;
	Scheduler.SetCanSendSegment([&TransportBytesAvailable](const int32 Bytes)
	{
		return Bytes <= TransportBytesAvailable;
	});
	TArray<uint8> Payload;
	Payload.Init(17, 2 * 1024);
	for (uint64 Id = 1; Id <= 8; ++Id)
	{
		TestTrue(TEXT("小 Chunk 入队"), Scheduler.Enqueue(ETransferClass::WorldStorage, {1, Id, 1}, Payload,
			DefaultSegmentBytes, [&TransportBytesAvailable, &DispatchedIds](FSegment&& Segment)
			{
				TransportBytesAvailable -= Segment.Bytes.Num();
				DispatchedIds.Add(Segment.PayloadId.Transfer);
			}));
	}
	while (Scheduler.TryDispatch()) {}
	TestEqual(TEXT("连接余量内一次发送五份小 Chunk，而不是只发两份"), DispatchedIds.Num(), 5);
	TestEqual(TEXT("拥塞时后三份仍在原队列"), Scheduler.GetQueuedSegmentCount(), 3);
	TestTrue(TEXT("接收 ACK 可回收身份"), Scheduler.Acknowledge({1, 1, 1}, 0));
	TestFalse(TEXT("接收 ACK 不伪造 UE 传输余量"), Scheduler.TryDispatch());
	Scheduler.Cancel({1, 7, 1});
	TransportBytesAvailable = 4 * 1024;
	while (Scheduler.TryDispatch()) {}
	if (!TestEqual(TEXT("拥塞解除后继续原队列且不重发"), DispatchedIds.Num(), 7)) return false;
	TestEqual(TEXT("被阻挡任务没有丢失游标"), DispatchedIds[5], uint64(6));
	TestEqual(TEXT("取消的任务不会被发送"), DispatchedIds[6], uint64(8));
	TestEqual(TEXT("已全部发送"), Scheduler.GetQueuedSegmentCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNetBulkWeightedFairnessTest,
	"ElementSandbox.Meteor.NetBulk.WeightedFairness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNetBulkWeightedFairnessTest::RunTest(const FString& Parameters)
{
	FConnectionScheduler Scheduler;
	FSegment Segment;
	const TArray<uint8> OneByte = {7};
	for (uint64 Index = 0; Index < 8; ++Index)
	{
		TestTrue(TEXT("八份临期页入队"), Scheduler.Enqueue(
			ETransferClass::MeteorUrgent, {20, 100 + Index, 1}, OneByte));
	}
	for (uint64 Index = 0; Index < 4; ++Index)
	{
		TestTrue(TEXT("四份 WorldStorage 页入队"), Scheduler.Enqueue(
			ETransferClass::WorldStorage, {21, 200 + Index, 1}, OneByte));
	}
	TestTrue(TEXT("一份远期页入队"), Scheduler.Enqueue(
		ETransferClass::MeteorBackground, {22, 300, 1}, OneByte));
	TArray<ETransferClass> Order;
	for (int32 Index = 0; Index < 13; ++Index)
	{
		TestTrue(TEXT("权重环持续出队"), Scheduler.TryDequeue(Segment));
		Order.Add(Segment.TransferClass);
		Scheduler.Acknowledge(Segment.PayloadId, Segment.SegmentIndex);
	}
	TestEqual(TEXT("权重环前八份保留临期 Meteor"),
		Order.FilterByPredicate([](ETransferClass Value)
		{
			return Value == ETransferClass::MeteorUrgent;
		}).Num(), 8);
	TestEqual(TEXT("WorldStorage 获得四份公平配额"),
		Order.FilterByPredicate([](ETransferClass Value)
		{
			return Value == ETransferClass::WorldStorage;
		}).Num(), 4);
	TestEqual(TEXT("远期 Meteor 不被长期饿死"), Order.Last(), ETransferClass::MeteorBackground);
	return true;
}
}

#endif
