#include "pch.h"

#include "ReplicationGraph.h"

#include "Engine/ActorChannel.h"
#include "Engine/NetConnection.h"
#include "GameFramework/PlayerController.h"
#include "Templates/Casts.h"

PRISM_NAMESPACE_START

namespace RepGraphConsoleVariables
{
	/** Frames a UReplicationGraphNode_ConnectionDormancyNode may go ungathered before it is thrown away. 0 disables. */
	uint32 ConnectionDormancy_NumFramesUntilObsolete = 0;

	/**
	 * Connections whose view location is above this pull nothing from a dormancy node.
	 *
	 * This is WORLD_MAX, i.e. the check is off, which is what the shipping binary carries:
	 * ?MaxZForConnection@UReplicationGraphNode_DormancyNode@@2NA holds 0x42A0000000000000 =
	 * 2^43, UE5's large-world WORLD_MAX, and nothing in the image ever writes it.
	 *
	 * It was 3000.0 here, which is a guess and a catastrophic one: Z is in centimetres, so
	 * 3000 is thirty metres. Athena terrain sits well above that, and every viewer above it
	 * made UReplicationGraphNode_DormancyNode::GatherActorListsForConnection return without
	 * gathering anything. Nothing routed to Spatialize_Dormancy replicated - which is exactly
	 * and only the BuildingSMActor / BuildingContainer / BuildingGameplayActor / FortPickup
	 * family, so doors, chests and ammo boxes never left the server while everything else
	 * worked.
	 */
	double DormancyNode_MaxZForConnection = 8796093022208.0;
}

/**
 * Diagnostic: per-frame replication summary, plus every write of a connection's own
 * PlayerController. Off by default - it logs twice per frame to the console, which is enough
 * synchronous I/O to hitch the server tick.
 *
 * This is what found the frozen ReplicationFrame: "wrote=1" every frame while everything else
 * wrote nothing is the signature of only bNetInitial actors being compared. Worth re-enabling
 * if replication ever looks stale again.
 */
int32 LogOwnerReplication = 0;

/**
 * Diagnostic: every N frames, dump why each connection ended up with the actors it did.
 *
 * There are four places a spatialized actor can be lost between existing and arriving, and from
 * the outside they all look identical - the other player is simply not there. This separates
 * them in one run:
 *
 *   - the grid line says where the connection thinks it is, which cell that resolves to, and how
 *     many actors that cell holds. A viewer at a nonsense location, or an empty cell, is visible
 *     immediately.
 *   - the summary line counts every skip reason in the prioritize pass, so "gathered but culled"
 *     and "never gathered" are distinguishable.
 *   - the pawn lines name every pawn the graph knows about, where it is, and what happened to it
 *     for this connection.
 *
 * Set to 0 to turn off. It builds class name strings, so it is only ever run on a diag frame.
 */
int32 LogRelevancyDiagnostics = 120;

namespace
{
	bool IsRelevancyDiagFrame(uint32 FrameNum)
	{
		return LogRelevancyDiagnostics > 0 && (FrameNum % static_cast<uint32>(LogRelevancyDiagnostics)) == 0;
	}

	/** Cheap "is this one of the actors we care about" test, used only on diag frames. */
	bool IsPawnishClass(const AActor* Actor)
	{
		if (!Actor || !Actor->Class)
		{
			return false;
		}

		const std::string ClassName = Actor->Class->GetName();
		return ClassName.find("Pawn") != std::string::npos;
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// FNetViewer
// --------------------------------------------------------------------------------------------------------------------------------------------

FNetViewer::FNetViewer(UNetConnection* InConnection, float DeltaSeconds)
	: Connection(InConnection)
{
	if (!InConnection)
	{
		return;
	}

	PRISM_NAMESPACE_NAME::APlayerController* ViewingController = ToPrism(InConnection->GetPlayerController());

	// A connection without a PlayerController still has an owning actor - that is what a
	// beacon connection is - and the engine views the world through that instead.
	InViewer = ViewingController ? ViewingController->AsActor() : InConnection->GetOwningActor();

	/*
	 * The engine's only writer of UNetConnection::ViewTarget is
	 * UNetDriver::ServerReplicateActors_PrepConnections, which is inside WITH_SERVER_CODE and
	 * was compiled out of this binary. The field is therefore not maintained, and reading it
	 * gives whatever it happened to be left at - so the view target is resolved here exactly
	 * as that function would have resolved it, and written back so that anything else reading
	 * the connection agrees with the graph.
	 */
	AActor* DesiredViewTarget = InConnection->GetOwningActor();
	if (ViewingController)
	{
		if (AActor* ControllerViewTarget = ViewingController->GetViewTarget())
		{
			DesiredViewTarget = ControllerViewTarget;
		}
	}

	if (!DesiredViewTarget)
	{
		DesiredViewTarget = InViewer;
	}

	InConnection->SetViewTarget(DesiredViewTarget);
	ViewTarget = DesiredViewTarget;

	if (!ViewTarget)
	{
		return;
	}

	// Seeded from the view target and then refined by the controller, which is the engine's
	// order: GetPlayerViewPoint overwrites both outputs on every path it can take.
	ViewLocation = ViewTarget->GetActorLocation();

	if (ViewingController)
	{
		FRotator ViewRotation = ViewingController->AsController()->GetControlRotation();
		ViewingController->GetPlayerViewPoint(ViewLocation, ViewRotation);
		ViewDir = ViewRotation.Vector();
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode
// --------------------------------------------------------------------------------------------------------------------------------------------

UReplicationGraphNode::~UReplicationGraphNode()
{
	for (UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		delete ChildNode;
	}

	AllChildNodes.Reset();
}

void UReplicationGraphNode::NotifyResetAllNetworkActors()
{
	for (UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->NotifyResetAllNetworkActors();
	}
}

void UReplicationGraphNode::TearDown()
{
	for (UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->TearDown();
	}
}

void UReplicationGraphNode::GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const
{
	for (const UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->GetAllActorsInNode_Debugging(OutArray);
	}
}

void UReplicationGraphNode::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);

	DebugInfo.PushIndent();
	for (const UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->LogNode(DebugInfo, ChildNode->GetDebugString());
	}
	DebugInfo.PopIndent();
}

bool UReplicationGraphNode::RemoveChildNode(UReplicationGraphNode* OutChildNode, NodeOrdering NodeOrder)
{
	const int32 Index = AllChildNodes.IndexOfByKey(OutChildNode);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	OutChildNode->TearDown();

	if (NodeOrder == NodeOrdering::KeepOrder)
	{
		AllChildNodes.RemoveAt(Index);
	}
	else
	{
		AllChildNodes.RemoveAtSwap(Index);
	}

	delete OutChildNode;
	return true;
}

void UReplicationGraphNode::CleanChildNodes(NodeOrdering NodeOrder)
{
	if (NodeOrder == NodeOrdering::KeepOrder)
	{
		AllChildNodes.RemoveAll([](const UReplicationGraphNode* Node) { return Node == nullptr; });
	}
	else
	{
		for (int32 Index = AllChildNodes.Num() - 1; Index >= 0; --Index)
		{
			if (AllChildNodes[Index] == nullptr)
			{
				AllChildNodes.RemoveAtSwap(Index);
			}
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_ActorList
// --------------------------------------------------------------------------------------------------------------------------------------------

void UReplicationGraphNode_ActorList::NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo)
{
	if (ActorInfo.StreamingLevelName.IsNone())
	{
		ReplicationActorList.ConditionalAdd(ActorInfo.Actor);
	}
	else
	{
		StreamingLevelCollection.AddActor(ActorInfo);
	}
}

bool UReplicationGraphNode_ActorList::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	if (ActorInfo.StreamingLevelName.IsNone())
	{
		if (ReplicationActorList.RemoveSlow(ActorInfo.Actor))
		{
			return true;
		}

		if (bWarnIfNotFound)
		{
			UE_LOG(LogReplicationGraph, Warning, "Attempted to remove %s from list %s but it was not found.",
				GetActorRepListTypeDebugString(ActorInfo.Actor).c_str(), GetDebugString().c_str());
		}

		return false;
	}

	return StreamingLevelCollection.RemoveActor(ActorInfo, bWarnIfNotFound, this);
}

bool UReplicationGraphNode_ActorList::RemoveNetworkActorFast(const FNewReplicatedActorInfo& ActorInfo)
{
	if (ActorInfo.StreamingLevelName.IsNone())
	{
		return ReplicationActorList.RemoveFast(ActorInfo.Actor);
	}

	return StreamingLevelCollection.RemoveActorFast(ActorInfo, this);
}

bool UReplicationGraphNode_ActorList::NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	// Only the level the actor is filed under changes, so take it out of the old place and put it in the new one.
	const bool bRemoved = NotifyRemoveNetworkActor(ActorInfo.OldActorInfo, bWarnIfNotFound);
	if (bRemoved)
	{
		NotifyAddNetworkActor(ActorInfo.NewActorInfo);
	}

	return bRemoved;
}

void UReplicationGraphNode_ActorList::NotifyResetAllNetworkActors()
{
	ReplicationActorList.Reset();
	StreamingLevelCollection.Reset();

	UReplicationGraphNode::NotifyResetAllNetworkActors();
}

void UReplicationGraphNode_ActorList::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);

	StreamingLevelCollection.Gather(Params);

	for (UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->GatherActorListsForConnection(Params);
	}
}

void UReplicationGraphNode_ActorList::DeepCopyActorListsFrom(const UReplicationGraphNode_ActorList* Source)
{
	if (Source->ReplicationActorList.Num() > 0)
	{
		ReplicationActorList.CopyContentsFrom(Source->ReplicationActorList);
	}

	StreamingLevelCollection.DeepCopyFrom(Source->StreamingLevelCollection);
}

void UReplicationGraphNode_ActorList::TearDown()
{
	UReplicationGraphNode::TearDown();

	ReplicationActorList.TearDown();
	StreamingLevelCollection.TearDown();
}

void UReplicationGraphNode_ActorList::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();

	LogActorList(DebugInfo);

	for (const UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->LogNode(DebugInfo, ChildNode->GetDebugString());
	}

	DebugInfo.PopIndent();
}

void UReplicationGraphNode_ActorList::GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const
{
	ReplicationActorList.AppendToTArray(OutArray);
	StreamingLevelCollection.GetAll_Debug(OutArray);

	UReplicationGraphNode::GetAllActorsInNode_Debugging(OutArray);
}

void UReplicationGraphNode_ActorList::LogActorList(FReplicationGraphDebugInfo& DebugInfo) const
{
	PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "ReplicationActorList", ReplicationActorList);

	for (const FStreamingLevelActorListCollection::FStreamingLevelActors& StreamingLevel : StreamingLevelCollection.StreamingLevelLists)
	{
		PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "Level: " + StreamingLevel.StreamingLevelName.ToString(), StreamingLevel.ReplicationActorList);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_ActorListFrequencyBuckets
// --------------------------------------------------------------------------------------------------------------------------------------------

UReplicationGraphNode_ActorListFrequencyBuckets::FSettings UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings;

void UReplicationGraphNode_ActorListFrequencyBuckets::SetNonStreamingCollectionSize(const int32 NewSize)
{
	if (NewSize <= 0)
	{
		// A node with no buckets can hold nothing and would divide by zero dealing the actors
		// back out. The console command that resets bucket counts can ask for this.
		UE_LOG(LogReplicationGraph, Warning, "SetNonStreamingCollectionSize ignored: %d is not a usable bucket count.", NewSize);
		return;
	}

	// Everything is pulled out, the collection is resized, and the actors are dealt back out round robin.
	TArray<FActorRepListType> FullList;
	for (FActorRepListRefView& List : NonStreamingCollection)
	{
		List.AppendToTArray(FullList);
		List.TearDown();
	}

	NonStreamingCollection.SetNum(NewSize);
	for (FActorRepListRefView& List : NonStreamingCollection)
	{
		List.Reset();
	}

	for (int32 Index = 0; Index < FullList.Num(); ++Index)
	{
		NonStreamingCollection[Index % NewSize].Add(FullList[Index]);
	}

	TotalNumNonStreamingActors = FullList.Num();
}

void UReplicationGraphNode_ActorListFrequencyBuckets::NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo)
{
	if (ActorInfo.StreamingLevelName.IsNone())
	{
		if (NonStreamingCollection.Num() == 0)
		{
			// Can only happen if the settings asked for zero buckets. Restore one so the actor
			// has somewhere to go rather than dropping it.
			SetNonStreamingCollectionSize(1);
		}

		// Add to the smallest bucket so the buckets stay even.
		FActorRepListRefView* BestList = &NonStreamingCollection[0];
		for (FActorRepListRefView& List : NonStreamingCollection)
		{
			if (List.Num() < BestList->Num())
			{
				BestList = &List;
			}
		}

		BestList->Add(ActorInfo.Actor);
		TotalNumNonStreamingActors++;

		CheckRebalance();
	}
	else
	{
		StreamingLevelCollection.AddActor(ActorInfo);
	}
}

bool UReplicationGraphNode_ActorListFrequencyBuckets::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	if (ActorInfo.StreamingLevelName.IsNone())
	{
		for (FActorRepListRefView& List : NonStreamingCollection)
		{
			if (List.RemoveFast(ActorInfo.Actor))
			{
				TotalNumNonStreamingActors--;
				CheckRebalance();
				return true;
			}
		}

		if (bWarnIfNotFound)
		{
			UE_LOG(LogReplicationGraph, Warning, "Attempted to remove %s from frequency buckets but it was not found.",
				GetActorRepListTypeDebugString(ActorInfo.Actor).c_str());
		}

		return false;
	}

	return StreamingLevelCollection.RemoveActor(ActorInfo, bWarnIfNotFound, this);
}

bool UReplicationGraphNode_ActorListFrequencyBuckets::NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	const bool bRemoved = NotifyRemoveNetworkActor(ActorInfo.OldActorInfo, bWarnIfNotFound);
	if (bRemoved)
	{
		NotifyAddNetworkActor(ActorInfo.NewActorInfo);
	}

	return bRemoved;
}

void UReplicationGraphNode_ActorListFrequencyBuckets::NotifyResetAllNetworkActors()
{
	for (FActorRepListRefView& List : NonStreamingCollection)
	{
		List.Reset();
	}

	TotalNumNonStreamingActors = 0;
	StreamingLevelCollection.Reset();
}

void UReplicationGraphNode_ActorListFrequencyBuckets::CheckRebalance()
{
	const FSettings& MySettings = GetSettings();

	int32 DesiredNumBuckets = MySettings.NumBuckets;
	for (const FSettings::FBucketThresholds& Threshold : MySettings.BucketThresholds)
	{
		if (TotalNumNonStreamingActors <= Threshold.MaxActors)
		{
			DesiredNumBuckets = Threshold.NumBuckets;
			break;
		}
	}

	if (DesiredNumBuckets != NonStreamingCollection.Num() && DesiredNumBuckets > 0)
	{
		SetNonStreamingCollectionSize(DesiredNumBuckets);
	}
}

void UReplicationGraphNode_ActorListFrequencyBuckets::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	const FSettings& MySettings = GetSettings();

	const int32 NumBuckets = NonStreamingCollection.Num();
	if (NumBuckets > 0)
	{
		const int32 ThisFrameBucket = static_cast<int32>(Params.ReplicationFrameNum % static_cast<uint32>(NumBuckets));

		// The modulo is settings-driven, so it is only trusted once and never as a divisor.
		const int32 FastPathModulo = FMath::Max(1, MySettings.FastPathFrameModulo);
		const bool bFastPathThisFrame = MySettings.EnableFastPath
			&& (Params.ReplicationFrameNum % static_cast<uint32>(FastPathModulo)) == 0;

		for (int32 BucketIndex = 0; BucketIndex < NumBuckets; ++BucketIndex)
		{
			if (BucketIndex == ThisFrameBucket)
			{
				Params.OutGatheredReplicationLists.AddReplicationActorList(NonStreamingCollection[BucketIndex]);
			}
			else if (bFastPathThisFrame)
			{
				// Off-frame buckets can still go out on the FastShared path, which is much cheaper.
				Params.OutGatheredReplicationLists.AddReplicationActorList(NonStreamingCollection[BucketIndex], EActorRepListTypeFlags::FastShared);
			}
		}
	}

	StreamingLevelCollection.Gather(Params);

	for (UReplicationGraphNode* ChildNode : AllChildNodes)
	{
		ChildNode->GatherActorListsForConnection(Params);
	}
}

void UReplicationGraphNode_ActorListFrequencyBuckets::TearDown()
{
	UReplicationGraphNode::TearDown();

	for (FActorRepListRefView& List : NonStreamingCollection)
	{
		List.TearDown();
	}

	StreamingLevelCollection.TearDown();
}

void UReplicationGraphNode_ActorListFrequencyBuckets::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();

	int32 BucketIndex = 0;
	for (const FActorRepListRefView& List : NonStreamingCollection)
	{
		PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "Bucket[" + std::to_string(BucketIndex++) + "]", List);
	}

	DebugInfo.PopIndent();
}

