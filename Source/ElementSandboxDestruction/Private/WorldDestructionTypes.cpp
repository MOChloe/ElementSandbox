#include "WorldDestructionTypes.h"

#include "Definition/WorldDestructionDefinition.h"

namespace UE::ElementSandbox::Destruction
{
bool FWorldDestructionProductBatch::IsValid() const
{
	return Target.IsSet() && SourceId.IsSet() && SourceId == Target.WorldEntityId
		&& DestructionRevision != 0 && SourceBounds.IsValid != 0
		&& !SourceBounds.ContainsNaN() && Definition
		&& Definition->IsEnabled() && Definition->IsValid()
		&& (!LaunchContext || LaunchContext->IsValid());
}
}
