#include "Runtime/ElementRuntimeTypes.h"

bool FElementAuthorityTargetStateSnapshot::IsValid() const
{
	if (!Target.IsValid() || StateRevision == 0 || LastSettlementMilliseconds < 0) return false;
	TSet<FName> NumericChannels;
	for (const FElementNumericValue& Value : NumericValues)
	{
		if (Value.Channel.IsNone() || !FMath::IsFinite(Value.Value) || NumericChannels.Contains(Value.Channel))
		{
			return false;
		}
		NumericChannels.Add(Value.Channel);
	}
	TSet<FName> StateChannels;
	for (const FElementStateValue& Value : StateValues)
	{
		if (!Value.IsValid() || StateChannels.Contains(Value.SchemaId)) return false;
		StateChannels.Add(Value.SchemaId);
	}
	TSet<FName> WakeProcessors;
	for (const FElementPersistentWake& Wake : Wakes)
	{
		if (!Wake.IsValid() || WakeProcessors.Contains(Wake.ProcessorId)) return false;
		WakeProcessors.Add(Wake.ProcessorId);
	}
	return true;
}

namespace
{
	bool HasValidDomains(const EElementTargetDomain Domains)
	{
		constexpr EElementTargetDomain All = EElementTargetDomain::Character
			| EElementTargetDomain::Building | EElementTargetDomain::WorldObject;
		return Domains != EElementTargetDomain::None
			&& !EnumHasAnyFlags(Domains, static_cast<EElementTargetDomain>(~static_cast<uint8>(All)));
	}

	bool HasUniqueValidChannels(const TArray<FName, TInlineAllocator<4>>& Channels)
	{
		TSet<FName> Seen;
		for (const FName Channel : Channels)
		{
			if (Channel.IsNone() || Seen.Contains(Channel)) return false;
			Seen.Add(Channel);
		}
		return true;
	}

	bool IsSubset(
		const TArray<FName, TInlineAllocator<4>>& Candidate,
		const TArray<FName, TInlineAllocator<4>>& Superset)
	{
		for (const FName Channel : Candidate) if (!Superset.Contains(Channel)) return false;
		return true;
	}
}

bool FElementProcessorDescriptor::IsNumericValid() const
{
	return !ProcessorId.IsNone() && !FragmentType.IsNone() && HasValidDomains(TargetDomains)
			&& OwnedStateChannel.IsNone() && !WriteNumericChannels.IsEmpty()
			&& HasUniqueValidChannels(ReadNumericChannels) && HasUniqueValidChannels(WriteNumericChannels)
			&& HasUniqueValidChannels(RecomputedNumericChannels)
			&& IsSubset(RecomputedNumericChannels, WriteNumericChannels);
}

bool FElementProcessorDescriptor::IsStateValid() const
{
	return !ProcessorId.IsNone() && FragmentType.IsNone() && HasValidDomains(TargetDomains)
			&& !OwnedStateChannel.IsNone() && WriteNumericChannels.IsEmpty()
			&& RecomputedNumericChannels.IsEmpty()
		&& HasUniqueValidChannels(ReadNumericChannels);
}