void UReplicationGraphNode_ActorListFrequencyBuckets::GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const
{
	for (const FActorRepListRefView& List : NonStreamingCollection)
	{
		List.AppendToTArray(OutArray);
	}

	StreamingLevelCollection.GetAll_Debug(OutArray);
	UReplicationGraphNode::GetAllActorsInNode_Debugging(OutArray);
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_DynamicSpatialFrequency
// --------------------------------------------------------------------------------------------------------------------------------------------

namespace RepGraphDynamicSpatialFrequency
{
	/** UNetDriver::NetServerMaxTickRate. The zones below are expressed in Hz and converted against it. */
	constexpr float AssumedTickRate = 30.f;

	/** The node's own bandwidth allowance, before the connection's. 10 KB/sec. */
	constexpr int64 BitsPerFrame = static_cast<int64>((10.f * 1024.f * 8.f) / AssumedTickRate);

	/**
	 * The zones, in the engine's order and with the engine's numbers.
	 *
	 * A zone is chosen by the dot product between the connection's view direction and the
	 * direction to the actor, and MinDotProduct is the *upper* bound of its band - the first
	 * zone whose MinDotProduct the dot product does not exceed wins. Ordered ascending, that
	 * reads as "behind the viewer", "in front but off to the side", "in the viewer's FOV",
	 * and the rate rises as the actor gets closer to what the player is looking at.
	 *
	 * Within a zone, distance decides the rate: MinDistPct of the cull distance replicates at
	 * MinDistHz and MaxDistPct at MaxDistHz, interpolated between.
	 */
	static TArray<UReplicationGraphNode_DynamicSpatialFrequency::FSpatializationZone> MakeDefaultZones()
	{
		TArray<UReplicationGraphNode_DynamicSpatialFrequency::FSpatializationZone> Zones;
		Zones.Reserve(3);

		//                 DOT     MinPCT MaxPCT   [Default]           [FastShared]
		//                                         MinHz  MaxHz        MinHz  MaxHz
		Zones.Emplace(      0.00f, 0.05f, 0.10f,    1.f,   1.f,          0.f,   0.f, AssumedTickRate);	// Behind the viewer
		Zones.Emplace(      0.71f, 0.05f, 0.10f,    1.f,   1.f,          0.f,   0.f, AssumedTickRate);	// In front, but not in FOV
		Zones.Emplace(      1.00f, 0.10f, 0.50f,    5.f,   1.f,         20.f,  10.f, AssumedTickRate);	// Directly in the viewer's FOV

		return Zones;
	}

	/**
	 * The same, for actors with no FastShared support: the default path has to carry the whole rate.
	 *
	 * Epic's numbers, verified line-for-line against the engine. They were briefly retuned here to
	 * compensate for pawns having no fast path, and that made things worse rather than better, so
	 * they are back as shipped - which is also the right table now that pawn movement really is
	 * carried by a fast path again. See UFortReplicationGraph::DriveFastSharedReplication.
	 */
	static TArray<UReplicationGraphNode_DynamicSpatialFrequency::FSpatializationZone> MakeDefaultZones_NoFastShared()
	{
		TArray<UReplicationGraphNode_DynamicSpatialFrequency::FSpatializationZone> Zones;
		Zones.Reserve(3);

		//                 DOT     MinPCT MaxPCT   [Default]           [FastShared disabled]
		//                                         MinHz  MaxHz        MinHz  MaxHz
		Zones.Emplace(      0.00f, 0.05f, 0.10f,    5.f,   1.f,          0.f,   0.f, AssumedTickRate);	// Behind the viewer
		Zones.Emplace(      0.71f, 0.05f, 0.10f,   10.f,   5.f,          0.f,   0.f, AssumedTickRate);	// In front, but not in FOV
		Zones.Emplace(      1.00f, 0.10f, 0.50f,   20.f,   5.f,          0.f,   0.f, AssumedTickRate);	// Directly in the viewer's FOV

		return Zones;
	}

	/**
	 * Turns a distance-within-zone fraction into a replication period, and works out the frame
	 * the actor is next allowed to replicate on. Mirrors CalcDynamicReplicationPeriod.
	 */
	inline void CalcDynamicReplicationPeriod(float FinalPct, uint32 MinRepPeriod, uint32 MaxRepPeriod,
		uint16& OutReplicationPeriodFrame, uint32& OutNextReplicationFrame, uint32 LastRepFrameNum)
	{
		const float PeriodRange = static_cast<float>(MaxRepPeriod) - static_cast<float>(MinRepPeriod);
		const uint32 ExtraPeriod = static_cast<uint32>(FMath::CeilToInt(PeriodRange * FinalPct));

		OutReplicationPeriodFrame = static_cast<uint16>(FMath::Clamp<uint32>(MinRepPeriod + ExtraPeriod, 1u, static_cast<uint32>(MAX_uint16)));
		OutNextReplicationFrame = LastRepFrameNum + OutReplicationPeriodFrame;
	}

	/** An actor already replicating every frame cannot be spread out any further. */
	inline bool ReplicatesEveryFrame(const FConnectionReplicationActorInfo& ConnectionInfo, bool bCheckFastPath)
	{
		return !(ConnectionInfo.ReplicationPeriodFrame > 1 && (!bCheckFastPath || ConnectionInfo.FastPath_ReplicationPeriodFrame > 1));
	}
}

UReplicationGraphNode_DynamicSpatialFrequency::FSettings UReplicationGraphNode_DynamicSpatialFrequency::DefaultSettings;

UReplicationGraphNode_DynamicSpatialFrequency::UReplicationGraphNode_DynamicSpatialFrequency()
{
	bRequiresPrepareForReplicationCall = false;

	/*
	 * Filled on first use rather than at static-init time, which is where the engine builds it.
	 *
	 * The zone tables are TArrays, and a TArray here allocates out of the game's FMalloc. Doing
	 * that from a static initializer would make the module's load order depend on the game's
	 * allocator already being usable; doing it from the first node's constructor happens well
	 * after the graph, the world and the net driver all exist. Game code that wants different
	 * zones overwrites DefaultSettings after that, exactly as before.
	 */
	static bool bDefaultSettingsInitialized = false;
	if (!bDefaultSettingsInitialized)
	{
		bDefaultSettingsInitialized = true;

		DefaultSettings = FSettings(
			RepGraphDynamicSpatialFrequency::MakeDefaultZones(),
			RepGraphDynamicSpatialFrequency::MakeDefaultZones_NoFastShared(),
			RepGraphDynamicSpatialFrequency::BitsPerFrame);
	}
}

void UReplicationGraphNode_DynamicSpatialFrequency::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	UReplicationGraph* RepGraph = GraphGlobals.IsValid() ? GraphGlobals->ReplicationGraph : nullptr;
	if (!RepGraph)
	{
		return;
	}

	FGlobalActorReplicationInfoMap& GlobalMap = RepGraph->GetGlobalActorReplicationInfoMap();
	FPerConnectionActorInfoMap& ConnectionMap = Params.ConnectionManager.ActorInfoMap;

	SortedReplicationList.Reset();
	NumExpectedReplicationsThisFrame = 0;
	NumExpectedReplicationsNextFrame = 0;

	UNetConnection* NetConnection = Params.ConnectionManager.NetConnection;

	FSettings& MySettings = GetSettings();

	if (MySettings.MaxNearestActors >= 0 && ReplicationActorList.Num() > MySettings.MaxNearestActors)
	{
		// Two passes: sort by distance, keep the closest N, then work out frequencies for those.
		// The first pass fills SortedReplicationList with distances, so the second pass rewrites
		// each item in place rather than building a second array.
		GatherActors_DistanceOnly(ReplicationActorList, GlobalMap, ConnectionMap, Params);

		SortedReplicationList.Sort();
		SortedReplicationList.SetNum(FMath::Min(MySettings.MaxNearestActors, SortedReplicationList.Num()));

		// Backwards, because an actor that turns out not to belong here is removed by swapping
		// the last item into its slot.
		for (int32 Index = SortedReplicationList.Num() - 1; Index >= 0; --Index)
		{
			const FDynamicSpatialFrequency_SortedItem& Item = SortedReplicationList[Index];
			CalcFrequencyForActor(Item.Actor, RepGraph, NetConnection, *Item.GlobalInfo, ConnectionMap, MySettings, Params.Viewers, Params.ReplicationFrameNum, Index);
		}
	}
	else
	{
		GatherActors(ReplicationActorList, GlobalMap, ConnectionMap, Params, NetConnection);
	}

	StreamingLevelCollection.Gather(Params);

	// Most urgent first: FramesTillReplicate is negative for an actor that is already overdue.
	SortedReplicationList.Sort();

	/*
	 * Opportunistic load balance.
	 *
	 * CalcFrequencyForActor counted how many actors want this frame and how many want the next
	 * one. When this frame is the busier of the two, half the difference is held back a frame -
	 * they are not starved, only deferred, and the traffic evens out over time. Actors that
	 * replicate every frame anyway, and actors that are already overdue, are never deferred.
	 */
	int32 OpportunisticLoadBalanceQuota = (NumExpectedReplicationsThisFrame - NumExpectedReplicationsNextFrame) >> 1;

	/*
	 * The engine's node replicates the sorted list itself, against its own MaxBitsPerFrame. Here
	 * the list is handed back to the graph, which has one prioritization and one bandwidth budget
	 * for every node's output - so the node's job ends at deciding *when* each actor is due, which
	 * is the part that makes it a dynamic spatial frequency node. MaxBitsPerFrame is therefore
	 * unused on this path and is left in FSettings only because game code sets it.
	 */
	FActorRepListRefView& GatheredList = SortedGatherList;
	GatheredList.Reset(SortedReplicationList.Num());

	for (const FDynamicSpatialFrequency_SortedItem& Item : SortedReplicationList)
	{
		if (Item.Actor == nullptr)
		{
			continue;
		}

		if (OpportunisticLoadBalanceQuota > 0
			&& Item.FramesTillReplicate == 0
			&& Item.ConnectionInfo != nullptr
			&& !RepGraphDynamicSpatialFrequency::ReplicatesEveryFrame(*Item.ConnectionInfo, Item.EnableFastPath))
		{
			--OpportunisticLoadBalanceQuota;
			continue;
		}

		GatheredList.Add(Item.Actor);
	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(GatheredList);
}

void UReplicationGraphNode_DynamicSpatialFrequency::GatherActors(const FActorRepListRefView& RepList, FGlobalActorReplicationInfoMap& GlobalMap, FPerConnectionActorInfoMap& ConnectionMap, const FConnectionGatherActorListParameters& Params, UNetConnection* NetConnection)
{
	UReplicationGraph* RepGraph = GraphGlobals->ReplicationGraph;
	FSettings& MySettings = GetSettings();

	for (FActorRepListType Actor : RepList)
	{
		if (!IsActorValidForReplicationGather(Actor))
		{
			continue;
		}

		FGlobalActorReplicationInfo& GlobalInfo = GlobalMap.Get(Actor);
		CalcFrequencyForActor(Actor, RepGraph, NetConnection, GlobalInfo, ConnectionMap, MySettings, Params.Viewers, Params.ReplicationFrameNum, INDEX_NONE);
	}
}

void UReplicationGraphNode_DynamicSpatialFrequency::GatherActors_DistanceOnly(const FActorRepListRefView& RepList, FGlobalActorReplicationInfoMap& GlobalMap, FPerConnectionActorInfoMap& ConnectionMap, const FConnectionGatherActorListParameters& Params)
{
	for (FActorRepListType Actor : RepList)
	{
		if (!IsActorValidForReplicationGather(Actor))
		{
			continue;
		}

		FGlobalActorReplicationInfo& GlobalInfo = GlobalMap.Get(Actor);

		double ClosestDistSq = MAX_dbl;
		for (const FNetViewer& Viewer : Params.Viewers)
		{
			ClosestDistSq = FMath::Min(ClosestDistSq, VectorDistSquared(GlobalInfo.WorldLocation, Viewer.ViewLocation));
		}

		// This pass only orders the list, and FramesTillReplicate is the field it orders on, so
		// the squared distance is parked there. Clamped because a squared distance in world
		// units passes int32 at 46km, well inside the map.
		const int32 SortKey = static_cast<int32>(FMath::Min(ClosestDistSq, static_cast<double>(MAX_int32)));

		SortedReplicationList.Emplace(Actor, SortKey, &GlobalInfo);
	}
}

void UReplicationGraphNode_DynamicSpatialFrequency::CalcFrequencyForActor(AActor* Actor, UReplicationGraph* RepGraph, UNetConnection* NetConnection, FGlobalActorReplicationInfo& GlobalInfo, FPerConnectionActorInfoMap& ConnectionMap, FSettings& MySettings, const FNetViewerArray& Viewers, const uint32 FrameNum, int32 ExistingItemIndex)
{
	// An actor that was in the list from the distance-only pass and turns out not to belong
	// here has to come out of it, not merely be skipped.
	auto RemoveExistingItem = [this, ExistingItemIndex]()
	{
		if (ExistingItemIndex != INDEX_NONE)
		{
			SortedReplicationList.RemoveAtSwap(ExistingItemIndex, 1, EAllowShrinking::No);
		}
	};

	auto AddOrUpdateItem = [this, ExistingItemIndex](AActor* InActor, int32 InFramesTillReplicate, bool bInEnableFastPath, FGlobalActorReplicationInfo* InGlobal, FConnectionReplicationActorInfo* InConnection)
	{
		const FDynamicSpatialFrequency_SortedItem Item(InActor, InFramesTillReplicate, bInEnableFastPath, InGlobal, InConnection);

		if (ExistingItemIndex != INDEX_NONE)
		{
			SortedReplicationList[ExistingItemIndex] = Item;
		}
		else
		{
			SortedReplicationList.Add(Item);
		}
	};

	FConnectionReplicationActorInfo& ConnectionInfo = ConnectionMap.FindOrAdd(Actor);

	if (ConnectionInfo.bDormantOnConnection || ConnectionInfo.bTearOff)
	{
		RemoveExistingItem();
		return;
	}

	// ------------------------------------------------------------------
	//	Closest viewer
	// ------------------------------------------------------------------

	double SmallestDistanceSq = MAX_dbl;
	const FNetViewer* ClosestViewer = nullptr;

	for (const FNetViewer& Viewer : Viewers)
	{
		const double DistSq = VectorDistSquared(GlobalInfo.WorldLocation, Viewer.ViewLocation);
		if (ClosestViewer == nullptr || DistSq < SmallestDistanceSq)
		{
			ClosestViewer = &Viewer;
			SmallestDistanceSq = DistSq;
		}
	}

	if (ClosestViewer == nullptr)
	{
		RemoveExistingItem();
		return;
	}

	const float CullDistanceSq = ConnectionInfo.GetCullDistanceSquared();
	if (!IgnoreCullDistance && CullDistanceSq > 0.f && SmallestDistanceSq > static_cast<double>(CullDistanceSq))
	{
		RemoveExistingItem();
		return;
	}

	// ------------------------------------------------------------------
	//	Zone
	// ------------------------------------------------------------------

	const double DistanceToActor = FMath::Sqrt(SmallestDistanceSq);
	const FVector DirToActor = VectorSubtract(GlobalInfo.WorldLocation, ClosestViewer->ViewLocation);
	const FVector NormDirToActor = VectorGetSafeNormal(DirToActor);
	const float DotP = static_cast<float>(FMath::Clamp(VectorDot(NormDirToActor, ClosestViewer->ViewDir), -1.0, 1.0));

	const bool bSupportsFastShared = GlobalInfo.Settings.FastSharedReplicationFunc != nullptr;
	const TArray<FSpatializationZone>& Zones = bSupportsFastShared ? MySettings.ZoneSettings : MySettings.ZoneSettings_NonFastSharedActors;

	// MinDotProduct is the top of a zone's band, and the zones are ordered ascending, so the
	// first one the dot product does not exceed is the one the actor is in.
	const FSpatializationZone* Zone = nullptr;
	for (const FSpatializationZone& TestZone : Zones)
	{
		if (DotP <= TestZone.MinDotProduct)
		{
			Zone = &TestZone;
			break;
		}
	}

	if (Zone == nullptr)
	{
		// No zones configured, or none covering this dot product. Leave the actor's own period
		// alone and let the graph's normal frequency check decide when it goes out.
		AddOrUpdateItem(Actor, 0, false, &GlobalInfo, &ConnectionInfo);
		return;
	}

	// ------------------------------------------------------------------
	//	Period
	// ------------------------------------------------------------------

	// The connection's cull distance is the one that applies, falling back to the class default
	// when a node has zeroed the connection's copy to force relevancy.
	const float EffectiveCullDistanceSq = CullDistanceSq > 0.f ? CullDistanceSq : GlobalInfo.Settings.GetCullDistanceSquared();
	if (EffectiveCullDistanceSq <= 0.f)
	{
		// Nothing to scale against - this actor should not be in a distance based node at all.
		RemoveExistingItem();
		return;
	}

	const float CullDistance = CullDistanceSq > 0.f ? ConnectionInfo.GetCullDistance() : GlobalInfo.Settings.GetCullDistance();
	const float DistPct = static_cast<float>(DistanceToActor) / CullDistance;
	const float ZonePctRange = Zone->MaxDistPct - Zone->MinDistPct;
	const float FinalPct = ZonePctRange > 0.f ? FMath::Clamp((DistPct - Zone->MinDistPct) / ZonePctRange, 0.f, 1.f) : 0.f;

	RepGraphDynamicSpatialFrequency::CalcDynamicReplicationPeriod(FinalPct, Zone->MinRepPeriod, Zone->MaxRepPeriod,
		ConnectionInfo.ReplicationPeriodFrame, ConnectionInfo.NextReplicationFrameNum, ConnectionInfo.LastRepFrameNum);

	int32 FramesTillReplicate = static_cast<int32>(ConnectionInfo.NextReplicationFrameNum) - static_cast<int32>(FrameNum);

	// The channel must outlive the period we just assigned, in case the actor is starved and
	// does not actually get to send on the frame it was due.
	ConnectionInfo.ActorChannelCloseFrameNum = FMath::Max<uint32>(ConnectionInfo.ActorChannelCloseFrameNum, ConnectionInfo.NextReplicationFrameNum + 1);

	bool bEnableFastPath = false;
	if (bSupportsFastShared && Zone->FastPath_MinRepPeriod > 0)
	{
		RepGraphDynamicSpatialFrequency::CalcDynamicReplicationPeriod(FinalPct, Zone->FastPath_MinRepPeriod, Zone->FastPath_MaxRepPeriod,
			ConnectionInfo.FastPath_ReplicationPeriodFrame, ConnectionInfo.FastPath_NextReplicationFrameNum, ConnectionInfo.FastPath_LastRepFrameNum);

		FramesTillReplicate = FMath::Min(FramesTillReplicate, static_cast<int32>(ConnectionInfo.FastPath_NextReplicationFrameNum) - static_cast<int32>(FrameNum));
		bEnableFastPath = true;
	}

	// ------------------------------------------------------------------
	//	Due now, due next frame, or not yet
	// ------------------------------------------------------------------

	// Game code that called ForceNetUpdate is asking for this actor now, and this node is the
	// only thing standing between it and the prioritizer - an actor dropped here never reaches
	// the graph's own ForceNetUpdate check at all.
	if (GlobalInfo.ForceNetUpdateFrame > ConnectionInfo.LastRepFrameNum)
	{
		FramesTillReplicate = FMath::Min(FramesTillReplicate, 0);
	}

	if (FramesTillReplicate <= 0)
	{
		if (FramesTillReplicate == 0 && !RepGraphDynamicSpatialFrequency::ReplicatesEveryFrame(ConnectionInfo, bEnableFastPath))
		{
			++NumExpectedReplicationsThisFrame;
		}

		AddOrUpdateItem(Actor, FramesTillReplicate, bEnableFastPath, &GlobalInfo, &ConnectionInfo);
		return;
	}

	if (FramesTillReplicate == 1 && !RepGraphDynamicSpatialFrequency::ReplicatesEveryFrame(ConnectionInfo, bEnableFastPath))
	{
		++NumExpectedReplicationsNextFrame;
	}

	RemoveExistingItem();
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_ConnectionDormancyNode
// --------------------------------------------------------------------------------------------------------------------------------------------

uint32 UReplicationGraphNode_ConnectionDormancyNode::NumFramesUntilObsolete = 0;

void UReplicationGraphNode_ConnectionDormancyNode::SetNumFramesUntilObsolete(uint32 NumFrames)
{
	NumFramesUntilObsolete = NumFrames;
}

bool UReplicationGraphNode_ConnectionDormancyNode::IsNodeObsolete(uint32 CurrentFrame) const
{
	if (NumFramesUntilObsolete == 0)
	{
		return false;
	}

	return (CurrentFrame > LastGatheredFrame) && ((CurrentFrame - LastGatheredFrame) > NumFramesUntilObsolete);
}

void UReplicationGraphNode_ConnectionDormancyNode::TearDown()
{
	UReplicationGraphNode_ActorList::TearDown();
	RemovedStreamingLevelActorListCollection.TearDown();
}

void UReplicationGraphNode_ConnectionDormancyNode::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	LastGatheredFrame = Params.ReplicationFrameNum;

	ConditionalGatherDormantActorsForConnection(ReplicationActorList, Params, nullptr);

	for (int32 Index = StreamingLevelCollection.StreamingLevelLists.Num() - 1; Index >= 0; --Index)
	{
		FStreamingLevelActorListCollection::FStreamingLevelActors& StreamingList = StreamingLevelCollection.StreamingLevelLists[Index];

		if (StreamingList.ReplicationActorList.Num() <= 0)
		{
			StreamingLevelCollection.StreamingLevelLists.RemoveAtSwap(Index);
			continue;
		}

		if (Params.CheckClientVisibilityForLevel(StreamingList.StreamingLevelName))
		{
			// Where the actors we drop go, so that they come back if the level is hidden and
			// shown again. There may be no such list yet, and that is fine.
			FStreamingLevelActorListCollection::FStreamingLevelActors* RemovedList =
				RemovedStreamingLevelActorListCollection.StreamingLevelLists.FindByKey(StreamingList.StreamingLevelName);

			ConditionalGatherDormantActorsForConnection(StreamingList.ReplicationActorList, Params,
				RemovedList ? &RemovedList->ReplicationActorList : nullptr);
		}
	}
}

void UReplicationGraphNode_ConnectionDormancyNode::ConditionalGatherDormantActorsForConnection(FActorRepListRefView& ConnectionRepList, const FConnectionGatherActorListParameters& Params, FActorRepListRefView* RemovedList)
{
	FPerConnectionActorInfoMap& ConnectionActorInfoMap = Params.ConnectionManager.ActorInfoMap;

	// Walking backwards so a removal does not disturb the elements still to be looked at.
	for (int32 Index = ConnectionRepList.Num() - 1; Index >= 0; --Index)
	{
		FActorRepListType Actor = ConnectionRepList[Index];

		if (FConnectionReplicationActorInfo* ConnectionActorInfo = ConnectionActorInfoMap.Find(Actor))
		{
			if (ConnectionActorInfo->bDormantOnConnection)
			{
				// Fully dormant on this connection: it does not need to be considered again until it wakes.
				ConnectionRepList.RemoveAtSwap(Index);

				if (RemovedList)
				{
					RemovedList->ConditionalAdd(Actor);
				}

				continue;
			}
		}
	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(ConnectionRepList);
}

void UReplicationGraphNode_ConnectionDormancyNode::NotifyActorDormancyFlush(FActorRepListType Actor)
{
	FNewReplicatedActorInfo ActorInfo(Actor);

	// The actor was taken out when it went dormant, so it has to go back in when it wakes.
	if (ActorInfo.StreamingLevelName.IsNone())
	{
		if (!ReplicationActorList.Contains(Actor))
		{
			ReplicationActorList.ConditionalAdd(Actor);
		}
	}
	else
	{
		if (!StreamingLevelCollection.Contains(ActorInfo))
		{
			StreamingLevelCollection.AddActor(ActorInfo);
		}

		RemovedStreamingLevelActorListCollection.RemoveActorFast(ActorInfo, this);
	}
}

void UReplicationGraphNode_ConnectionDormancyNode::OnClientLevelVisibilityAdd(FName LevelName, UWorld* World)
{
	// Actors dropped while the level was hidden come back when it is shown again.
	if (FStreamingLevelActorListCollection::FStreamingLevelActors* RemovedList = RemovedStreamingLevelActorListCollection.StreamingLevelLists.FindByKey(LevelName))
	{
		for (FActorRepListType Actor : RemovedList->ReplicationActorList)
		{
			FNewReplicatedActorInfo ActorInfo(Actor, LevelName);
			if (!StreamingLevelCollection.Contains(ActorInfo))
			{
				StreamingLevelCollection.AddActor(ActorInfo);
			}
		}

		RemovedList->ReplicationActorList.Reset();
	}
}

bool UReplicationGraphNode_ConnectionDormancyNode::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	RemovedStreamingLevelActorListCollection.RemoveActorFast(ActorInfo, this);
	return UReplicationGraphNode_ActorList::NotifyRemoveNetworkActor(ActorInfo, false);
}

bool UReplicationGraphNode_ConnectionDormancyNode::NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	RemovedStreamingLevelActorListCollection.RemoveActorFromLevelFast(ActorInfo.OldActorInfo.Actor, ActorInfo.OldActorInfo.StreamingLevelName);
	return UReplicationGraphNode_ActorList::NotifyActorRenamed(ActorInfo, false);
}

