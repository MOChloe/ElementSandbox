#pragma once

#include "Network/WorldChunkLiveDeltaFlowControl.h"
#include "Network/WorldChunkStreamingComponent.h"

namespace UE::ElementSandbox::WorldStorage::Private
{
	enum class EWorldChunkLiveDeltaEnqueueResult : uint8
	{
		Queued,
		AlreadyApplied,
		AlreadyQueued,
		OutOfOrder,
		Full
	};

	struct FQueuedWorldChunkLiveDeltaBatch final
	{
		uint64 Sequence = 0;
		TArray<FWorldChunkLiveDelta> Deltas;
	};

	/**
	 * Reliable RPC 只负责把至多两个在途批次放入有序队列；真正的 ECS 提交由
	 * Component Tick 每帧只取一批。这样保留网络双窗口吞吐，又不会在一次 Net Tick
	 * 内连续执行两次 WorldStorage/领域生命周期事务。
	 */
	class FWorldChunkClientLiveDeltaQueue final
	{
	public:
		static constexpr int32 MaximumQueuedBatches =
			FWorldChunkLiveDeltaFlowControl::MaximumInFlightBatches;

		EWorldChunkLiveDeltaEnqueueResult Enqueue(
			const uint64 Sequence,
			TArray<FWorldChunkLiveDelta>&& Deltas)
		{
			if (Sequence == 0 || Deltas.IsEmpty())
			{
				return EWorldChunkLiveDeltaEnqueueResult::OutOfOrder;
			}
			if (Sequence == LastAppliedSequence
				|| (LastAppliedSequence != MAX_uint64 && Sequence < LastAppliedSequence))
			{
				return EWorldChunkLiveDeltaEnqueueResult::AlreadyApplied;
			}
			if (Batches.ContainsByPredicate(
				[Sequence](const FQueuedWorldChunkLiveDeltaBatch& Batch)
				{
					return Batch.Sequence == Sequence;
				}))
			{
				return EWorldChunkLiveDeltaEnqueueResult::AlreadyQueued;
			}
			const uint64 PreviousSequence = Batches.IsEmpty()
				? LastAppliedSequence
				: Batches.Last().Sequence;
			const uint64 ExpectedSequence = PreviousSequence == MAX_uint64 ? 1 : PreviousSequence + 1;
			if (Sequence != ExpectedSequence)
			{
				return EWorldChunkLiveDeltaEnqueueResult::OutOfOrder;
			}
			if (Batches.Num() >= MaximumQueuedBatches)
			{
				return EWorldChunkLiveDeltaEnqueueResult::Full;
			}
			FQueuedWorldChunkLiveDeltaBatch& Batch = Batches.AddDefaulted_GetRef();
			Batch.Sequence = Sequence;
			Batch.Deltas = MoveTemp(Deltas);
			return EWorldChunkLiveDeltaEnqueueResult::Queued;
		}

		const FQueuedWorldChunkLiveDeltaBatch* Peek() const
		{
			return Batches.IsEmpty() ? nullptr : &Batches[0];
		}

		bool CompleteFront(const uint64 Sequence)
		{
			if (Batches.IsEmpty() || Batches[0].Sequence != Sequence)
			{
				return false;
			}
			LastAppliedSequence = Sequence;
			Batches.RemoveAt(0, 1, EAllowShrinking::No);
			return true;
		}

		void DiscardAllAsConsumed(TArray<uint64>& OutSequences)
		{
			OutSequences.Reset(Batches.Num());
			for (const FQueuedWorldChunkLiveDeltaBatch& Batch : Batches)
			{
				OutSequences.Add(Batch.Sequence);
				LastAppliedSequence = Batch.Sequence;
			}
			Batches.Reset();
		}

		uint64 GetLastAppliedSequence() const { return LastAppliedSequence; }
		int32 Num() const { return Batches.Num(); }

	private:
		TArray<FQueuedWorldChunkLiveDeltaBatch, TInlineAllocator<MaximumQueuedBatches>> Batches;
		uint64 LastAppliedSequence = 0;
	};
}
