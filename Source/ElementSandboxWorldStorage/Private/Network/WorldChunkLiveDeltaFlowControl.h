#pragma once

#include "CoreMinimal.h"

namespace UE::ElementSandbox::WorldStorage::Private
{
	/**
	 * Live Delta 使用两个连续批次的应用层 ACK 窗口。底层 Reliable 只能保证已提交的 Bunch，
	 * 不能阻止 Gameplay 生产速度把 Actor Channel 的 Reliable Buffer 填满。
	 */
	class FWorldChunkLiveDeltaFlowControl final
	{
	public:
		static constexpr int32 MaximumInFlightBatches = 2;
		bool CanPublish() const { return InFlightSequences.Num() < MaximumInFlightBatches; }
		bool HasInFlightBatch() const { return !InFlightSequences.IsEmpty(); }
		int32 GetInFlightBatchCount() const { return InFlightSequences.Num(); }

		uint64 BeginBatch()
		{
			if (!CanPublish())
			{
				return 0;
			}
			const uint64 Sequence = NextSequence;
			NextSequence = NextSequence == MAX_uint64 ? 1u : NextSequence + 1u;
			InFlightSequences.Add(Sequence);
			return Sequence;
		}

		bool Acknowledge(const uint64 Sequence)
		{
			if (Sequence == 0 || !InFlightSequences.Remove(Sequence))
			{
				return false;
			}
			return true;
		}

	private:
		uint64 NextSequence = 1;
		TSet<uint64> InFlightSequences;
	};
}
