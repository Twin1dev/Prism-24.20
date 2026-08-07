#include "pch.h"

#include "ReplicationGraphTypes.h"

#include "ReplicationGraph.h"
#include "Templates/Casts.h"

PRISM_NAMESPACE_START

// --------------------------------------------------------------------------------------------------------------------------------------------
// FActorRepListRefView
// --------------------------------------------------------------------------------------------------------------------------------------------

std::string FActorRepListRefView::BuildDebugString() const
{
	// The generated FString is a view over memory the game allocated and cannot build a new
	// string, so debug text is narrow here and goes out through the log rather than an FString.
	std::string Result;
	for (int32 Index = 0; Index < Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += ", ";
		}

		Result += GetActorRepListTypeDebugString(RepList[Index]);
	}

	return Result;
}

void LogActorRepList(FReplicationGraphDebugInfo& DebugInfo, const std::string& Prefix, const FActorRepListRefView& List)
{
	if (List.Num() <= 0)
	{
		return;
	}

	std::string Line = Prefix + " [" + std::to_string(List.Num()) + "]: ";
	for (int32 Index = 0; Index < List.Num(); ++Index)
	{
		Line += GetActorRepListTypeDebugString(List[Index]);
		if (Index + 1 < List.Num())
		{
			Line += ", ";
		}
	}

	DebugInfo.Log(Line);
}

