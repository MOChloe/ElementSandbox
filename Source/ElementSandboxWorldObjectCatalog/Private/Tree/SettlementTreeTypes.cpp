#include "Tree/SettlementTreeTypes.h"

float ComputeSettlementTreeColorVariation(const FWorldEntityId WorldEntityId)
{
	uint64 Value = WorldEntityId.GetValue() + 0x9e3779b97f4a7c15ull;
	Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ull;
	Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebull;
	Value ^= Value >> 31;
	return static_cast<float>(Value & 0xffffu) / 65535.0f;
}
