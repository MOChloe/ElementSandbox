#pragma once

#include "CoreMinimal.h"

namespace UE::ElementSandbox::WorldStorage::Private
{
	/** 单个 Chunk Offer 从发布到 Snapshot ACK 期间占用应用层发送窗口。 */
	class FWorldChunkOfferFlowControl final
	{
	public:
		void Reset()
		{
			bPublished = false;
			bClientResponseReceived = false;
		}

		void MarkPublished()
		{
			bPublished = true;
			bClientResponseReceived = false;
		}

		void MarkClientResponseReceived()
		{
			bClientResponseReceived = true;
		}

		void ReleasePublishedWindow()
		{
			bPublished = false;
			bClientResponseReceived = true;
		}

		bool OccupiesPublishedWindow() const { return bPublished; }

		bool ShouldRetryOffer(
			const bool bSnapshotPipelineActive,
			const bool bAlreadyQueued,
			const double NowSeconds,
			const double LastOfferSentSeconds,
			const double ResponseTimeoutSeconds) const
		{
			return bPublished
				&& !bClientResponseReceived
				&& !bSnapshotPipelineActive
				&& !bAlreadyQueued
				&& NowSeconds - LastOfferSentSeconds >= ResponseTimeoutSeconds;
		}

		static bool CanPublish(
			const bool bAlreadyPublished,
			const int32 PublishedOfferCount,
			const int32 MaximumPublishedOffers)
		{
			return bAlreadyPublished
				|| (MaximumPublishedOffers > 0
					&& PublishedOfferCount < MaximumPublishedOffers);
		}

	private:
		bool bPublished = false;
		bool bClientResponseReceived = false;
	};
}