void FReplicationGraphDebugInfo::Log(const std::string& Str) const
{
	UE_LOG(LogReplicationGraph, Display, "%s%s", GetIndentString().c_str(), Str.c_str());
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// FNewReplicatedActorInfo
// --------------------------------------------------------------------------------------------------------------------------------------------

FName FNewReplicatedActorInfo::GetStreamingLevelNameOfActor(const AActor* Actor)
{
	if (!Actor)
	{
		return FName();
	}

	// Actors are outered to their level. Anything in the persistent level has no streaming level name.
	ULevel* Level = Cast<ULevel>(Actor->Outer);
	if (!Level)
	{
		return FName();
	}

	if (UWorld* OwningWorld = Level->OwningWorld)
	{
		if (OwningWorld->PersistentLevel == Level)
		{
			return FName();
		}
	}

	// The name the client knows the level by is the name of its package, which is the level's outermost object.
	UObject* Outermost = Level;
	while (Outermost->Outer != nullptr)
	{
		Outermost = Outermost->Outer;
	}

	return Outermost->Name;
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// FStreamingLevelActorListCollection
// --------------------------------------------------------------------------------------------------------------------------------------------

void FStreamingLevelActorListCollection::AddActor(const FNewReplicatedActorInfo& ActorInfo)
{
	FStreamingLevelActors* Item = StreamingLevelLists.FindByKey(ActorInfo.StreamingLevelName);
	if (!Item)
	{
		const int32 Index = StreamingLevelLists.Emplace(ActorInfo.StreamingLevelName);
		Item = &StreamingLevelLists[Index];
	}

	if (Item->ReplicationActorList.Contains(ActorInfo.Actor))
	{
		UE_LOG(LogReplicationGraph, Warning, "Actor %s already in FStreamingLevelActorListCollection for level %s",
			GetActorRepListTypeDebugString(ActorInfo.Actor).c_str(), ActorInfo.StreamingLevelName.ToString().c_str());
		return;
	}

	Item->ReplicationActorList.Add(ActorInfo.Actor);
}

bool FStreamingLevelActorListCollection::RemoveActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound, UReplicationGraphNode* Outer)
{
	bool bRemovedSomething = false;
	for (FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		if (StreamingList.StreamingLevelName == ActorInfo.StreamingLevelName)
		{
			bRemovedSomething = StreamingList.ReplicationActorList.RemoveSlow(ActorInfo.Actor);

			if (!bRemovedSomething && bWarnIfNotFound)
			{
				UE_LOG(LogReplicationGraph, Warning, "Attempted to remove %s from list %s but it was not found. (Outer: %s)",
					GetActorRepListTypeDebugString(ActorInfo.Actor).c_str(),
					ActorInfo.StreamingLevelName.ToString().c_str(),
					Outer ? Outer->GetDebugString().c_str() : "None");
			}

			break;
		}
	}

	return bRemovedSomething;
}

bool FStreamingLevelActorListCollection::RemoveActorFast(const FNewReplicatedActorInfo& ActorInfo, UReplicationGraphNode* Outer)
{
	bool bRemovedSomething = false;
	for (FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		if (StreamingList.StreamingLevelName == ActorInfo.StreamingLevelName)
		{
			bRemovedSomething = StreamingList.ReplicationActorList.RemoveFast(ActorInfo.Actor);
			break;
		}
	}

	return bRemovedSomething;
}

bool FStreamingLevelActorListCollection::RemoveActorFromLevelFast(AActor* Actor, FName LevelName)
{
	if (FStreamingLevelActors* Item = StreamingLevelLists.FindByKey(LevelName))
	{
		return Item->ReplicationActorList.RemoveFast(Actor);
	}

	return false;
}

void FStreamingLevelActorListCollection::Reset()
{
	for (FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		StreamingList.ReplicationActorList.Reset();
	}
}

void FStreamingLevelActorListCollection::Gather(const FConnectionGatherActorListParameters& Params) const
{
	for (const FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		if (Params.CheckClientVisibilityForLevel(StreamingList.StreamingLevelName))
		{
			Params.OutGatheredReplicationLists.AddReplicationActorList(StreamingList.ReplicationActorList);
		}
	}
}

void FStreamingLevelActorListCollection::Gather(const UNetReplicationGraphConnection& ConnectionManager, FGatheredReplicationActorLists& OutGatheredList) const
{
	const TSet<FName>& VisibleLevels = ConnectionManager.GetCachedClientVisibleLevelNames();

	for (const FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		if (VisibleLevels.Contains(StreamingList.StreamingLevelName))
		{
			OutGatheredList.AddReplicationActorList(StreamingList.ReplicationActorList);
		}
	}
}

void FStreamingLevelActorListCollection::AppendAllLists(FGatheredReplicationActorLists& OutGatheredList) const
{
	for (const FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		OutGatheredList.AddReplicationActorList(StreamingList.ReplicationActorList);
	}
}

bool FStreamingLevelActorListCollection::Contains(const FNewReplicatedActorInfo& ActorInfo) const
{
	for (const FStreamingLevelActors& StreamingList : StreamingLevelLists)
	{
		if (StreamingList.StreamingLevelName == ActorInfo.StreamingLevelName)
		{
			return StreamingList.ReplicationActorList.Contains(ActorInfo.Actor);
		}
	}

	return false;
}

void FStreamingLevelActorListCollection::DeepCopyFrom(const FStreamingLevelActorListCollection& Source)
{
	StreamingLevelLists.Reset();
	for (const FStreamingLevelActors& StreamingLevel : Source.StreamingLevelLists)
	{
		if (StreamingLevel.ReplicationActorList.Num() > 0)
		{
			const int32 Index = StreamingLevelLists.Emplace(StreamingLevel.StreamingLevelName);
			StreamingLevelLists[Index].ReplicationActorList.CopyContentsFrom(StreamingLevel.ReplicationActorList);
		}
	}
}

void FStreamingLevelActorListCollection::GetAll_Debug(TArray<FActorRepListType>& OutArray) const
{
	for (const FStreamingLevelActors& StreamingLevel : StreamingLevelLists)
	{
		StreamingLevel.ReplicationActorList.AppendToTArray(OutArray);
	}
}

void FStreamingLevelActorListCollection::TearDown()
{
	for (FStreamingLevelActors& StreamingLevel : StreamingLevelLists)
	{
		StreamingLevel.ReplicationActorList.TearDown();
	}

	StreamingLevelLists.Empty();
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// FLevelBasedActorList
// --------------------------------------------------------------------------------------------------------------------------------------------

void FLevelBasedActorList::AddNetworkActor(AActor* NetActor)
{
	FNewReplicatedActorInfo ActorInfo(NetActor);

	if (ActorInfo.StreamingLevelName.IsNone())
	{
		PermanentLevelActors.ConditionalAdd(NetActor);
	}
	else
	{
		StreamingLevelActors.AddActor(ActorInfo);
	}
}

bool FLevelBasedActorList::RemoveNetworkActor(const FNewReplicatedActorInfo& NetActor)
{
	if (NetActor.StreamingLevelName.IsNone())
	{
		return PermanentLevelActors.RemoveFast(NetActor.Actor);
	}

	return StreamingLevelActors.RemoveActorFast(NetActor);
}

void FLevelBasedActorList::Reset()
{
	PermanentLevelActors.Reset();
	StreamingLevelActors.Reset();
}

bool FLevelBasedActorList::IsEmpty() const
{
	if (!PermanentLevelActors.IsEmpty())
	{
		return false;
	}

	for (const FStreamingLevelActorListCollection::FStreamingLevelActors& StreamingList : StreamingLevelActors.StreamingLevelLists)
	{
		if (StreamingList.ReplicationActorList.Num() > 0)
		{
			return false;
		}
	}

	return true;
}

bool FLevelBasedActorList::Contains(AActor* NetActor) const
{
	if (PermanentLevelActors.Contains(NetActor))
	{
		return true;
	}

	return StreamingLevelActors.Contains(FNewReplicatedActorInfo(NetActor));
}

void FLevelBasedActorList::Gather(const FConnectionGatherActorListParameters& Params) const
{
	Params.OutGatheredReplicationLists.AddReplicationActorList(PermanentLevelActors);
	StreamingLevelActors.Gather(Params);
}

void FLevelBasedActorList::Gather(const UNetReplicationGraphConnection& ConnectionManager, FGatheredReplicationActorLists& OutGatheredList) const
{
	OutGatheredList.AddReplicationActorList(PermanentLevelActors);
	StreamingLevelActors.Gather(ConnectionManager, OutGatheredList);
}

void FLevelBasedActorList::AppendAllLists(FGatheredReplicationActorLists& OutGatheredList) const
{
	OutGatheredList.AddReplicationActorList(PermanentLevelActors);
	StreamingLevelActors.AppendAllLists(OutGatheredList);
}

void FLevelBasedActorList::GetAllActors(TArray<AActor*>& OutAllActors) const
{
	PermanentLevelActors.AppendToTArray(OutAllActors);
	StreamingLevelActors.GetAll_Debug(OutAllActors);
}

void FLevelBasedActorList::UpdateActorLevel(AActor* NetActor, FName PreviousLevelName)
{
	if (PreviousLevelName.IsNone())
	{
		PermanentLevelActors.RemoveFast(NetActor);
	}
	else
	{
		StreamingLevelActors.RemoveActorFromLevelFast(NetActor, PreviousLevelName);
	}

	AddNetworkActor(NetActor);
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// FClassReplicationInfo
// --------------------------------------------------------------------------------------------------------------------------------------------

std::string FClassReplicationInfo::BuildDebugStringDelta() const
{
	FClassReplicationInfo DefaultValues;
	std::string Str;

	auto AppendFloat = [&Str](const char* Label, float Value)
	{
		char Buffer[128];
		std::snprintf(Buffer, sizeof(Buffer), "%s: %.2f ", Label, Value);
		Str += Buffer;
	};

	auto AppendInt = [&Str](const char* Label, int32 Value)
	{
		char Buffer[128];
		std::snprintf(Buffer, sizeof(Buffer), "%s: %d ", Label, Value);
		Str += Buffer;
	};

	if (CullDistance != DefaultValues.CullDistance)
	{
		AppendFloat("CullDistance", CullDistance);
	}
	if (StarvationPriorityScale != DefaultValues.StarvationPriorityScale)
	{
		AppendFloat("StarvationPriorityScale", StarvationPriorityScale);
	}
	if (DistancePriorityScale != DefaultValues.DistancePriorityScale)
	{
		AppendFloat("DistancePriorityScale", DistancePriorityScale);
	}
	if (ReplicationPeriodFrame != DefaultValues.ReplicationPeriodFrame)
	{
		AppendInt("ReplicationPeriodFrame", ReplicationPeriodFrame);
	}
	if (FastPath_ReplicationPeriodFrame != DefaultValues.FastPath_ReplicationPeriodFrame)
	{
		AppendInt("FastPath_ReplicationPeriodFrame", FastPath_ReplicationPeriodFrame);
	}
	if (ActorChannelFrameTimeout != DefaultValues.ActorChannelFrameTimeout)
	{
		AppendInt("ActorChannelFrameTimeout", ActorChannelFrameTimeout);
	}
	if (FastSharedReplicationFunc)
	{
		Str += "FastSharedReplicationFunc is SET.";
	}

	return Str;
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// FGlobalActorReplicationInfoMap
// --------------------------------------------------------------------------------------------------------------------------------------------

int32 FGlobalActorReplicationInfoMap::Remove(const FActorRepListType& RemovedActor)
{
	if (FGlobalActorReplicationInfo* ActorInfo = Find(RemovedActor))
	{
		RemoveAllActorDependencies(RemovedActor, ActorInfo);
	}

	return ActorMap.Remove(RemovedActor);
}

void FGlobalActorReplicationInfoMap::NotifyActorRenamed(AActor* Actor, FName PreviousStreamingLevelName)
{
	if (FGlobalActorReplicationInfo* ActorInfo = Find(Actor))
	{
		ActorInfo->NotifyActorRenamed(Actor, PreviousStreamingLevelName);
	}
}

void FGlobalActorReplicationInfoMap::AddDependentActor(AActor* Parent, AActor* Child, EWarnFlag WarnFlag)
{
	if (!Parent || !Child)
	{
		return;
	}

	FGlobalActorReplicationInfo& ParentInfo = Get(Parent);

	if (ParentInfo.DependentActorList.Contains(Child))
	{
		if ((static_cast<uint8>(WarnFlag) & static_cast<uint8>(EWarnFlag::WarnAlreadyDependant)) != 0)
		{
			UE_LOG(LogReplicationGraph, Warning, "AddDependentActor: %s is already a dependent of %s",
				GetActorRepListTypeDebugString(Child).c_str(), GetActorRepListTypeDebugString(Parent).c_str());
		}
		return;
	}

	ParentInfo.DependentActorList.AddNetworkActor(Child);

	FGlobalActorReplicationInfo& ChildInfo = Get(Child);
	ChildInfo.ParentActorList.AddUnique(Parent);
}

void FGlobalActorReplicationInfoMap::RemoveAllActorDependencies(AActor* MainActor, FGlobalActorReplicationInfo* MainActorInfo)
{
	TArray<AActor*> DependentActors;
	MainActorInfo->DependentActorList.GetAllActors(DependentActors);

	// Take this actor off every child's parent list.
	for (AActor* DependentActor : DependentActors)
	{
		if (FGlobalActorReplicationInfo* ChildInfo = Find(DependentActor))
		{
			ChildInfo->ParentActorList.RemoveSingleSwap(MainActor);
		}
	}

	// And off every parent's dependent list.
	FNewReplicatedActorInfo LevelActorInfo(MainActor);

	for (AActor* ParentActor : MainActorInfo->ParentActorList)
	{
		if (FGlobalActorReplicationInfo* ParentInfo = Find(ParentActor))
		{
			ParentInfo->DependentActorList.RemoveNetworkActor(LevelActorInfo);
		}
	}

	MainActorInfo->DependentActorList.Reset();
	MainActorInfo->ParentActorList.Reset();
}

PRISM_NAMESPACE_END