void UReplicationGraphNode_ConnectionDormancyNode::NotifyResetAllNetworkActors()
{
	UReplicationGraphNode_ActorList::NotifyResetAllNetworkActors();
	RemovedStreamingLevelActorListCollection.Reset();
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_DormancyNode
// --------------------------------------------------------------------------------------------------------------------------------------------

double UReplicationGraphNode_DormancyNode::MaxZForConnection = RepGraphConsoleVariables::DormancyNode_MaxZForConnection;

void UReplicationGraphNode_DormancyNode::NotifyResetAllNetworkActors()
{
	if (GraphGlobals.IsValid())
	{
		// Stop listening for dormancy flushes on everything we were holding. Only our own
		// binding comes off: the same event carries the graph's streaming-level listener.
		TArray<FActorRepListType> AllActors;
		ReplicationActorList.AppendToTArray(AllActors);
		StreamingLevelCollection.GetAll_Debug(AllActors);

		for (FActorRepListType Actor : AllActors)
		{
			if (FGlobalActorReplicationInfo* GlobalInfo = GraphGlobals->GlobalActorReplicationInfoMap->Find(Actor))
			{
				GlobalInfo->Events.DormancyFlush.RemoveAll(this);
			}
		}
	}

	UReplicationGraphNode_ActorList::NotifyResetAllNetworkActors();
}

void UReplicationGraphNode_DormancyNode::AddDormantActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
	UReplicationGraphNode_ActorList::NotifyAddNetworkActor(ActorInfo);

	// The graph has to hear about the actor waking up so it can be put back in every connection's copy.
	GlobalInfo.Events.DormancyFlush.AddRaw(this, &UReplicationGraphNode_DormancyNode::OnActorDormancyFlush);

	// Every connection node already made for this cell needs the actor too.
	CallFunctionOnValidConnectionNodes([&ActorInfo](UReplicationGraphNode_ConnectionDormancyNode* Node)
	{
		Node->NotifyAddNetworkActor(ActorInfo);
	});
}

void UReplicationGraphNode_DormancyNode::RemoveDormantActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo)
{
	UReplicationGraphNode_ActorList::NotifyRemoveNetworkActor(ActorInfo, false);

	ActorRepInfo.Events.DormancyFlush.RemoveAll(this);

	CallFunctionOnValidConnectionNodes([&ActorInfo](UReplicationGraphNode_ConnectionDormancyNode* Node)
	{
		Node->NotifyRemoveNetworkActor(ActorInfo, false);
	});
}

void UReplicationGraphNode_DormancyNode::RenameDormantActor(const FRenamedReplicatedActorInfo& ActorInfo)
{
	UReplicationGraphNode_ActorList::NotifyActorRenamed(ActorInfo, false);

	CallFunctionOnValidConnectionNodes([&ActorInfo](UReplicationGraphNode_ConnectionDormancyNode* Node)
	{
		Node->NotifyActorRenamed(ActorInfo, false);
	});
}

void UReplicationGraphNode_DormancyNode::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	// A connection looking down from above the world (spectating, in the bus) does not need
	// per-connection dormancy nodes built for every cell it flies over.
	for (const FNetViewer& Viewer : Params.Viewers)
	{
		if (Viewer.ViewLocation.Z > MaxZForConnection)
		{
			return;
		}
	}

	if (UReplicationGraphNode_ConnectionDormancyNode* ConnectionNode = GetConnectionNode(Params))
	{
		ConnectionNode->GatherActorListsForConnection(Params);
	}
}

UReplicationGraphNode_ConnectionDormancyNode* UReplicationGraphNode_DormancyNode::GetExistingConnectionNode(const FConnectionGatherActorListParameters& Params)
{
	const FRepGraphConnectionKey RepGraphConnection(&Params.ConnectionManager);

	if (UReplicationGraphNode_ConnectionDormancyNode** NodePtr = ConnectionNodes.Find(RepGraphConnection))
	{
		return *NodePtr;
	}

	return nullptr;
}

UReplicationGraphNode_ConnectionDormancyNode* UReplicationGraphNode_DormancyNode::GetConnectionNode(const FConnectionGatherActorListParameters& Params)
{
	if (UReplicationGraphNode_ConnectionDormancyNode* ExistingNode = GetExistingConnectionNode(Params))
	{
		return ExistingNode;
	}

	return CreateConnectionNode(Params);
}

UReplicationGraphNode_ConnectionDormancyNode* UReplicationGraphNode_DormancyNode::CreateConnectionNode(const FConnectionGatherActorListParameters& Params)
{
	const FRepGraphConnectionKey RepGraphConnection(&Params.ConnectionManager);

	UReplicationGraphNode_ConnectionDormancyNode* NewNode = CreateChildNode<UReplicationGraphNode_ConnectionDormancyNode>();
	NewNode->InitConnectionNode(RepGraphConnection, Params.ReplicationFrameNum);

	ConnectionNodes.Add(RepGraphConnection, NewNode);

	// The new node starts as a copy of what this cell currently holds.
	NewNode->DeepCopyActorListsFrom(this);

	// And it wants to hear about levels coming into view so it can restore what it dropped.
	Params.ConnectionManager.OnClientVisibleLevelNameAdd.AddRaw(NewNode, &UReplicationGraphNode_ConnectionDormancyNode::OnClientLevelVisibilityAdd);

	return NewNode;
}

void UReplicationGraphNode_DormancyNode::CallFunctionOnValidConnectionNodes(FConnectionDormancyNodeFunction Function)
{
	for (auto It = ConnectionNodes.CreateIterator(); It; ++It)
	{
		UReplicationGraphNode_ConnectionDormancyNode* Node = It.Value();
		if (Node == nullptr || It.Key().ResolveObjectPtr() == nullptr)
		{
			It.RemoveCurrent();
			continue;
		}

		Function(Node);
	}
}

void UReplicationGraphNode_DormancyNode::OnActorDormancyFlush(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo)
{
	CallFunctionOnValidConnectionNodes([Actor](UReplicationGraphNode_ConnectionDormancyNode* Node)
	{
		Node->NotifyActorDormancyFlush(Actor);
	});
}

void UReplicationGraphNode_DormancyNode::ConditionalGatherDormantDynamicActors(FActorRepListRefView& RepList, const FConnectionGatherActorListParameters& Params, FActorRepListRefView* RemovedList, bool bEnforceReplistUniqueness, FActorRepListRefView* RemoveFromList)
{
	FPerConnectionActorInfoMap& ConnectionActorInfoMap = Params.ConnectionManager.ActorInfoMap;

	TArray<FActorRepListType> AllDormantActors;
	ReplicationActorList.AppendToTArray(AllDormantActors);

	for (FActorRepListType Actor : AllDormantActors)
	{
		if (!IsActorValidForReplication(Actor))
		{
			continue;
		}

		// Only dynamic actors are worth telling the client to destroy - a level actor is already there.
		if (Actor->IsNetStartupActor())
		{
			continue;
		}

		FConnectionReplicationActorInfo& ConnectionInfo = ConnectionActorInfoMap.FindOrAdd(Actor);
		if (!ConnectionInfo.bDormantOnConnection)
		{
			continue;
		}

		if (bEnforceReplistUniqueness && RepList.Contains(Actor))
		{
			continue;
		}

		RepList.Add(Actor);

		if (RemoveFromList)
		{
			RemoveFromList->RemoveFast(Actor);
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_GridCell
// --------------------------------------------------------------------------------------------------------------------------------------------

UReplicationGraphNode* UReplicationGraphNode_GridCell::GetDynamicNode()
{
	if (DynamicNode == nullptr)
	{
		if (CreateDynamicNodeOverride)
		{
			DynamicNode = CreateDynamicNodeOverride(this);
		}
		else
		{
			DynamicNode = CreateChildNode<UReplicationGraphNode_ActorListFrequencyBuckets>();
		}
	}

	return DynamicNode;
}

UReplicationGraphNode_DormancyNode* UReplicationGraphNode_GridCell::GetDormancyNode(bool bInCreateIfMissing)
{
	if (DormancyNode == nullptr && bInCreateIfMissing)
	{
		DormancyNode = CreateChildNode<UReplicationGraphNode_DormancyNode>();
	}

	return DormancyNode;
}

void UReplicationGraphNode_GridCell::AddStaticActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalRepInfo, bool bParentNodeHandlesDormancyChange)
{
	if (GlobalRepInfo.bWantsToBeDormant)
	{
		GetDormancyNode()->AddDormantActor(ActorInfo, GlobalRepInfo);
	}
	else
	{
		UReplicationGraphNode_ActorList::NotifyAddNetworkActor(ActorInfo);
	}

	// The cell only listens for the change itself when the grid is not doing it for us.
	if (!bParentNodeHandlesDormancyChange)
	{
		GlobalRepInfo.Events.DormancyChange.AddRaw(this, &UReplicationGraphNode_GridCell::OnStaticActorNetDormancyChange);
	}
}

void UReplicationGraphNode_GridCell::AddDynamicActor(const FNewReplicatedActorInfo& ActorInfo)
{
	GetDynamicNode()->NotifyAddNetworkActor(ActorInfo);
}

void UReplicationGraphNode_GridCell::RemoveStaticActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool bWasAddedAsDormantActor)
{
	if (bWasAddedAsDormantActor)
	{
		GetDormancyNode()->RemoveDormantActor(ActorInfo, ActorRepInfo);
	}
	else
	{
		UReplicationGraphNode_ActorList::NotifyRemoveNetworkActor(ActorInfo, false);
	}

	ActorRepInfo.Events.DormancyChange.RemoveAll(this);
}

void UReplicationGraphNode_GridCell::RemoveDynamicActor(const FNewReplicatedActorInfo& ActorInfo)
{
	GetDynamicNode()->NotifyRemoveNetworkActor(ActorInfo, false);
}

void UReplicationGraphNode_GridCell::ConditionalCopyDormantActors(FActorRepListRefView& FromList, UReplicationGraphNode_DormancyNode* ToNode)
{
	if (!GraphGlobals.IsValid())
	{
		return;
	}

	for (int32 Index = FromList.Num() - 1; Index >= 0; --Index)
	{
		FActorRepListType Actor = FromList[Index];
		if (FGlobalActorReplicationInfo* GlobalInfo = GraphGlobals->GlobalActorReplicationInfoMap->Find(Actor))
		{
			if (GlobalInfo->bWantsToBeDormant)
			{
				ToNode->AddDormantActor(FNewReplicatedActorInfo(Actor), *GlobalInfo);
				FromList.RemoveAtSwap(Index);
			}
		}
	}
}

void UReplicationGraphNode_GridCell::OnStaticActorNetDormancyChange(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo, ENetDormancy NewValue, ENetDormancy OldValue)
{
	const bool bCurrentlyDormant = NewValue > ENetDormancy::DORM_Awake;
	const bool bPreviouslyDormant = OldValue > ENetDormancy::DORM_Awake;

	if (bCurrentlyDormant == bPreviouslyDormant)
	{
		return;
	}

	FNewReplicatedActorInfo ActorInfo(Actor);

	if (bCurrentlyDormant)
	{
		// Went dormant: move it out of the always-considered list and into the dormancy node.
		UReplicationGraphNode_ActorList::NotifyRemoveNetworkActor(ActorInfo, false);
		GetDormancyNode()->AddDormantActor(ActorInfo, GlobalInfo);
	}
	else
	{
		// Woke up: move it back.
		if (UReplicationGraphNode_DormancyNode* Node = GetDormancyNode(false))
		{
			Node->RemoveDormantActor(ActorInfo, GlobalInfo);
		}

		UReplicationGraphNode_ActorList::NotifyAddNetworkActor(ActorInfo);
	}
}

void UReplicationGraphNode_GridCell::OnActorDormancyFlush(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo, UReplicationGraphNode_DormancyNode* InDormancyNode)
{
	if (InDormancyNode)
	{
		InDormancyNode->OnActorDormancyFlush(Actor, GlobalInfo);
	}
}

void UReplicationGraphNode_GridCell::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	UReplicationGraphNode_ActorList::LogNode(DebugInfo, InNodeName);
}

void UReplicationGraphNode_GridCell::GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const
{
	UReplicationGraphNode_ActorList::GetAllActorsInNode_Debugging(OutArray);
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_GridSpatialization2D
// --------------------------------------------------------------------------------------------------------------------------------------------

UReplicationGraphNode_GridSpatialization2D::UReplicationGraphNode_GridSpatialization2D()
{
	bRequiresPrepareForReplicationCall = true;
}

void UReplicationGraphNode_GridSpatialization2D::NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor)
{
	UE_LOG(LogReplicationGraph, Warning,
		"UReplicationGraphNode_GridSpatialization2D::NotifyAddNetworkActor should not be called directly. Use AddActor_Static/Dynamic/Dormancy.");
}

