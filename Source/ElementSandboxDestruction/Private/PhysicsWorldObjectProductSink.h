#pragma once

#include "WorldDestructionTypes.h"
#include "WorldObjectCreateDesc.h"

class UWorldObjectWorldSubsystem;

namespace UE::ElementSandbox::Destruction
{
	/** 斧头/燃尽默认出口：准备普通 Physics WorldObject，源提交后无失败发布。 */
	class FPhysicsWorldObjectProductSink final : public IWorldDestructionProductSink
	{
	public:
		explicit FPhysicsWorldObjectProductSink(UWorldObjectWorldSubsystem& InWorldObjects)
			: WorldObjects(InWorldObjects)
		{
		}

		virtual bool Prepare(const FWorldDestructionProductBatch& Batch) override;
		virtual void Commit() override;
		virtual void Rollback() override;

	private:
		UWorldObjectWorldSubsystem& WorldObjects;
		FWorldObjectStagedCreateBatch StagedProducts;
		bool bPrepared = false;
	};
}
