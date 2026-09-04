#include "Chunk/WorldChunkTypes.h"

bool FWorldPersistentEntityRecord::IsValid() const
{
	return EntityId.IsSet() && Domain != EWorldEntityDomain::Invalid && Domain != EWorldEntityDomain::Character
		&& !DefinitionId.IsNone() && StateRevision != 0 && !WorldTransform.ContainsNaN();
}

bool FWorldChunkData::IsValid() const
{
	if (Revision == 0)
	{
		return false;
	}
	FWorldEntityId Previous;
	TSet<FWorldEntityId> Seen;
	Seen.Reserve(Records.Num());
	for (const FWorldPersistentEntityRecord& Record : Records)
	{
		if (!Record.IsValid() || FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != Coord
			|| Seen.Contains(Record.EntityId))
		{
			return false;
		}
		Seen.Add(Record.EntityId);
		Previous = Record.EntityId;
	}
	return true;
}

bool FWorldCompressedChunk::IsValid() const
{
	return Revision != 0 && ContentHash.IsSet() && UncompressedSize > 0 && !Bytes.IsEmpty()
		&& BuildingEntityCount >= 0 && WorldObjectEntityCount >= 0 && ElementEntityCount >= 0;
}