bool UReplicationGraphNode_GridSpatialization2D::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	UE_LOG(LogReplicationGraph, Warning,
		"UReplicationGraphNode_GridSpatialization2D::NotifyRemoveNetworkActor should not be called directly. Use RemoveActor_Static/Dynamic/Dormancy.");
	return false;
}

bool UReplicationGraphNode_GridSpatialization2D::NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound)
{
	return false;
}

void UReplicationGraphNode_GridSpatialization2D::NotifyResetAllNetworkActors()
{
	StaticSpatializedActors.Reset();
	DynamicSpatializedActors.Reset();
	PendingStaticSpatializedActors.Reset();

	UReplicationGraphNode::NotifyResetAllNetworkActors();
}

void UReplicationGraphNode_GridSpatialization2D::AddActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo)
{
	if (ActorInfo.Actor == nullptr)
	{
		return;
	}

	DynamicSpatializedActors.Emplace(ActorInfo.Actor, FCachedDynamicActorInfo(ActorInfo));
}

void UReplicationGraphNode_GridSpatialization2D::AddActorInternal_Static(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool IsDormancyDriven)
{
	AActor* Actor = ActorInfo.Actor;
	if (Actor == nullptr)
	{
		return;
	}

	// An actor that has not finished initializing has no meaningful location yet, so its
	// placement waits for the next frame rather than landing in the wrong cell.
	const FVector Location3D = Actor->GetActorLocation();
	if (VectorContainsNaN(Location3D))
	{
		PendingStaticSpatializedActors.Emplace(Actor, IsDormancyDriven);
		return;
	}

	AddActorInternal_Static_Implementation(ActorInfo, ActorRepInfo, IsDormancyDriven);
}

void UReplicationGraphNode_GridSpatialization2D::AddActorInternal_Static_Implementation(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool IsDormancyDriven)
{
	AActor* Actor = ActorInfo.Actor;

	ActorRepInfo.WorldLocation = Actor->GetActorLocation();

	StaticSpatializedActors.Emplace(Actor, FCachedStaticActorInfo(ActorInfo, IsDormancyDriven));

	PutStaticActorIntoCell(ActorInfo, ActorRepInfo, IsDormancyDriven);
}

void UReplicationGraphNode_GridSpatialization2D::AddActor_Dormancy(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo)
{
	if (ActorRepInfo.bWantsToBeDormant)
	{
		AddActorInternal_Static(ActorInfo, ActorRepInfo, true);
	}
	else
	{
		AddActorInternal_Dynamic(ActorInfo);
	}

	// This node owns the transition between the two treatments for dormancy-driven actors.
	ActorRepInfo.Events.DormancyChange.AddRaw(this, &UReplicationGraphNode_GridSpatialization2D::OnNetDormancyChange);
}

void UReplicationGraphNode_GridSpatialization2D::RemoveActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo)
{
	if (FCachedDynamicActorInfo* DynamicActorInfo = DynamicSpatializedActors.Find(ActorInfo.Actor))
	{
		if (DynamicActorInfo->CellInfo.IsValid())
		{
			GetGridNodesForActor(ActorInfo.Actor, DynamicActorInfo->CellInfo, GatheredNodes);
			for (UReplicationGraphNode_GridCell* Node : GatheredNodes)
			{
				Node->RemoveDynamicActor(ActorInfo);
			}
		}

		DynamicSpatializedActors.Remove(ActorInfo.Actor);
	}
}

void UReplicationGraphNode_GridSpatialization2D::RemoveActorInternal_Static(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool WasAddedAsDormantActor)
{
	if (StaticSpatializedActors.Remove(ActorInfo.Actor) == 0)
	{
		// It may still be waiting to be placed.
		PendingStaticSpatializedActors.RemoveAll([&ActorInfo](const FPendingStaticActors& Pending) { return Pending.Actor == ActorInfo.Actor; });
		return;
	}

	GetGridNodesForActor(ActorInfo.Actor, ActorRepInfo, GatheredNodes);
	for (UReplicationGraphNode_GridCell* Node : GatheredNodes)
	{
		Node->RemoveStaticActor(ActorInfo, ActorRepInfo, WasAddedAsDormantActor);
	}
}

void UReplicationGraphNode_GridSpatialization2D::RemoveActor_Static(const FNewReplicatedActorInfo& ActorInfo)
{
	if (!GraphGlobals.IsValid())
	{
		return;
	}

	FGlobalActorReplicationInfo& ActorRepInfo = GraphGlobals->GlobalActorReplicationInfoMap->Get(ActorInfo.Actor);
	RemoveActorInternal_Static(ActorInfo, ActorRepInfo, ActorRepInfo.bWantsToBeDormant);
}

void UReplicationGraphNode_GridSpatialization2D::RemoveActor_Dormancy(const FNewReplicatedActorInfo& ActorInfo)
{
	if (!GraphGlobals.IsValid())
	{
		return;
	}

	FGlobalActorReplicationInfo& ActorRepInfo = GraphGlobals->GlobalActorReplicationInfoMap->Get(ActorInfo.Actor);

	if (ActorRepInfo.bWantsToBeDormant)
	{
		RemoveActorInternal_Static(ActorInfo, ActorRepInfo, true);
	}
	else
	{
		RemoveActorInternal_Dynamic(ActorInfo);
	}

	ActorRepInfo.Events.DormancyChange.RemoveAll(this);
}

void UReplicationGraphNode_GridSpatialization2D::OnNetDormancyChange(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo, ENetDormancy NewValue, ENetDormancy OldValue)
{
	const bool bCurrentlyDormant = NewValue > ENetDormancy::DORM_Awake;
	const bool bPreviouslyDormant = OldValue > ENetDormancy::DORM_Awake;

	if (bCurrentlyDormant == bPreviouslyDormant)
	{
		return;
	}

	FNewReplicatedActorInfo ActorInfo(Actor);

	if (bCurrentlyDormant)
	{
		// While dormant it is treated as static: it will not be moving.
		RemoveActorInternal_Dynamic(ActorInfo);
		AddActorInternal_Static(ActorInfo, GlobalInfo, true);
	}
	else
	{
		// Awake again, so it goes back to being tracked as something that moves.
		RemoveActorInternal_Static(ActorInfo, GlobalInfo, true);
		AddActorInternal_Dynamic(ActorInfo);
	}
}

bool UReplicationGraphNode_GridSpatialization2D::WillActorLocationGrowSpatialBounds(const FVector& Location) const
{
	return Location.X < SpatialBias.X || Location.Y < SpatialBias.Y;
}

void UReplicationGraphNode_GridSpatialization2D::HandleActorOutOfSpatialBounds(AActor* Actor, const FVector& Location3D, const bool bStaticActor)
{
	// Some classes must never force a rebuild: a projectile leaving the map would
	// otherwise rebuild the entire grid. Those are clamped into the grid instead.
	if (const bool* bDenied = ClassRebuildDenyList.Get(Actor->Class))
	{
		if (*bDenied)
		{
			return;
		}
	}

	SpatialBias.X = FMath::Min(SpatialBias.X, Location3D.X - static_cast<double>(CellSize));
	SpatialBias.Y = FMath::Min(SpatialBias.Y, Location3D.Y - static_cast<double>(CellSize));

	bNeedsRebuild = true;

	UE_LOG(LogReplicationGraph, Log, "Spatialization bias changed to (%.2f, %.2f) because of %s. Rebuilding grid.",
		SpatialBias.X, SpatialBias.Y, GetActorRepListTypeDebugString(Actor).c_str());
}

UReplicationGraphNode_GridSpatialization2D::FActorCellInfo UReplicationGraphNode_GridSpatialization2D::GetCellInfoForActor(FActorRepListType Actor, const FVector& Location3D, float CullDistance)
{
	FActorCellInfo CellInfo;

	if (CellSize <= 0.f)
	{
		return CellInfo;
	}

	const double MinX = Location3D.X - static_cast<double>(CullDistance) - SpatialBias.X;
	const double MinY = Location3D.Y - static_cast<double>(CullDistance) - SpatialBias.Y;
	const double MaxX = Location3D.X + static_cast<double>(CullDistance) - SpatialBias.X;
	const double MaxY = Location3D.Y + static_cast<double>(CullDistance) - SpatialBias.Y;

	CellInfo.StartX = FMath::Max(0, FMath::FloorToInt(MinX / static_cast<double>(CellSize)));
	CellInfo.StartY = FMath::Max(0, FMath::FloorToInt(MinY / static_cast<double>(CellSize)));
	CellInfo.EndX = FMath::Max(0, FMath::FloorToInt(MaxX / static_cast<double>(CellSize)));
	CellInfo.EndY = FMath::Max(0, FMath::FloorToInt(MaxY / static_cast<double>(CellSize)));

	return CellInfo;
}

void UReplicationGraphNode_GridSpatialization2D::GetGridNodesForActor(FActorRepListType Actor, const FGlobalActorReplicationInfo& ActorRepInfo, TArray<UReplicationGraphNode_GridCell*>& OutNodes)
{
	GetGridNodesForActor(Actor, GetCellInfoForActor(Actor, ActorRepInfo.WorldLocation, ActorRepInfo.Settings.GetCullDistance()), OutNodes);
}

void UReplicationGraphNode_GridSpatialization2D::GetGridNodesForActor(FActorRepListType Actor, const FActorCellInfo& CellInfo, TArray<UReplicationGraphNode_GridCell*>& OutNodes)
{
	OutNodes.Reset();

	if (!CellInfo.IsValid())
	{
		return;
	}

	for (int32 X = CellInfo.StartX; X <= CellInfo.EndX; ++X)
	{
		TArray<UReplicationGraphNode_GridCell*>& GridX = GetGridX(X);
		for (int32 Y = CellInfo.StartY; Y <= CellInfo.EndY; ++Y)
		{
			OutNodes.Add(GetCellNode(GetCell(GridX, Y)));
		}
	}
}

void UReplicationGraphNode_GridSpatialization2D::PutStaticActorIntoCell(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool bDormancyDriven)
{
	GetGridNodesForActor(ActorInfo.Actor, ActorRepInfo, GatheredNodes);
	for (UReplicationGraphNode_GridCell* Node : GatheredNodes)
	{
		Node->AddStaticActor(ActorInfo, ActorRepInfo, bDormancyDriven);
	}
}

void UReplicationGraphNode_GridSpatialization2D::NotifyActorCullDistChange(AActor* Actor, FGlobalActorReplicationInfo& GlobalInfo, float OldDist)
{
	// A static actor's cell membership is a function of its cull distance, so it has to be re-placed.
	if (FCachedStaticActorInfo* StaticActorInfo = StaticSpatializedActors.Find(Actor))
	{
		const FNewReplicatedActorInfo ActorInfo = StaticActorInfo->ActorInfo;
		const bool bDormancyDriven = StaticActorInfo->bDormancyDriven;

		// Remove using the old distance, then add back with the new one.
		const float NewDist = GlobalInfo.Settings.GetCullDistance();
		GlobalInfo.Settings.SetCullDistanceSquared(OldDist * OldDist);
		RemoveActorInternal_Static(ActorInfo, GlobalInfo, bDormancyDriven);
		GlobalInfo.Settings.SetCullDistanceSquared(NewDist * NewDist);

		AddActorInternal_Static_Implementation(ActorInfo, GlobalInfo, bDormancyDriven);
		return;
	}

	// Dynamic actors are re-cellled by PrepareForReplication anyway; just invalidate.
	if (FCachedDynamicActorInfo* DynamicActorInfo = DynamicSpatializedActors.Find(Actor))
	{
		DynamicActorInfo->CellInfo.Reset();
	}
}

void UReplicationGraphNode_GridSpatialization2D::PrepareForReplication()
{
	if (!GraphGlobals.IsValid())
	{
		return;
	}

	FGlobalActorReplicationInfoMap* GlobalMap = GraphGlobals->GlobalActorReplicationInfoMap;

	// Actors that could not be placed when they registered get another chance now.
	if (PendingStaticSpatializedActors.Num() > 0)
	{
		TArray<FPendingStaticActors> Pending = PendingStaticSpatializedActors;
		PendingStaticSpatializedActors.Reset();

		for (const FPendingStaticActors& PendingActor : Pending)
		{
			if (!IsActorValidForReplication(PendingActor.Actor))
			{
				continue;
			}

			FGlobalActorReplicationInfo& ActorRepInfo = GlobalMap->Get(PendingActor.Actor);
			AddActorInternal_Static_Implementation(FNewReplicatedActorInfo(PendingActor.Actor), ActorRepInfo, PendingActor.DormancyDriven);
		}
	}

	if (bNeedsRebuild)
	{
		// Everything has to come out of the grid and go back in against the new bias.
		UE_LOG(LogReplicationGraph, Log, "Rebuilding spatialization grid.");

		for (TArray<UReplicationGraphNode_GridCell*>& GridX : Grid)
		{
			for (UReplicationGraphNode_GridCell* Cell : GridX)
			{
				if (Cell)
				{
					Cell->NotifyResetAllNetworkActors();
				}
			}
		}

		for (auto It = StaticSpatializedActors.CreateIterator(); It; ++It)
		{
			FCachedStaticActorInfo& StaticActorInfo = It.Value();
			if (!IsActorValidForReplication(StaticActorInfo.ActorInfo.Actor))
			{
				continue;
			}

			FGlobalActorReplicationInfo& ActorRepInfo = GlobalMap->Get(StaticActorInfo.ActorInfo.Actor);
			PutStaticActorIntoCell(StaticActorInfo.ActorInfo, ActorRepInfo, StaticActorInfo.bDormancyDriven);
		}

		for (auto It = DynamicSpatializedActors.CreateIterator(); It; ++It)
		{
			It.Value().CellInfo.Reset();
		}

		bNeedsRebuild = false;
	}

	/*
	 * Diagnostic: how many dynamic actors the grid is tracking, and where it is putting the
	 * pawns among them. If a pawn never appears on one of these lines it never reached the
	 * grid at all, and the problem is routing, not relevancy - look at
	 * UFortReplicationGraph::RouteAddNetworkActorToNodes and the class policy for its class.
	 */
	const bool bDiag = IsRelevancyDiagFrame(GraphGlobals->ReplicationGraph ? GraphGlobals->ReplicationGraph->GetReplicationGraphFrame() : 0);

	if (bDiag)
	{
		UE_LOG(LogReplicationGraph, Display, "RepDiag grid: tracking %d dynamic and %d static spatialized actors.",
			DynamicSpatializedActors.Num(), StaticSpatializedActors.Num());
	}

	// Dynamic actors move, so every frame their world location is refreshed and their
	// cell membership updated if it changed.
	for (auto It = DynamicSpatializedActors.CreateIterator(); It; ++It)
	{
		FActorRepListType Actor = It.Key();
		FCachedDynamicActorInfo& DynamicActorInfo = It.Value();

		if (!IsActorValidForReplication(Actor))
		{
			continue;
		}

		FGlobalActorReplicationInfo& ActorRepInfo = GlobalMap->Get(Actor);

		const FVector Location3D = Actor->GetActorLocation();
		if (VectorContainsNaN(Location3D))
		{
			ActorRepInfo.bWasWorldLocClamped = true;
			continue;
		}

		ActorRepInfo.WorldLocation = Location3D;

		if (WillActorLocationGrowSpatialBounds(Location3D))
		{
			HandleActorOutOfSpatialBounds(Actor, Location3D, false);
		}

		const FActorCellInfo NewCellInfo = GetCellInfoForActor(Actor, Location3D, ActorRepInfo.Settings.GetCullDistance());
		const FActorCellInfo& OldCellInfo = DynamicActorInfo.CellInfo;

		if (bDiag && IsPawnishClass(Actor))
		{
			/*
			 * AcknowledgedPawn is reported alongside the position because it is the one flag that
			 * silently freezes a server-side pawn.
			 *
			 * APlayerController::NotifyServerReceivedClientData returns false whenever
			 * AcknowledgedPawn is not the pawn the move arrived for, and
			 * UCharacterMovementComponent::ServerMove_Implementation responds by zeroing the
			 * acceleration before running the move. The server then simulates a player who is
			 * standing still, no matter what the client sends - and because nothing calls
			 * APlayerController::SendClientAdjustment here, the client is never corrected back,
			 * so it walks away happily while everyone else watches it stand still.
			 *
			 * ack=yes with a frozen position rules that out and points at the move RPCs
			 * themselves; ack=no names it outright.
			 */
			const char* AckState = "n/a";
			if (SDK::APawn* SdkPawn = Cast<SDK::APawn>(static_cast<SDK::AActor*>(Actor)))
			{
				if (SDK::APlayerController* PC = Cast<SDK::APlayerController>(SdkPawn->Controller))
				{
					AckState = (static_cast<SDK::AActor*>(PC->AcknowledgedPawn) == static_cast<SDK::AActor*>(SdkPawn)) ? "yes" : "NO";
				}
				else
				{
					AckState = "no-controller";
				}
			}

			UE_LOG(LogReplicationGraph, Display,
				"RepDiag grid: pawn %s at (%.1f, %.1f, %.1f) cullDist=%.0f -> cells X[%d..%d] Y[%d..%d] ack=%s",
				Actor->GetName().c_str(), Location3D.X, Location3D.Y, Location3D.Z,
				ActorRepInfo.Settings.GetCullDistance(),
				NewCellInfo.StartX, NewCellInfo.EndX, NewCellInfo.StartY, NewCellInfo.EndY,
				AckState);
		}

		if (OldCellInfo.IsValid()
			&& OldCellInfo.StartX == NewCellInfo.StartX && OldCellInfo.StartY == NewCellInfo.StartY
			&& OldCellInfo.EndX == NewCellInfo.EndX && OldCellInfo.EndY == NewCellInfo.EndY)
		{
			// Same cells as last frame: nothing to do.
			continue;
		}

		if (OldCellInfo.IsValid())
		{
			GetGridNodesForActor(Actor, OldCellInfo, GatheredNodes);
			for (UReplicationGraphNode_GridCell* Node : GatheredNodes)
			{
				Node->RemoveDynamicActor(DynamicActorInfo.ActorInfo);
			}
		}

		GetGridNodesForActor(Actor, NewCellInfo, GatheredNodes);
		for (UReplicationGraphNode_GridCell* Node : GatheredNodes)
		{
			Node->AddDynamicActor(DynamicActorInfo.ActorInfo);
		}

		DynamicActorInfo.CellInfo = NewCellInfo;
	}
}

void UReplicationGraphNode_GridSpatialization2D::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	if (CellSize <= 0.f)
	{
		return;
	}

	const bool bDiag = IsRelevancyDiagFrame(Params.ReplicationFrameNum);

	for (const FNetViewer& Viewer : Params.Viewers)
	{
		if (Viewer.ViewLocation.Z > ConnectionMaxZ)
		{
			if (bDiag)
			{
				UE_LOG(LogReplicationGraph, Display,
					"RepDiag grid conn=%d: viewer above ConnectionMaxZ (%.0f > %.0f), gathering nothing.",
					Params.ConnectionManager.ConnectionOrderNum, Viewer.ViewLocation.Z, ConnectionMaxZ);
			}
			continue;
		}

		// A connection only pulls from the single cell it is standing in. Actors were placed
		// into every cell their cull distance reaches, which is what makes that correct.
		const double LocationBiasX = (Viewer.ViewLocation.X - SpatialBias.X) / static_cast<double>(CellSize);
		const double LocationBiasY = (Viewer.ViewLocation.Y - SpatialBias.Y) / static_cast<double>(CellSize);

		const int32 CellX = FMath::Max(0, FMath::FloorToInt(LocationBiasX));
		const int32 CellY = FMath::Max(0, FMath::FloorToInt(LocationBiasY));

		UReplicationGraphNode_GridCell* CellNode = nullptr;
		if (Grid.Num() > CellX && Grid[CellX].Num() > CellY)
		{
			CellNode = Grid[CellX][CellY];
		}

		if (bDiag)
		{
			int32 NumInCell = -1;
			if (CellNode)
			{
				TArray<FActorRepListType> CellActors;
				CellNode->GetAllActorsInNode_Debugging(CellActors);
				NumInCell = CellActors.Num();
			}

			UE_LOG(LogReplicationGraph, Display,
				"RepDiag grid conn=%d viewTarget=%s view=(%.0f, %.0f, %.0f) bias=(%.0f, %.0f) cellSize=%.0f -> cell=(%d, %d) gridDim=(%d, %d) cellNode=%s actorsInCell=%d",
				Params.ConnectionManager.ConnectionOrderNum,
				Viewer.ViewTarget ? Viewer.ViewTarget->GetName().c_str() : "None",
				Viewer.ViewLocation.X, Viewer.ViewLocation.Y, Viewer.ViewLocation.Z,
				SpatialBias.X, SpatialBias.Y, CellSize,
				CellX, CellY,
				Grid.Num(), Grid.Num() > CellX ? Grid[CellX].Num() : 0,
				CellNode ? "yes" : "NULL",
				NumInCell);
		}

		if (CellNode)
		{
			CellNode->GatherActorListsForConnection(Params);
		}
	}
}

void UReplicationGraphNode_GridSpatialization2D::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();

	for (int32 X = 0; X < Grid.Num(); ++X)
	{
		for (int32 Y = 0; Y < Grid[X].Num(); ++Y)
		{
			if (const UReplicationGraphNode_GridCell* Cell = Grid[X][Y])
			{
				Cell->LogNode(DebugInfo, "Cell [" + std::to_string(X) + ", " + std::to_string(Y) + "]");
			}
		}
	}

	DebugInfo.PopIndent();
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_AlwaysRelevant
// --------------------------------------------------------------------------------------------------------------------------------------------

UReplicationGraphNode_AlwaysRelevant::UReplicationGraphNode_AlwaysRelevant()
{
	bRequiresPrepareForReplicationCall = true;
}

void UReplicationGraphNode_AlwaysRelevant::AddAlwaysRelevantClass(UClass* Class)
{
	// Only add the class if neither it nor one of its parents is already covered.
	for (UClass* ExistingClass : AlwaysRelevantClasses)
	{
		if (Class->IsSubclassOf(ExistingClass))
		{
			return;
		}
	}

	AlwaysRelevantClasses.AddUnique(Class);
}

void UReplicationGraphNode_AlwaysRelevant::PrepareForReplication()
{
	if (ChildNode == nullptr)
	{
		ChildNode = CreateChildNode<UReplicationGraphNode_ActorList>();
	}

	ChildNode->NotifyResetAllNetworkActors();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// The list is rebuilt each frame by walking the world. These classes are rare, so this
	// costs less than maintaining change notifications for them - but it is a walk of the
	// levels rather than of the object table, which in this game has millions of entries.
	for (ULevel* Level : World->Levels)
	{
		if (!Level)
		{
			continue;
		}

		for (SDK::AActor* LevelActor : Level->Actors)
		{
			AActor* Actor = reinterpret_cast<AActor*>(LevelActor);
			if (!IsActorValidForReplicationGather(Actor))
			{
				continue;
			}

			for (UClass* ActorClass : AlwaysRelevantClasses)
			{
				if (Actor->IsA(ActorClass))
				{
					ChildNode->NotifyAddNetworkActor(FNewReplicatedActorInfo(Actor));
					break;
				}
			}
		}
	}
}

void UReplicationGraphNode_AlwaysRelevant::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	if (ChildNode)
	{
		ChildNode->GatherActorListsForConnection(Params);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_AlwaysRelevant_ForConnection
// --------------------------------------------------------------------------------------------------------------------------------------------

void UReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	ReplicationActorList.Reset();

	for (const FNetViewer& CurViewer : Params.Viewers)
	{
		FCachedAlwaysRelevantActorInfo& LastData = PastRelevantActorMap.FindOrAdd(TObjectKey<UNetConnection>(CurViewer.Connection));

		AddCachedRelevantActor(Params, CurViewer.InViewer, LastData.LastViewer);

		// FNetViewer falls back to the viewer when there is no view target, and a controller
		// with no pawn hits that. Adding both would replicate it twice a frame.
		if (CurViewer.ViewTarget != CurViewer.InViewer)
		{
			AddCachedRelevantActor(Params, CurViewer.ViewTarget, LastData.LastViewTarget);
		}
	}

	CleanupCachedRelevantActors(PastRelevantActorMap);

	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);
}

void UReplicationGraphNode_AlwaysRelevant_ForConnection::TearDown()
{
	UReplicationGraphNode_ActorList::TearDown();
	ReplicationActorList.TearDown();
	PastRelevantActorMap.Reset();
}

void UReplicationGraphNode_AlwaysRelevant_ForConnection::AddCachedRelevantActor(const FConnectionGatherActorListParameters& Params, AActor* NewActor, AActor*& LastActor)
{
	UpdateCachedRelevantActor(Params, NewActor, LastActor);

	if (NewActor)
	{
		ReplicationActorList.ConditionalAdd(NewActor);
	}
}

void UReplicationGraphNode_AlwaysRelevant_ForConnection::UpdateCachedRelevantActor(const FConnectionGatherActorListParameters& Params, AActor* NewActor, AActor*& LastActor)
{
	// When the connection's focus moves off an actor, that actor's channel is given a
	// short grace period rather than being closed the moment it stops being relevant.
	if (NewActor != LastActor)
	{
		if (LastActor)
		{
			if (FConnectionReplicationActorInfo* LastActorInfo = Params.ConnectionManager.ActorInfoMap.Find(LastActor))
			{
				LastActorInfo->ActorChannelCloseFrameNum = Params.ReplicationFrameNum + 1;
			}
		}

		LastActor = NewActor;
	}

	if (NewActor)
	{
		FConnectionReplicationActorInfo& NewActorInfo = Params.ConnectionManager.ActorInfoMap.FindOrAdd(NewActor);
		NewActorInfo.ActorChannelCloseFrameNum = 0;
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode_TearOff_ForConnection
// --------------------------------------------------------------------------------------------------------------------------------------------

void UReplicationGraphNode_TearOff_ForConnection::NotifyTearOffActor(AActor* Actor, uint32 FrameNum)
{
	if (TearOffActors.FindByPredicate([Actor](const FTearOffActorInfo& Info) { return Info.Actor == Actor; }) == nullptr)
	{
		TearOffActors.Emplace(Actor, FrameNum);
	}
}

void UReplicationGraphNode_TearOff_ForConnection::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	ReplicationActorList.Reset();

	for (int32 Index = TearOffActors.Num() - 1; Index >= 0; --Index)
	{
		FTearOffActorInfo& TearOffInfo = TearOffActors[Index];
		AActor* Actor = TearOffInfo.Actor;

		// The actor gets exactly one more replication, then it is done and the channel is closed.
		if (TearOffInfo.bHasReppedOnce || !IsActorValidForReplication(Actor))
		{
			if (FConnectionReplicationActorInfo* ActorInfo = Params.ConnectionManager.ActorInfoMap.Find(Actor))
			{
				if (ActorInfo->Channel)
				{
					// Only close a channel that is still this actor's: the engine may have cleaned
					// it up already - which is the common case here, since a torn-off actor is
					// usually on its way out - and closing a pooled channel would either fault or
					// tear down whatever has since been given it. See ReplicateSingleActor.
					if (ActorInfo->Channel->GetActor() == Actor)
					{
						ActorInfo->Channel->Close(EChannelCloseReason::TearOff);
					}

					ActorInfo->Channel = nullptr;
				}
			}

			TearOffActors.RemoveAtSwap(Index);
			continue;
		}

		if (FConnectionReplicationActorInfo* ActorInfo = Params.ConnectionManager.ActorInfoMap.Find(Actor))
		{
			if (ActorInfo->LastRepFrameNum >= TearOffInfo.TearOffFrameNum)
			{
				TearOffInfo.bHasReppedOnce = true;
			}
		}

		ReplicationActorList.Add(Actor);
	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);
}

void UReplicationGraphNode_TearOff_ForConnection::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();
	PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "TearOff", ReplicationActorList);
	DebugInfo.PopIndent();
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UNetReplicationGraphConnection
// --------------------------------------------------------------------------------------------------------------------------------------------

void UNetReplicationGraphConnection::InitForGraph(UReplicationGraph* Graph)
{
	if (Graph && Graph->GetGraphGlobals().IsValid())
	{
		ActorInfoMap.SetGlobalMap(Graph->GetGraphGlobals()->GlobalActorReplicationInfoMap);
	}
}

void UNetReplicationGraphConnection::InitForConnection(UNetConnection* Connection)
{
	NetConnection = Connection;
}

void UNetReplicationGraphConnection::AddConnectionGraphNode(UReplicationGraphNode* Node)
{
	ConnectionGraphNodes.AddUnique(Node);
}

void UNetReplicationGraphConnection::RemoveConnectionGraphNode(UReplicationGraphNode* Node)
{
	ConnectionGraphNodes.RemoveSingleSwap(Node);
}

bool UNetReplicationGraphConnection::PrepareForReplication()
{
	if (NetConnection == nullptr)
	{
		return false;
	}

	BuildVisibleLevels();

	return true;
}

void UNetReplicationGraphConnection::BuildVisibleLevels()
{
	CachedVisibleLevels.Reset();
	GetClientVisibleLevelNames(CachedVisibleLevels);
}

void UNetReplicationGraphConnection::GetClientVisibleLevelNames(TSet<FName>& OutLevelNames) const
{
	OutLevelNames.Append(ClientVisibleLevelNames);
}

void UNetReplicationGraphConnection::NotifyClientVisibleLevelNamesAdd(FName LevelName, UWorld* StreamingWorld)
{
	ClientVisibleLevelNames.Add(LevelName);
	OnClientVisibleLevelNameAdd.Broadcast(LevelName, StreamingWorld);
}

void UNetReplicationGraphConnection::NotifyClientVisibleLevelNamesRemove(FName LevelName)
{
	ClientVisibleLevelNames.Remove(LevelName);
	OnClientVisibleLevelNameRemove.Broadcast(LevelName);
}

void UNetReplicationGraphConnection::NotifyActorChannelAdded(AActor* Actor, UActorChannel* Channel)
{
	FConnectionReplicationActorInfo& ActorInfo = ActorInfoMap.FindOrAdd(Actor);
	ActorInfo.Channel = Channel;
	ActorInfoMap.AddChannel(Actor, Channel);
}

void UNetReplicationGraphConnection::NotifyActorChannelRemoved(AActor* Actor)
{
	if (FConnectionReplicationActorInfo* ActorInfo = ActorInfoMap.Find(Actor))
	{
		ActorInfo->Channel = nullptr;
	}
}

void UNetReplicationGraphConnection::NotifyActorChannelCleanedUp(UActorChannel* Channel)
{
	if (FConnectionReplicationActorInfo* ActorInfo = ActorInfoMap.FindByChannel(Channel))
	{
		ActorInfo->Channel = nullptr;
	}

	ActorInfoMap.RemoveChannel(Channel);
}

void UNetReplicationGraphConnection::NotifyResetAllNetworkActors()
{
	ActorInfoMap.ResetActorMap();

	for (UReplicationGraphNode* Node : ConnectionGraphNodes)
	{
		Node->NotifyResetAllNetworkActors();
	}
}

void UNetReplicationGraphConnection::SetActorNotDormantOnConnection(AActor* InActor)
{
	if (FConnectionReplicationActorInfo* ActorInfo = ActorInfoMap.Find(InActor))
	{
		ActorInfo->bDormantOnConnection = false;
	}
}

FActorRepListRefView& UNetReplicationGraphConnection::GetPrevDormantActorListForNode(const UReplicationGraphNode* GridNode)
{
	return PrevDormantActorListPerNode.FindOrAdd(TObjectKey<UReplicationGraphNode>(GridNode));
}

void UNetReplicationGraphConnection::RemoveActorFromAllPrevDormantActorLists(AActor* InActor)
{
	for (auto It = PrevDormantActorListPerNode.CreateIterator(); It; ++It)
	{
		It.Value().RemoveFast(InActor);
	}
}

TArray<UNetReplicationGraphConnection::FVisibleCellInfo>& UNetReplicationGraphConnection::GetVisibleCellsForNode(const UReplicationGraphNode* GridNode)
{
	return NodesVisibleCells.FindOrAdd(TObjectKey<UReplicationGraphNode>(GridNode));
}

void UNetReplicationGraphConnection::CleanupNodeCaches(const UReplicationGraphNode* Node)
{
	PrevDormantActorListPerNode.Remove(TObjectKey<UReplicationGraphNode>(Node));
	NodesVisibleCells.Remove(TObjectKey<UReplicationGraphNode>(Node));
}

void UNetReplicationGraphConnection::TearDown()
{
	for (UReplicationGraphNode* Node : ConnectionGraphNodes)
	{
		Node->TearDown();
		delete Node;
	}

	ConnectionGraphNodes.Reset();

	if (TearOffNode)
	{
		TearOffNode->TearDown();
		delete TearOffNode;
		TearOffNode = nullptr;
	}

	ActorInfoMap.ResetActorMap();
	PrevDormantActorListPerNode.Reset();
	NodesVisibleCells.Reset();
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraph
// --------------------------------------------------------------------------------------------------------------------------------------------

UReplicationGraph::UReplicationGraph()
{
	GlobalActorChannelFrameNumTimeout = 2;

	ReplicationConnectionManagerFactory = []() { return new UNetReplicationGraphConnection(); };
}

UReplicationGraph::~UReplicationGraph()
{
	TearDown();
}

int32 UReplicationGraph::GetNetServerMaxTickRate() const
{
	const int32 TickRate = NetDriver ? NetDriver->NetServerMaxTickRate : 0;
	return TickRate > 0 ? TickRate : 30;
}

int32 UReplicationGraph::GetMaxBitsPerFrameForConnection(UNetConnection* Connection) const
{
	if (!Connection)
	{
		return 0;
	}

	const int32 NetSpeed = Connection->GetCurrentNetSpeed();
	if (NetSpeed <= 0)
	{
		return 0;
	}

	return (NetSpeed * 8) / GetNetServerMaxTickRate();
}

void UReplicationGraph::InitForNetDriver(UNetDriver* InNetDriver)
{
	NetDriver = InNetDriver;

	InitGlobalActorClassSettings();
	InitGlobalGraphNodes();

	if (NetDriver)
	{
		for (SDK::UNetConnection* ClientConnection : NetDriver->ClientConnections)
		{
			AddClientConnection(reinterpret_cast<UNetConnection*>(ClientConnection));
		}
	}
}

void UReplicationGraph::InitializeForWorld(UWorld* World)
{
	ActiveNetworkActors.Reset();
	GlobalActorReplicationInfoMap.ResetActorMap();

	for (UReplicationGraphNode* Node : GlobalGraphNodes)
	{
		Node->NotifyResetAllNetworkActors();
	}

	if (World)
	{
		InitializeActorsInWorld(World);
	}
}

void UReplicationGraph::SetRepDriverWorld(UWorld* InWorld)
{
	if (!GraphGlobals.IsValid())
	{
		GraphGlobals = MakeShared<FReplicationGraphGlobalData>(&GlobalActorReplicationInfoMap, InWorld, this);
	}
	else
	{
		GraphGlobals->World = InWorld;
	}
}

void UReplicationGraph::InitializeActorsInWorld(UWorld* InWorld)
{
	if (!InWorld)
	{
		return;
	}

	// Everything already in the world when the graph comes up has to be routed in, exactly
	// as it would have been had the graph been there when each actor was added.
	for (ULevel* Level : InWorld->Levels)
	{
		if (!Level)
		{
			continue;
		}

		for (SDK::AActor* LevelActor : Level->Actors)
		{
			AActor* Actor = reinterpret_cast<AActor*>(LevelActor);
			if (!IsActorValidForReplication(Actor) || !Actor->GetIsReplicated())
			{
				continue;
			}

			AddNetworkActor(Actor);
		}
	}
}

void UReplicationGraph::TearDown()
{
	for (UReplicationGraphNode* Node : GlobalGraphNodes)
	{
		Node->TearDown();
		delete Node;
	}
	GlobalGraphNodes.Reset();
	PrepareForReplicationNodes.Reset();

	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		ConnectionManager->TearDown();
		delete ConnectionManager;
	}
	Connections.Reset();

	for (UNetReplicationGraphConnection* ConnectionManager : PendingConnections)
	{
		ConnectionManager->TearDown();
		delete ConnectionManager;
	}
	PendingConnections.Reset();

	ActiveNetworkActors.Reset();
	GlobalActorReplicationInfoMap.ResetActorMap();
}

void UReplicationGraph::InitNode(UReplicationGraphNode* Node)
{
	Node->Initialize(GraphGlobals);

	if (Node->GetRequiresPrepareForReplication())
	{
		PrepareForReplicationNodes.Add(Node);
	}
}

void UReplicationGraph::InitGlobalActorClassSettings()
{
	// Multicast RPCs open a channel by default. Controllers are the exception: opening a
	// channel on a non-owning connection breaks the controller's replication entirely.
	RPC_Multicast_OpenChannelForClass.Reset();
	RPC_Multicast_OpenChannelForClass.Set(AActor::StaticClass(), true);
	RPC_Multicast_OpenChannelForClass.Set(AController::StaticClass(), false);
}

void UReplicationGraph::InitGlobalGraphNodes()
{
}

void UReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnectionManager)
{
	ConnectionManager->TearOffNode = CreateNewNode<UReplicationGraphNode_TearOff_ForConnection>();
	AddConnectionGraphNode(ConnectionManager->TearOffNode, ConnectionManager);
}

void UReplicationGraph::AddGlobalGraphNode(UReplicationGraphNode* GraphNode)
{
	GlobalGraphNodes.Add(GraphNode);
}

void UReplicationGraph::RemoveGlobalGraphNode(UReplicationGraphNode* GraphNode)
{
	GlobalGraphNodes.RemoveSingleSwap(GraphNode);
	PrepareForReplicationNodes.RemoveSingleSwap(GraphNode);

	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		ConnectionManager->CleanupNodeCaches(GraphNode);
	}
}

void UReplicationGraph::AddConnectionGraphNode(UReplicationGraphNode* GraphNode, UNetReplicationGraphConnection* ConnectionManager)
{
	if (ConnectionManager)
	{
		ConnectionManager->AddConnectionGraphNode(GraphNode);
	}
}

void UReplicationGraph::RemoveConnectionGraphNode(UReplicationGraphNode* GraphNode, UNetReplicationGraphConnection* ConnectionManager)
{
	if (ConnectionManager)
	{
		ConnectionManager->RemoveConnectionGraphNode(GraphNode);
	}
}

UNetReplicationGraphConnection* UReplicationGraph::FindConnectionManager(UNetConnection* NetConnection) const
{
	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		if (ConnectionManager->NetConnection == NetConnection)
		{
			return ConnectionManager;
		}
	}

	for (UNetReplicationGraphConnection* ConnectionManager : PendingConnections)
	{
		if (ConnectionManager->NetConnection == NetConnection)
		{
			return ConnectionManager;
		}
	}

	return nullptr;
}

UNetReplicationGraphConnection* UReplicationGraph::FindOrAddConnectionManager(UNetConnection* NetConnection)
{
	if (UNetReplicationGraphConnection* Existing = FindConnectionManager(NetConnection))
	{
		return Existing;
	}

	// The net driver has not officially handed us this connection yet, so it is parked in
	// PendingConnections until AddClientConnection is called for it.
	UNetReplicationGraphConnection* NewManager = CreateClientConnectionManagerInternal(NetConnection);
	PendingConnections.Add(NewManager);
	return NewManager;
}

UNetReplicationGraphConnection* UReplicationGraph::CreateClientConnectionManagerInternal(UNetConnection* Connection)
{
	UNetReplicationGraphConnection* NewConnectionManager = ReplicationConnectionManagerFactory
		? ReplicationConnectionManagerFactory()
		: new UNetReplicationGraphConnection();

	NewConnectionManager->InitForGraph(this);
	NewConnectionManager->InitForConnection(Connection);

	InitConnectionGraphNodes(NewConnectionManager);

	return NewConnectionManager;
}

void UReplicationGraph::AddClientConnection(UNetConnection* NetConnection)
{
	// It may already exist as a pending connection, in which case it is simply promoted.
	for (int32 Index = PendingConnections.Num() - 1; Index >= 0; --Index)
	{
		if (UNetReplicationGraphConnection* ConnectionManager = PendingConnections[Index])
		{
			if (ConnectionManager->NetConnection == NetConnection)
			{
				PendingConnections.RemoveAtSwap(Index);
				ConnectionManager->ConnectionOrderNum = Connections.Num();
				Connections.Add(ConnectionManager);
				return;
			}
		}
	}

	UNetReplicationGraphConnection* NewConnectionManager = CreateClientConnectionManagerInternal(NetConnection);
	NewConnectionManager->ConnectionOrderNum = Connections.Num();
	Connections.Add(NewConnectionManager);
}

void UReplicationGraph::RemoveClientConnection(UNetConnection* NetConnection)
{
	for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
	{
		UNetReplicationGraphConnection* ConnectionManager = Connections[Index];
		if (ConnectionManager->NetConnection == NetConnection)
		{
			ConnectionManager->TearDown();
			delete ConnectionManager;
			Connections.RemoveAt(Index);
			break;
		}
	}

	for (int32 Index = PendingConnections.Num() - 1; Index >= 0; --Index)
	{
		UNetReplicationGraphConnection* ConnectionManager = PendingConnections[Index];
		if (ConnectionManager->NetConnection == NetConnection)
		{
			ConnectionManager->TearDown();
			delete ConnectionManager;
			PendingConnections.RemoveAt(Index);
			break;
		}
	}

	// ConnectionOrderNum is only meaningful within a tick, so it is simply reassigned.
	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		Connections[Index]->ConnectionOrderNum = Index;
	}
}

void UReplicationGraph::AddNetworkActor(AActor* Actor)
{
	if (!IsActorValidForReplication(Actor))
	{
		return;
	}

	if (Actor->GetIsReplicated() == false)
	{
		return;
	}

	/*
	 * A DORM_Initial startup actor does not belong in the graph yet, and putting it in is what
	 * broke every building on the map.
	 *
	 * The engine never adds one: UWorld::AddNetworkActor is not called for a level-placed actor
	 * that is still DORM_Initial, because the client already has it from the level load and
	 * nothing is meant to be sent until something changes it. The first change calls
	 * AActor::FlushNetDormancy, which converts DORM_Initial to DORM_DormantAll and *then* asks
	 * the graph to add the actor - see FlushNetDormancy's bWasDormInitial branch below.
	 *
	 * This graph gets its initial population from InitializeActorsInWorld (and from the
	 * UNetDriver::AddNetworkActor detour, which the engine's own InitializeActorsForWorld walks
	 * every level actor through), so unlike the engine it does see these actors early. The
	 * result was a silent, permanent hole:
	 *
	 *   1. The building is added here at DORM_Initial and goes into ActiveNetworkActors.
	 *   2. Routing reaches UReplicationGraphNode_ActorList::NotifyAddNetworkActor, whose
	 *      ReplicationActorList.ConditionalAdd runs IsActorValidForReplicationGather - which
	 *      rejects exactly DORM_Initial && IsNetStartupActor. The actor is dropped from the
	 *      dormancy node's list without a word. (Streaming-level actors take the unfiltered
	 *      FStreamingLevelActorListCollection::AddActor branch, which is why this only ever
	 *      showed up on the persistent level.)
	 *   3. The pickaxe hit, or the door opening, flushes dormancy. bWasDormInitial is true, so
	 *      FlushNetDormancy calls AddNetworkActor to do the routing that step 2 threw away.
	 *   4. ...and AddNetworkActor returned right here, because step 1 had already put the actor
	 *      in ActiveNetworkActors.
	 *
	 * So the building stayed in ActiveNetworkActors and in GlobalActorReplicationInfoMap while
	 * belonging to no node at all. UReplicationGraphNode_DormancyNode::CreateConnectionNode
	 * seeds each new per-connection node with DeepCopyActorListsFrom(this), so every connection
	 * that had not already built its node for that cell never heard about the actor again: the
	 * door state and the harvest damage were correct on the server and never reached the client.
	 *
	 * Refusing the add keeps the engine's invariant - a DORM_Initial startup actor is not in the
	 * graph - so the flush's AddNetworkActor is a real add, and by then NetDormancy is
	 * DORM_DormantAll and ConditionalAdd accepts it.
	 */
	if (Actor->NetDormancy == ENetDormancy::DORM_Initial && Actor->IsNetStartupActor())
	{
		return;
	}

	bool bWasAlreadyThere = ActiveNetworkActors.Contains(Actor);
	if (bWasAlreadyThere)
	{
		return;
	}

	ActiveNetworkActors.Add(Actor);

	FGlobalActorReplicationInfo& GlobalInfo = GlobalActorReplicationInfoMap.Get(Actor);
	GlobalInfo.bWantsToBeDormant = Actor->NetDormancy > ENetDormancy::DORM_Awake;
	GlobalInfo.WorldLocation = Actor->GetActorLocation();

	// Unconditional, not rate limited: a pawn enters the graph a handful of times a match, and
	// pairing this against the removal log below is what says whether a pawn that is missing on
	// a client ever got into the graph, or got in and was taken straight back out.
	if (LogRelevancyDiagnostics > 0 && IsPawnishClass(Actor))
	{
		UE_LOG(LogReplicationGraph, Display, "RepDiag ADD pawn %s at (%.0f, %.0f, %.0f) dormancy=%d",
			Actor->GetName().c_str(), GlobalInfo.WorldLocation.X, GlobalInfo.WorldLocation.Y, GlobalInfo.WorldLocation.Z,
			static_cast<int32>(Actor->NetDormancy));
	}

	RouteAddNetworkActorToNodes(FNewReplicatedActorInfo(Actor), GlobalInfo);
}

void UReplicationGraph::RemoveNetworkActor(AActor* Actor)
{
	/*
	 * Paired with the ADD log above. UNetDriver::RemoveNetworkActor was only detoured recently;
	 * if a pawn shows up here shortly after it was added, that detour is pulling live pawns out
	 * of the graph and is the reason they are not replicating.
	 */
	if (LogRelevancyDiagnostics > 0 && IsPawnishClass(Actor))
	{
		UE_LOG(LogReplicationGraph, Display, "RepDiag REMOVE pawn %s (frame %u)", Actor->GetName().c_str(), ReplicationGraphFrame);
	}

	// Deliberately unconditional: an actor that was never routed can still have left
	// per-connection state behind, and this is the only chance to take it back out.
	ActiveNetworkActors.Remove(Actor);

	RouteRemoveNetworkActorToNodes(FNewReplicatedActorInfo(Actor));

	GlobalActorReplicationInfoMap.Remove(Actor);

	auto ForgetActorOnConnection = [Actor](UNetReplicationGraphConnection* ConnectionManager)
	{
		ConnectionManager->ActorInfoMap.RemoveActor(Actor);
		ConnectionManager->RemoveActorFromAllPrevDormantActorLists(Actor);
	};

	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		ForgetActorOnConnection(ConnectionManager);
	}

	// Pending connections hold the same per-actor state and are just as capable of outliving
	// the actor, so they are cleaned up too.
	for (UNetReplicationGraphConnection* ConnectionManager : PendingConnections)
	{
		ForgetActorOnConnection(ConnectionManager);
	}
}

void UReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
	// The base implementation does nothing: subclasses decide where an actor belongs.
}

void UReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	// The base implementation does nothing: subclasses decide where an actor belongs.
}

void UReplicationGraph::RouteRenameNetworkActorToNodes(const FRenamedReplicatedActorInfo& ActorInfo)
{
	for (UReplicationGraphNode* Node : GlobalGraphNodes)
	{
		Node->NotifyActorRenamed(ActorInfo, false);
	}
}

void UReplicationGraph::ForceNetUpdate(AActor* Actor)
{
	if (FGlobalActorReplicationInfo* RepInfo = GlobalActorReplicationInfoMap.Find(Actor))
	{
		RepInfo->ForceNetUpdateFrame = ReplicationGraphFrame;
		RepInfo->Events.ForceNetUpdate.Broadcast(Actor, *RepInfo);
	}
}

void UReplicationGraph::FlushNetDormancy(AActor* Actor, bool bWasDormInitial)
{
	FGlobalActorReplicationInfo& GlobalInfo = GlobalActorReplicationInfoMap.Get(Actor);

	if (GlobalInfo.LastFlushNetDormancyFrame == ReplicationGraphFrame)
	{
		return;
	}

	GlobalInfo.LastFlushNetDormancyFrame = ReplicationGraphFrame;

	// The actor is awake again on every connection that had it dormant.
	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		ConnectionManager->SetActorNotDormantOnConnection(Actor);
	}

	GlobalInfo.Events.DormancyFlush.Broadcast(Actor, GlobalInfo);

	// A DORM_Initial actor was never in the graph at all, so this is where it enters.
	if (bWasDormInitial)
	{
		AddNetworkActor(Actor);
	}
}

void UReplicationGraph::NotifyActorTearOff(AActor* Actor)
{
	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		if (ConnectionManager->TearOffNode)
		{
			ConnectionManager->TearOffNode->NotifyTearOffActor(Actor, ReplicationGraphFrame);
		}
	}
}

void UReplicationGraph::NotifyActorFullyDormantForConnection(AActor* Actor, UNetConnection* Connection)
{
	if (UNetReplicationGraphConnection* ConnectionManager = FindConnectionManager(Connection))
	{
		FConnectionReplicationActorInfo& ActorInfo = ConnectionManager->ActorInfoMap.FindOrAdd(Actor);
		ActorInfo.bDormantOnConnection = true;
	}
}

void UReplicationGraph::NotifyActorDormancyChange(AActor* Actor, ENetDormancy OldDormancyState)
{
	FGlobalActorReplicationInfo& GlobalInfo = GlobalActorReplicationInfoMap.Get(Actor);

	const ENetDormancy NewDormancyState = Actor->NetDormancy;
	GlobalInfo.bWantsToBeDormant = NewDormancyState > ENetDormancy::DORM_Awake;

	GlobalInfo.Events.DormancyChange.Broadcast(Actor, GlobalInfo, NewDormancyState, OldDormancyState);
}

void UReplicationGraph::NotifyActorRenamed(AActor* Actor, UObject* PreviousOuter, FName PreviousName)
{
	const FName PreviousStreamingLevelName = FNewReplicatedActorInfo::GetStreamingLevelNameOfActor(reinterpret_cast<AActor*>(PreviousOuter));

	FRenamedReplicatedActorInfo ActorInfo(Actor, PreviousStreamingLevelName);

	RouteRenameNetworkActorToNodes(ActorInfo);

	GlobalActorReplicationInfoMap.NotifyActorRenamed(Actor, PreviousStreamingLevelName);
}

void UReplicationGraph::SetRoleSwapOnReplicate(AActor* Actor, bool bSwapRoles)
{
	FGlobalActorReplicationInfo& GlobalInfo = GlobalActorReplicationInfoMap.Get(Actor);
	GlobalInfo.bSwapRolesOnReplicate = bSwapRoles;
}

void UReplicationGraph::SetActorDiscoveryBudget(int32 ActorDiscoveryBudgetInKBytesPerSec)
{
	ActorDiscoveryMaxBitsPerFrame = ActorDiscoveryBudgetInKBytesPerSec > 0
		? (ActorDiscoveryBudgetInKBytesPerSec * 1024 * 8) / GetNetServerMaxTickRate()
		: 0;
}

void UReplicationGraph::SetAllCullDistanceSettingsForActor(const FActorRepListType& Actor, float CullDistanceSquared)
{
	FGlobalActorReplicationInfo& GlobalInfo = GlobalActorReplicationInfoMap.Get(Actor);
	GlobalInfo.Settings.SetCullDistanceSquared(CullDistanceSquared);

	for (UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		if (FConnectionReplicationActorInfo* ConnectionInfo = ConnectionManager->ActorInfoMap.Find(Actor))
		{
			ConnectionInfo->SetCullDistanceSquared(CullDistanceSquared);
		}
	}
}

void UReplicationGraph::SetActorDestructionInfoToIgnoreDistanceCulling(AActor* DestroyedActor)
{
	// Always-relevant actors have to be destroyed on the client wherever the client is,
	// so their destruction info opts out of the distance check.
	if (FGlobalActorReplicationInfo* GlobalInfo = GlobalActorReplicationInfoMap.Find(DestroyedActor))
	{
		GlobalInfo->Settings.SetCullDistanceSquared(0.f);
	}
}

bool UReplicationGraph::IsConnectionReady(UNetConnection* Connection)
{
	/*
	 * The engine's test, in UNetDriver::ServerReplicateActors_PrepConnections, is
	 *     OwningActor != nullptr && State == USOCK_Open && (ElapsedTime - LastReceiveTime < 1.5)
	 *
	 * Only the first term is reproducible here. UNetConnection::State is not a UPROPERTY and
	 * has no recovered offset, and the "has this connection said anything recently" term needs
	 * UNetDriver::ElapsedTime, which is not reflected either. Guessing at those offsets is how
	 * replication was starved to a standstill once before, so the check is the part that can be
	 * made true rather than the part that would be nice to have.
	 *
	 * The consequence of the missing terms is that a connection that has gone quiet still costs
	 * a gather until the engine tears it down. That is wasted work, not incorrect work.
	 */
	return Connection != nullptr && Connection->GetOwningActor() != nullptr;
}

void UReplicationGraph::NotifyConnectionSaturated(UNetReplicationGraphConnection& Connection)
{
	bWasConnectionSaturated = true;
}

void UReplicationGraph::NotifyConnectionFastPathSaturated()
{
	bWasConnectionFastPathSaturated = true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// Replication
// --------------------------------------------------------------------------------------------------------------------------------------------

int32 UReplicationGraph::ServerReplicateActors(float DeltaSeconds)
{
	if (NetDriver == nullptr)
	{
		return 0;
	}

	++ReplicationGraphFrame;

	/*
	 * The shipped graph's very first act, and it is not optional: without it no actor is ever
	 * compared a second time.
	 *
	 * FReplicationChangelistMgr::Update returns early - skipping the property comparison
	 * entirely - while an actor's LastReplicationFrame still equals this counter, because
	 * net.ShareShadowState means "compare once per frame, share across connections". The
	 * counter is normally bumped by UNetDriver::ServerReplicateActors, which is a
	 * WITH_SERVER_CODE stub in this binary, so the graph is the only thing left to bump it.
	 * Leave it frozen and every actor is compared exactly once, on the frame its channel
	 * opens, and is clean forever after - initial state arrives and nothing ever updates.
	 *
	 * Verified against the shipped UReplicationGraph::ServerReplicateActors, which opens with
	 * ++NetDriver->ReplicationFrame before touching anything else.
	 */
	++GetEngineMember<uint32>(NetDriver, EngineAddresses::UNetDriver_ReplicationFrame);

	FrameReplicationStats.Reset();

	// Nodes that asked for it get their once-per-frame update before anything is gathered.
	for (UReplicationGraphNode* Node : PrepareForReplicationNodes)
	{
		Node->PrepareForReplication();
	}

	bWasConnectionSaturated = false;
	bWasConnectionFastPathSaturated = false;

	for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
	{
		UNetReplicationGraphConnection* ConnectionManager = Connections[ConnectionIndex];
		if (!ConnectionManager || !IsConnectionReady(ConnectionManager->NetConnection) || !ConnectionManager->PrepareForReplication())
		{
			continue;
		}

		ConnectionManager->ConnectionOrderNum = ConnectionIndex;

		/*
		 * Movement corrections first, and never gated on anything below.
		 *
		 * This is where the shipped graph sends them, and the ordering is deliberate: the
		 * client's own pawn is predicted, so an acknowledgement or a correction that arrives late
		 * is worse than an actor update that does. Bandwidth saturation later in this frame must
		 * not be able to suppress it.
		 */
		if (SDK::APlayerController* SdkController = ConnectionManager->NetConnection->GetPlayerController())
		{
			ToPrism(SdkController)->SendClientAdjustment();
		}

		for (SDK::UChildConnection* Child : ConnectionManager->NetConnection->Children)
		{
			UNetConnection* ChildConnection = reinterpret_cast<UNetConnection*>(Child);
			if (ChildConnection && ChildConnection->GetViewTarget() != nullptr)
			{
				if (SDK::APlayerController* ChildController = ChildConnection->GetPlayerController())
				{
					ToPrism(ChildController)->SendClientAdjustment();
				}
			}
		}

		ReplicateActorsForConnection(ConnectionManager->NetConnection, ConnectionManager->ActorInfoMap, ConnectionManager, ReplicationGraphFrame);

		FrameReplicationStats.NumConnections++;
	}

	// Only one connection pays for the expensive per-connection work each frame.
	if (Connections.Num() > 0)
	{
		HeavyComputationConnectionSelector = (HeavyComputationConnectionSelector + 1) % Connections.Num();
	}

	PostServerReplicateStats(FrameReplicationStats);

	return FrameReplicationStats.NumReplicatedActors;
}

void UReplicationGraph::ReplicateActorsForConnection(UNetConnection* NetConnection, FPerConnectionActorInfoMap& ConnectionActorInfoMap, UNetReplicationGraphConnection* ConnectionManager, const uint32 FrameNum)
{
	if (NetConnection == nullptr || ConnectionManager == nullptr)
	{
		return;
	}

	// ------------------------------------------
	//	Build the viewers for this connection
	// ------------------------------------------

	FNetViewerArray Viewers;
	Viewers.Emplace(NetConnection, 0.f);

	for (SDK::UChildConnection* Child : NetConnection->Children)
	{
		if (Child)
		{
			Viewers.Emplace(reinterpret_cast<UNetConnection*>(Child), 0.f);
		}
	}

	if (Viewers.Num() == 0 || Viewers[0].ViewTarget == nullptr)
	{
		return;
	}

	ConnectionManager->LastGatherLocations.Reset();
	for (const FNetViewer& Viewer : Viewers)
	{
		ConnectionManager->LastGatherLocations.Emplace(Viewer.Connection, Viewer.ViewLocation);
	}

	// ------------------------------------------
	//	Gather
	// ------------------------------------------

	FGatheredReplicationActorLists GatheredReplicationListsForConnection;

	const bool bIsSelectedForHeavyComputation = (ConnectionManager->ConnectionOrderNum == HeavyComputationConnectionSelector);

	FConnectionGatherActorListParameters Params(
		Viewers,
		*ConnectionManager,
		ConnectionManager->GetCachedClientVisibleLevelNames(),
		FrameNum,
		GatheredReplicationListsForConnection,
		bIsSelectedForHeavyComputation);

	for (UReplicationGraphNode* Node : GlobalGraphNodes)
	{
		Node->GatherActorListsForConnection(Params);
	}

	for (UReplicationGraphNode* Node : ConnectionManager->GetConnectionGraphNodes())
	{
		Node->GatherActorListsForConnection(Params);
	}

	// ------------------------------------------
	//	Prioritize and replicate
	// ------------------------------------------

	ReplicateActorListsForConnections_Default(ConnectionManager, GatheredReplicationListsForConnection, Viewers);

	if (GatheredReplicationListsForConnection.ContainsLists(EActorRepListTypeFlags::FastShared))
	{
		ReplicateActorListsForConnections_FastShared(ConnectionManager, GatheredReplicationListsForConnection, Viewers);
	}
}

void UReplicationGraph::ReplicateActorListsForConnections_Default(UNetReplicationGraphConnection* ConnectionManager, FGatheredReplicationActorLists& GatheredReplicationListsForConnection, FNetViewerArray& Viewers)
{
	UNetConnection* const NetConnection = ConnectionManager->NetConnection;
	FPerConnectionActorInfoMap& ConnectionActorInfoMap = ConnectionManager->ActorInfoMap;

	const uint32 FrameNum = ReplicationGraphFrame;

	PrioritizedReplicationList.Reset();
	TArray<FPrioritizedRepList::FItem>& OutItems = PrioritizedReplicationList.Items;

	// ------------------------------------------
	//	Prioritize
	// ------------------------------------------

	const bool bDiag = IsRelevancyDiagFrame(FrameNum);

	int32 DiagNumGathered = 0;
	int32 DiagNumInvalid = 0;
	int32 DiagNumDormant = 0;
	int32 DiagNumNotReady = 0;
	int32 DiagNumDistanceCulled = 0;

	for (const FActorRepListConstView& List : GatheredReplicationListsForConnection.GetLists(EActorRepListTypeFlags::Default))
	{
		for (int32 Index = 0; Index < List.Num(); ++Index)
		{
			FActorRepListType Actor = List[Index];

			++DiagNumGathered;

			// Only computed on diag frames: it builds a class name string.
			const bool bDiagPawn = bDiag && IsPawnishClass(Actor);

			if (!IsActorValidForReplication(Actor))
			{
				++DiagNumInvalid;
				if (bDiagPawn)
				{
					UE_LOG(LogReplicationGraph, Display, "RepDiag conn=%d pawn %s SKIPPED: not valid for replication.",
						ConnectionManager->ConnectionOrderNum, Actor->GetName().c_str());
				}
				continue;
			}

			FConnectionReplicationActorInfo& ConnectionData = ConnectionActorInfoMap.FindOrAdd(Actor);

			// Dormant on this connection: nothing to send until it is flushed.
			if (ConnectionData.bDormantOnConnection)
			{
				++DiagNumDormant;
				if (bDiagPawn)
				{
					UE_LOG(LogReplicationGraph, Display, "RepDiag conn=%d pawn %s SKIPPED: dormant on connection.",
						ConnectionManager->ConnectionOrderNum, Actor->GetName().c_str());
				}
				continue;
			}

			FGlobalActorReplicationInfo& GlobalData = GlobalActorReplicationInfoMap.Get(Actor);

			// Not time yet, unless game code forced an update since the last time it replicated.
			if (FrameNum < ConnectionData.NextReplicationFrameNum && GlobalData.ForceNetUpdateFrame <= ConnectionData.LastRepFrameNum)
			{
				++DiagNumNotReady;
				if (bDiagPawn)
				{
					UE_LOG(LogReplicationGraph, Display,
						"RepDiag conn=%d pawn %s SKIPPED: not ready (frame %u < next %u, period %u).",
						ConnectionManager->ConnectionOrderNum, Actor->GetName().c_str(),
						FrameNum, ConnectionData.NextReplicationFrameNum, static_cast<uint32>(ConnectionData.ReplicationPeriodFrame));
				}
				continue;
			}

			// ------------------------------------------
			//	Distance cull
			// ------------------------------------------

			float AccumulatedPriority = GlobalData.Settings.AccumulatedNetPriorityBias;
			double SmallestDistanceSq = MAX_dbl;

			const float CullDistanceSquared = ConnectionData.GetCullDistanceSquared();
			if (CullDistanceSquared > 0.f)
			{
				for (const FNetViewer& Viewer : Viewers)
				{
					SmallestDistanceSq = FMath::Min(SmallestDistanceSq, VectorDistSquared(GlobalData.WorldLocation, Viewer.ViewLocation));
				}

				if (bDiagPawn)
				{
					UE_LOG(LogReplicationGraph, Display,
						"RepDiag conn=%d pawn %s at (%.0f, %.0f, %.0f) distSq=%.0f cullSq=%.0f -> %s",
						ConnectionManager->ConnectionOrderNum, Actor->GetName().c_str(),
						GlobalData.WorldLocation.X, GlobalData.WorldLocation.Y, GlobalData.WorldLocation.Z,
						SmallestDistanceSq, CullDistanceSquared,
						SmallestDistanceSq > static_cast<double>(CullDistanceSquared) ? "DISTANCE CULLED" : "prioritized");
				}

				if (SmallestDistanceSq > static_cast<double>(CullDistanceSquared))
				{
					++DiagNumDistanceCulled;
					continue;
				}

				// Closer is more urgent, so distance adds to the (ascending) priority value.
				const float DistanceFactor = FMath::Clamp(static_cast<float>(SmallestDistanceSq) / PrioritizationConstants.MaxDistanceScaling, 0.f, 1.f)
					* GlobalData.Settings.DistancePriorityScale;

				AccumulatedPriority += DistanceFactor;
			}
			else if (bDiagPawn)
			{
				UE_LOG(LogReplicationGraph, Display,
					"RepDiag conn=%d pawn %s at (%.0f, %.0f, %.0f) cullSq=0 (always relevant on this connection) -> prioritized",
					ConnectionManager->ConnectionOrderNum, Actor->GetName().c_str(),
					GlobalData.WorldLocation.X, GlobalData.WorldLocation.Y, GlobalData.WorldLocation.Z);
			}

			// ------------------------------------------
			//	Starvation
			// ------------------------------------------

			// ------------------------------------------
			//	Channel timeout
			// ------------------------------------------

			/*
			 * Pushed out here rather than in ReplicateSingleActor.
			 *
			 * The graph returned this actor and it survived the cull, so it is relevant to this
			 * connection - whether or not it wins a slot in the budget, and whether or not this
			 * is one of its frames. Leaving the bump until the actor actually writes means any
			 * actor whose ReplicationPeriodFrame exceeds its channel timeout has its channel
			 * closed and reopened forever, paying a full initial state every cycle.
			 */
			UpdateActorChannelCloseFrameNum(Actor, ConnectionData, GlobalData, FrameNum, NetConnection);

			// ------------------------------------------
			//	Starvation
			// ------------------------------------------

			if (GlobalData.Settings.StarvationPriorityScale > 0.f)
			{
				// The scale multiplies the frame count, not the resulting factor. It reads as
				// "treat every frame this actor went without as if it were N", so a scale of 2
				// saturates in half the frames instead of doubling an already clamped result.
				const float FramesSinceLastRep = static_cast<float>(FrameNum - ConnectionData.LastRepFrameNum) * GlobalData.Settings.StarvationPriorityScale;
				const float StarvationFactor = 1.f - FMath::Clamp(FramesSinceLastRep / static_cast<float>(PrioritizationConstants.MaxFramesSinceLastRep), 0.f, 1.f);

				AccumulatedPriority += StarvationFactor;
			}

			// ------------------------------------------
			//	Pending dormancy
			// ------------------------------------------

			/*
			 * An actor that wants to be dormant and has replicated at least once is trying to
			 * finish and get out of the way. Letting newly spawned actors starve it out keeps it
			 * in the gather, and in every connection's actor info, indefinitely.
			 */
			if (GlobalData.bWantsToBeDormant && ConnectionData.LastRepFrameNum > 0)
			{
				AccumulatedPriority -= 1.5f;
			}

			// ------------------------------------------
			//	ForceNetUpdate
			// ------------------------------------------

			// The test is "has it replicated since the update was forced", not "was the update
			// forced recently": a starved actor keeps its bump until it actually sends. The
			// windowed form this replaces let a forced actor lose its bump while still waiting.
			if (GlobalData.ForceNetUpdateFrame > ConnectionData.LastRepFrameNum)
			{
				AccumulatedPriority -= 1.f;
			}

			// ------------------------------------------
			//	The connection's own actors
			// ------------------------------------------

			/*
			 * A client's own PlayerController and whatever it is looking through are the two most
			 * important actors on its connection: everything the player does passes through them,
			 * and the pawn's corrections are what keeps the client's own movement from
			 * rubber-banding. They sort in front of the world.
			 *
			 * This is per connection, which is the point - unlike a class-wide
			 * AccumulatedNetPriorityBias, it does not also privilege every other player's pawn on
			 * every other connection.
			 */
			for (const FNetViewer& Viewer : Viewers)
			{
				if (Actor == Viewer.ViewTarget || Actor == Viewer.InViewer)
				{
					AccumulatedPriority -= 10.f;
					break;
				}
			}

			OutItems.Emplace(AccumulatedPriority, Actor, &GlobalData, &ConnectionData);
		}
	}

	if (bDiag)
	{
		/*
		 * gathered == 0 means the graph handed this connection nothing - look at the grid line
		 * above it for where the connection thinks it is. gathered > 0 with prioritized == 0
		 * means everything was thrown away, and the counters say by which test.
		 */
		UE_LOG(LogReplicationGraph, Display,
			"RepDiag conn=%d frame=%u lists=%d gathered=%d prioritized=%d | skipped: invalid=%d dormant=%d notready=%d distcull=%d",
			ConnectionManager->ConnectionOrderNum, FrameNum,
			GatheredReplicationListsForConnection.NumLists(),
			DiagNumGathered, OutItems.Num(),
			DiagNumInvalid, DiagNumDormant, DiagNumNotReady, DiagNumDistanceCulled);
	}

	// Ascending: the smallest accumulated priority is the most urgent.
	OutItems.Sort();

	// ------------------------------------------
	//	Replicate
	// ------------------------------------------

	const int32 MaxBitsPerFrame = GetMaxBitsPerFrameForConnection(NetConnection);
	int64 BitsWritten = 0;

	ConnectionManager->QueuedBitsForActorDiscovery = 0;

	// TEMPORARY: see LogOwnerReplication.
	int32 NumActorsThatWroteBits = 0;

	int32 ActorIndex = 0;
	for (; ActorIndex < OutItems.Num(); ++ActorIndex)
	{
		const FPrioritizedRepList::FItem& RepItem = OutItems[ActorIndex];

		if (MaxBitsPerFrame > 0 && BitsWritten >= MaxBitsPerFrame)
		{
			NotifyConnectionSaturated(*ConnectionManager);

			// TEMPORARY: see LogOwnerReplication.
			if (LogOwnerReplication != 0)
			{
				UE_LOG(LogReplicationGraph, Display, "Connection saturated at %d/%d actors (%lld/%d bits) on frame %u",
					ActorIndex, OutItems.Num(), static_cast<long long>(BitsWritten), MaxBitsPerFrame, FrameNum);
			}

			break;
		}

		/*
		 * An actor can legitimately be in more than one gathered list - a squadmate's pawn is in
		 * the grid and in the squad node, a high frequency pawn is in the grid and in the always
		 * relevant node - and the prioritized list is built from all of them, so the same actor
		 * can appear twice. Without this it is replicated twice in one frame: wasted bandwidth,
		 * and the second write is guaranteed to be empty because the first one just cleaned it.
		 */
		if (RepItem.ConnectionData->LastRepFrameNum == FrameNum)
		{
			continue;
		}

		const int64 ActorBits = ReplicateSingleActor(RepItem.Actor, *RepItem.ConnectionData, *RepItem.GlobalData, ConnectionActorInfoMap, *ConnectionManager, FrameNum);

		BitsWritten += ActorBits;
		if (ActorBits > 0)
		{
			++NumActorsThatWroteBits;
		}
	}

	/*
	 * TEMPORARY: see LogOwnerReplication.
	 *
	 * The owning PlayerController writes 0 bits even across a forced property change, which
	 * cannot happen if its changelist is being compared. This says whether that is specific to
	 * that actor or whether the whole pass is writing nothing - if wrote=0 every frame, no
	 * actor is replicating and the problem is in the channel/replicator setup, not the graph.
	 */
	if (LogOwnerReplication != 0)
	{
		UE_LOG(LogReplicationGraph, Display, "RepSummary frame %u: gathered=%d visited=%d wrote=%d bits=%lld",
			FrameNum, OutItems.Num(), ActorIndex, NumActorsThatWroteBits, static_cast<long long>(BitsWritten));
	}

	// Everything we did not get to has to keep its channel open a little longer.
	if (ActorIndex < OutItems.Num())
	{
		HandleStarvedActorList(*ConnectionManager, PrioritizedReplicationList, ActorIndex, ConnectionActorInfoMap, FrameNum);
	}
}

void UReplicationGraph::ReplicateActorListsForConnections_FastShared(UNetReplicationGraphConnection* ConnectionManager, FGatheredReplicationActorLists& GatheredReplicationListsForConnection, FNetViewerArray& Viewers)
{
	FPerConnectionActorInfoMap& ConnectionActorInfoMap = ConnectionManager->ActorInfoMap;
	const uint32 FrameNum = ReplicationGraphFrame;

	int64 BitsWritten = 0;

	for (const FActorRepListConstView& List : GatheredReplicationListsForConnection.GetLists(EActorRepListTypeFlags::FastShared))
	{
		// The fast path is deliberately sampled rather than walked: it exists to keep a
		// cheap trickle of movement flowing, not to be complete.
		for (int32 Index = FrameNum % FMath::Max(1, FastSharedPathConstants.ListSkipPerFrame); Index < List.Num(); Index += FastSharedPathConstants.ListSkipPerFrame)
		{
			if (BitsWritten >= FastSharedPathConstants.MaxBitsPerFrame)
			{
				NotifyConnectionFastPathSaturated();
				return;
			}

			FActorRepListType Actor = List[Index];
			if (!IsActorValidForReplication(Actor))
			{
				continue;
			}

			FConnectionReplicationActorInfo& ConnectionData = ConnectionActorInfoMap.FindOrAdd(Actor);
			if (ConnectionData.bDormantOnConnection || ConnectionData.Channel == nullptr)
			{
				continue;
			}

			// A cached channel the engine has since cleaned up no longer holds this actor, and
			// replicating through it faults on its null Actor. The fast path never opens channels
			// of its own, so it drops the actor and lets the default path re-acquire one. Same
			// reasoning as ReplicateSingleActor - see the longer note there.
			if (ConnectionData.Channel->GetActor() != Actor)
			{
				ConnectionManager->NotifyActorChannelCleanedUp(ConnectionData.Channel);
				ConnectionData.Channel = nullptr;
				continue;
			}

			if (FrameNum < ConnectionData.FastPath_NextReplicationFrameNum)
			{
				continue;
			}

			FGlobalActorReplicationInfo& GlobalData = GlobalActorReplicationInfoMap.Get(Actor);
			if (!GlobalData.Settings.FastSharedReplicationFunc)
			{
				continue;
			}

			// The fast path is only for actors that are comfortably inside the cull distance.
			const float CullDistanceSquared = ConnectionData.GetCullDistanceSquared() * FastSharedPathConstants.DistanceRequirementPct;
			if (CullDistanceSquared > 0.f)
			{
				double SmallestDistanceSq = MAX_dbl;
				for (const FNetViewer& Viewer : Viewers)
				{
					SmallestDistanceSq = FMath::Min(SmallestDistanceSq, VectorDistSquared(GlobalData.WorldLocation, Viewer.ViewLocation));
				}

				if (SmallestDistanceSq > static_cast<double>(CullDistanceSquared))
				{
					continue;
				}
			}

			if (!GlobalData.FastSharedReplicationInfo.IsValid())
			{
				GlobalData.FastSharedReplicationInfo = MakeUnique<FFastSharedReplicationInfo>();
			}

			// The function is called at most once per frame per actor; every connection in
			// range then shares the result, which is the whole point of the path.
			if (GlobalData.FastSharedReplicationInfo->LastAttemptBuildFrameNum != FrameNum)
			{
				GlobalData.FastSharedReplicationInfo->LastAttemptBuildFrameNum = FrameNum;

				if (GlobalData.Settings.FastSharedReplicationFunc(Actor))
				{
					GlobalData.FastSharedReplicationInfo->LastBunchBuildFrameNum = FrameNum;
				}
			}

			if (GlobalData.FastSharedReplicationInfo->LastBunchBuildFrameNum != FrameNum)
			{
				continue;
			}

			const int64 ActorBits = ConnectionData.Channel->ReplicateActor();

			BitsWritten += ActorBits;

			ConnectionData.FastPath_LastRepFrameNum = FrameNum;
			ConnectionData.FastPath_NextReplicationFrameNum = FrameNum + ConnectionData.FastPath_ReplicationPeriodFrame;

			FrameReplicationStats.NumReplicatedFastPathActors++;
		}
	}
}

int64 UReplicationGraph::ReplicateSingleActor(const FActorRepListType& Actor, FConnectionReplicationActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalActorInfo, FPerConnectionActorInfoMap& ConnectionActorInfoMap, UNetReplicationGraphConnection& ConnectionManager, const uint32 FrameNum)
{
	UNetConnection* NetConnection = ConnectionManager.NetConnection;

	// Push out the next allowed replication frame before anything else, so an early
	// return still costs the actor its slot rather than letting it retry every frame.
	ActorInfo.LastRepFrameNum = FrameNum;
	ActorInfo.NextReplicationFrameNum = FrameNum + ActorInfo.ReplicationPeriodFrame;

	if (!IsActorValidForReplication(Actor))
	{
		return 0;
	}

	// PreReplication is called once per frame regardless of how many connections want the actor.
	if (GlobalActorInfo.LastPreReplicationFrame != FrameNum)
	{
		GlobalActorInfo.LastPreReplicationFrame = FrameNum;
		Actor->CallPreReplication(NetDriver);
	}

	UActorChannel* Channel = ActorInfo.Channel;

	/*
	 * Drop a cached channel that is no longer this actor's. This is a crash fix, not a tidy-up.
	 *
	 * FConnectionReplicationActorInfo::Channel is written when the graph opens or adopts a
	 * channel and is never written again. The engine, meanwhile, closes channels on its own
	 * schedule - an actor being destroyed, its level going out of scope, a relevancy or
	 * dormancy close - and UActorChannel::CleanUp nulls the channel's Actor and returns the
	 * channel to the connection's pool. The graph is supposed to hear about that through
	 * UNetReplicationGraphConnection::NotifyActorChannelCleanedUp / NotifyActorChannelRemoved,
	 * which the engine drives via UNetConnection::ReplicationConnectionDriver - a hookup this
	 * module does not have, so both functions exist with no caller and ActorInfo.Channel is
	 * left dangling onto a dead channel forever.
	 *
	 * Calling ReplicateActor on one of those reads its null Actor, which faults on the object
	 * header at +0x8 - "EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000008".
	 *
	 * Nothing hit it until buildings started replicating, because until then no channel the
	 * graph owned was ever closed underneath it. The first harvested tree does it: destroying
	 * the actor closes its channel, and the next frame the graph replicates through the
	 * corpse. Re-checking the actor costs one load and closes the whole class of bug,
	 * including the dormancy-close case that would otherwise land the moment
	 * UActorChannel::StartBecomingDormant is wired up.
	 */
	if (Channel != nullptr && Channel->GetActor() != Actor)
	{
		ConnectionManager.NotifyActorChannelCleanedUp(Channel);
		ActorInfo.Channel = nullptr;
		Channel = nullptr;
	}

	/*
	 * Adopt a channel the connection already has for this actor.
	 *
	 * FConnectionReplicationActorInfo::Channel is only ever written by NotifyActorChannelAdded
	 * below, so the graph knows about the channels it opened itself and nothing else. The engine
	 * opens channels outside the graph - the PlayerController's is created during the login
	 * handshake, before the graph has ever seen the connection - and for those ActorInfo.Channel
	 * is null. Without this the graph opens a *second* channel for an actor that already has one.
	 *
	 * That is not cosmetic. A duplicate PlayerController channel makes the client run the
	 * controller's client-side setup a second time (ReceivedPlayer -> InitInputSystem ->
	 * SetupInputComponent), which binds every input action twice, so every key press fires its
	 * action twice: the map opens and immediately toggles shut, the inventory does the same, and
	 * so on for anything bound to a key.
	 *
	 * The classic UNetDriver::ServerReplicateActors that [[reference-gameserver]] runs scans
	 * Connection->OpenChannels for precisely this reason, which is why it never had the problem.
	 *
	 * The proper fix is to wire UNetConnection::ReplicationConnectionDriver so the engine tells
	 * the graph about channels as they are created and cleaned up; this scan closes the hole in
	 * the meantime and is cheap, because it only runs for actors the graph has no channel for.
	 */
	if (Channel == nullptr && NetConnection != nullptr)
	{
		static SDK::UClass* const ActorChannelClass = SDK::UActorChannel::StaticClass();

		const int32 NumOpenChannels = NetConnection->OpenChannels.Num();
		for (int32 OpenChannelIndex = 0; OpenChannelIndex < NumOpenChannels; ++OpenChannelIndex)
		{
			SDK::UChannel* OpenChannel = NetConnection->OpenChannels[OpenChannelIndex];
			if (!OpenChannel || !OpenChannel->IsA(ActorChannelClass))
			{
				continue;
			}

			UActorChannel* ExistingChannel = reinterpret_cast<UActorChannel*>(OpenChannel);
			if (ExistingChannel->GetActor() == Actor)
			{
				Channel = ExistingChannel;
				ActorInfo.Channel = Channel;
				ConnectionManager.NotifyActorChannelAdded(Actor, Channel);
				break;
			}
		}
	}

	if (Channel == nullptr)
	{
		// Opening a channel is the expensive part, so it can be given its own budget.
		if (ActorDiscoveryMaxBitsPerFrame > 0 && ConnectionManager.QueuedBitsForActorDiscovery >= ActorDiscoveryMaxBitsPerFrame)
		{
			return 0;
		}

		static const FName NAME_Actor(L"Actor");

		Channel = reinterpret_cast<UActorChannel*>(NetConnection->CreateChannelByName(NAME_Actor, EChannelCreateFlags::OpenedLocally, INDEX_NONE));
		if (Channel == nullptr)
		{
			return 0;
		}

		Channel->SetChannelActor(Actor, ESetChannelActorFlags::None);

		// Deliberately worded to match the reference server's line in
		// UNetDriver::ServerReplicateActors_ProcessPrioritizedActors, so that the set of actors
		// each one decides to open a channel for can be diffed straight out of the two logs.
		UE_LOG(LogReplicationGraph, Log, "Created ActorChannel for %s", Actor->GetName().c_str());

		ActorInfo.Channel = Channel;
		ConnectionManager.NotifyActorChannelAdded(Actor, Channel);
	}

	// Some actors want their roles swapped for the duration of the write (the "role swap" case).
	const bool bSwapRoles = GlobalActorInfo.bSwapRolesOnReplicate;
	ENetRole SavedRole = ENetRole::ROLE_None;
	ENetRole SavedRemoteRole = ENetRole::ROLE_None;

	if (bSwapRoles)
	{
		SavedRole = Actor->Role;
		SavedRemoteRole = Actor->RemoteRole;
		Actor->Role = SavedRemoteRole;
		Actor->RemoteRole = SavedRole;
	}

	const int64 BitsWritten = Channel->ReplicateActor();

	if (bSwapRoles)
	{
		Actor->Role = SavedRole;
		Actor->RemoteRole = SavedRemoteRole;
	}

	/*
	 * TEMPORARY: the client will not leave the loading screen until it receives
	 * AFortPlayerController::bHasServerFinishedLoading, so this reports whether the owning
	 * controller is being written at all and whether the write carried anything. See
	 * AFortPlayerControllerZone::CheckIfLoaded: the client's own bHasClientFinishedLoading
	 * is gated on that replicated bool, so nothing else on the client moves until it lands.
	 * Set FortRepGraph::LogOwnerReplication to 0 once the handshake is confirmed.
	 */
	if (LogOwnerReplication != 0 && Actor == reinterpret_cast<AActor*>(NetConnection->GetPlayerController()))
	{
		const bool bServerFinishedLoading = *(reinterpret_cast<const uint8*>(Actor) + 0x0BF9) != 0;

		/*
		 * UActorChannel::ActorReplicator, a TSharedPtr at +0x90 - read out of ReplicateActor
		 * (sub_7E412AC), which passes *(Channel + 144) to FObjectReplicator::ReplicateProperties
		 * and clears that same offset on the PendingKill path. If it is null then SetChannelActor
		 * never built a replicator for this actor, which would explain writing zero bits forever.
		 */
		const void* ActorReplicator = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8*>(Channel) + 0x90);

		UE_LOG(LogReplicationGraph, Display, "OwnerRep frame %u: %s bits=%lld chan=%p repl=%p bHasServerFinishedLoading=%d",
			FrameNum, Actor->GetName().c_str(), static_cast<long long>(BitsWritten), Channel, ActorReplicator, bServerFinishedLoading ? 1 : 0);
	}

	FrameReplicationStats.NumReplicatedActors++;
	if (BitsWritten == 0)
	{
		FrameReplicationStats.NumReplicatedCleanActors++;
	}

	/*
	 * Diagnostic: what a *remote* pawn's replication actually carried.
	 *
	 * The connection's own pawn is excluded, because the client predicts that one and its
	 * position on screen does not come from here. What is left is exactly the case being chased:
	 * another player standing in the wrong place.
	 *
	 * Reading the two numbers together:
	 *
	 *   bits > 0 while the location moves  -> the server is sending correct, current positions,
	 *                                         and the remaining error is client side (movement
	 *                                         component smoothing, or the based-movement
	 *                                         reference failing to resolve).
	 *   bits = 0 while the location moves  -> the property comparison is not seeing the movement.
	 *                                         The position on the client is frozen at whatever
	 *                                         arrived last, which looks exactly like "the wrong
	 *                                         location". This is the same signature as the frozen
	 *                                         NetDriver::ReplicationFrame bug.
	 *
	 * Every tenth frame, so it stays readable at 30Hz.
	 */
	if (LogRelevancyDiagnostics > 0 && (FrameNum % 10) == 0 && IsPawnishClass(Actor)
		&& Actor != NetConnection->GetViewTarget())
	{
		UE_LOG(LogReplicationGraph, Display, "RepDiag SEND conn=%d frame=%u pawn %s at (%.1f, %.1f, %.1f) bits=%lld",
			ConnectionManager.ConnectionOrderNum, FrameNum, Actor->GetName().c_str(),
			GlobalActorInfo.WorldLocation.X, GlobalActorInfo.WorldLocation.Y, GlobalActorInfo.WorldLocation.Z,
			static_cast<long long>(BitsWritten));
	}

	UpdateActorChannelCloseFrameNum(Actor, ActorInfo, GlobalActorInfo, FrameNum, NetConnection);

	// Dependent actors ride along with their parent: they are replicated straight after it,
	// even when they are outside their own cull distance. Almost no actor has any, so the
	// gather is not entered at all unless there is something to gather.
	if (!GlobalActorInfo.HasDependentActors())
	{
		return BitsWritten;
	}

	FGatheredReplicationActorLists DependentLists;
	GlobalActorInfo.GatherDependentActorLists(ConnectionManager, DependentLists);

	int64 TotalBitsWritten = BitsWritten;

	for (const FActorRepListConstView& DependentList : DependentLists.GetLists(EActorRepListTypeFlags::Default))
	{
		for (int32 Index = 0; Index < DependentList.Num(); ++Index)
		{
			FActorRepListType DependentActor = DependentList[Index];
			if (!IsActorValidForReplication(DependentActor))
			{
				continue;
			}

			FConnectionReplicationActorInfo& DependentActorInfo = ConnectionActorInfoMap.FindOrAdd(DependentActor);

			if (DependentActorInfo.bDormantOnConnection)
			{
				continue;
			}

			// The dependency does not override the dependent's own update frequency.
			if (FrameNum < DependentActorInfo.NextReplicationFrameNum)
			{
				continue;
			}

			FGlobalActorReplicationInfo& DependentGlobalInfo = GlobalActorReplicationInfoMap.Get(DependentActor);

			TotalBitsWritten += ReplicateSingleActor(DependentActor, DependentActorInfo, DependentGlobalInfo, ConnectionActorInfoMap, ConnectionManager, FrameNum);
		}
	}

	return TotalBitsWritten;
}

void UReplicationGraph::UpdateActorChannelCloseFrameNum(AActor* Actor, FConnectionReplicationActorInfo& ConnectionData, const FGlobalActorReplicationInfo& GlobalData, const uint32 FrameNum, UNetConnection* NetConnection) const
{
	// An actor that never times out (ActorChannelFrameTimeout == 0) keeps its channel forever.
	if (GlobalData.Settings.ActorChannelFrameTimeout > 0)
	{
		const uint32 ExpectedFrames = static_cast<uint32>(ConnectionData.ReplicationPeriodFrame)
			+ static_cast<uint32>(GlobalData.Settings.ActorChannelFrameTimeout)
			+ GlobalActorChannelFrameNumTimeout;

		ConnectionData.ActorChannelCloseFrameNum = FrameNum + ExpectedFrames;
	}
}

void UReplicationGraph::HandleStarvedActorList(const UNetReplicationGraphConnection& RepGraphConnection, const FPrioritizedRepList& List, int32 StartIdx, FPerConnectionActorInfoMap& ConnectionActorInfoMap, uint32 FrameNum)
{
	for (int32 Index = StartIdx; Index < List.Items.Num(); ++Index)
	{
		const FPrioritizedRepList::FItem& Item = List.Items[Index];

		// The actor wanted to replicate and we ran out of bandwidth, so its channel must not
		// be closed underneath it just because it did not get to send.
		if (Item.ConnectionData->ActorChannelCloseFrameNum <= FrameNum + 1)
		{
			Item.ConnectionData->ActorChannelCloseFrameNum = FrameNum + 1 + Item.GlobalData->Settings.ActorChannelFrameTimeout;
		}

		// Whatever rides along with it has to survive the starvation too.
		if (!Item.GlobalData->HasDependentActors())
		{
			continue;
		}

		FGatheredReplicationActorLists DependentLists;
		Item.GlobalData->GatherDependentActorLists(RepGraphConnection, DependentLists);

		for (const FActorRepListConstView& DependentList : DependentLists.GetLists(EActorRepListTypeFlags::Default))
		{
			for (int32 DependentIndex = 0; DependentIndex < DependentList.Num(); ++DependentIndex)
			{
				if (FConnectionReplicationActorInfo* DependentInfo = ConnectionActorInfoMap.Find(DependentList[DependentIndex]))
				{
					if (DependentInfo->ActorChannelCloseFrameNum <= FrameNum + 1)
					{
						DependentInfo->ActorChannelCloseFrameNum = FrameNum + 1 + Item.GlobalData->Settings.ActorChannelFrameTimeout;
					}
				}
			}
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------
// Debugging
// --------------------------------------------------------------------------------------------------------------------------------------------

void UReplicationGraph::LogGraph(FReplicationGraphDebugInfo& DebugInfo) const
{
	LogGlobalGraphNodes(DebugInfo);
	LogConnectionGraphNodes(DebugInfo);
}

void UReplicationGraph::LogGlobalGraphNodes(FReplicationGraphDebugInfo& DebugInfo) const
{
	for (const UReplicationGraphNode* Node : GlobalGraphNodes)
	{
		Node->LogNode(DebugInfo, Node->GetDebugString());
	}
}

void UReplicationGraph::LogConnectionGraphNodes(FReplicationGraphDebugInfo& DebugInfo) const
{
	for (const UNetReplicationGraphConnection* ConnectionManager : Connections)
	{
		DebugInfo.Log("Connection " + std::to_string(ConnectionManager->ConnectionOrderNum));
		DebugInfo.PushIndent();

		for (const UReplicationGraphNode* Node : ConnectionManager->GetConnectionGraphNodes())
		{
			Node->LogNode(DebugInfo, Node->GetDebugString());
		}

		DebugInfo.PopIndent();
	}
}

PRISM_NAMESPACE_END
