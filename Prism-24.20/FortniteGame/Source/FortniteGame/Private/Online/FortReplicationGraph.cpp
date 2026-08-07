#include "pch.h"

/**
*
*	===================== FortReplicationGraph Replication =====================
*
*	Overview
*
*		This changes the way actor relevancy works. AActor::IsNetRelevantFor is NOT used in this system!
*
*		Instead, the UFortReplicationGraph contains UReplicationGraphNodes. These nodes are responsible for generating lists of actors to replicate for each connection.
*		Most of these lists are persistent across frames. This enables most of the gathering work ("which actors should be considered for replication") to be shared/reused.
*		Nodes may be global (used by all connections), connection specific (each connection gets its own node), or shared (e.g, teams: all connections on the same team share).
*		Actors can be in multiple nodes! For example a pawn may be in the spatialization node but also in the always-relevant-for-squad node. It will be returned twice for
*		squadmates. This is ok though should be minimized when possible.
*
*	Fortnite Nodes
*
*		These are the top level nodes currently used:
*
*		UReplicationGraphNode_GridSpatialization2D (RootGridNode):
*		The spatialization node. All "distance based relevant" actors are routed here. The node divides the map into a 2D grid; each cell holds children nodes that group
*		actors by how they update or go dormant. Actors are placed in every cell their cull distance reaches. Connections pull from the single cell they stand in.
*
*		UReplicationGraphNode_ActorList (AlwaysRelevantNode)
*		The always relevant actors: relevant to every connection, every frame.
*
*		UFortReplicationGraphNode_AlwaysRelevant_ForConnection
*		Connection specific always relevant actors. Rebuilt every frame, because everything in it hangs off the PlayerController.
*
*		UFortReplicationGraphNode_PlayerStateFrequencyLimiter (PlayerStateLimiterNode / ReplayPlayerStateLimiterNode)
*		Replicates a small rolling set of player states so that simulated player states arrive at a low, steady frequency and take advantage of serialization sharing.
*		Autonomous proxy player states go out at full rate through the always-relevant-for-connection node instead.
*
*		UFortReplicationGraphNode_LiveSpectator
*		Actors only a spectating connection needs.
*
*		UReplicationGraphNode_TearOff_ForConnection
*		Connection specific node for tear off actors. Created and managed by the base Replication Graph.
*
*	How To Use
*
*		Making something always relevant: please avoid if you can. If you must, AActor::bAlwaysRelevant = true in the class defaults does it.
*
*		Making something always relevant to a connection: modify UFortReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection, and make sure the
*		actor does not also end up in another node - the safest way is EClassRepNodeMapping::NotRouted in UFortReplicationGraph::InitGlobalActorClassSettings.
*
*/

#include "Online/FortReplicationGraph.h"

#include "Athena/FortGameModeAthena.h"
#include "Templates/Casts.h"

PRISM_NAMESPACE_START

/*-----------------------------------------------------------------------------
	Console variables.

	Names and default values are taken from the shipped client: every one of
	these is still registered by FortReplicationGraph.cpp's static initializers
	even though the code that reads them was compiled out of the client.
-----------------------------------------------------------------------------*/

namespace FortRepGraph
{
	/** Fort.RepGraph.DestructInfo.MaxDist - max distance (not squared) to replicate destruction infos at. */
	float DestructionInfoMaxDist = 30000.f;

	/** FortRepGraph.DisplayClientLevelStreaming */
	int32 DisplayClientLevelStreaming = 0;

	/** FortRepGraph.CellSize - size of one spatialization cell. */
	float CellSize = 20000.f;

	/**
	 * FortRepGraph.SpatialBiasX / SpatialBiasY - effectively "min X" and "min Y"
	 * for replication. Only an initial value: the grid resets itself if an actor
	 * turns up outside it.
	 */
	float SpatialBiasX = -114936.f;
	float SpatialBiasY = -150000.f;

	/** FortRepGraph.DynamicActorFrequencyBuckets - buckets to spread dynamic spatialized actors across. */
	int32 DynamicActorFrequencyBuckets = 3;

	/** FortRepGraph.DynamicActorFrequencyBuckets.BigTeamMode */
	int32 DynamicActorFrequencyBuckets_BigTeamMode = 6;

	/** FortRepGraph.DisableSpatialrebuilds */
	int32 DisableSpatialRebuilds = 1;

	/** FortRepGraph.LogLazyInitClasses */
	int32 LogLazyInitClasses = 0;

	/** FortRepGraph.LogGlobalClassSettings - dump UGlobalRepGraphActorClassSettings::ClassSettings at init. */
	int32 LogGlobalClassSettings = 1;

	/** FortRepGraph.LogNotRoutedActors - log the first actor of each class the graph refuses to route. */
	int32 LogNotRoutedActors = 1;

	/** FortRepGraph.TargetKBitsSecFastSharedPath - bandwidth for FastShared updates, counted separately from the net driver's budget. */
	int32 TargetKBitsSecFastSharedPath = 81920;

	/** FortRepGraph.FastSharedPathCullDistPct */
	float FastSharedPathCullDistPct = 0.80f;

	/** FortRepGraph.FastSharedPathFrameModulo */
	int32 FastSharedPathFrameModulo = 2;

	/** FortRepGraph.MinVelocitySqrFastSharedPath */
	float MinVelocitySqrFastSharedPath = 250000.f;

	/** FortRepGraph.EnableFastSharedPath */
	int32 EnableFastSharedPath = 1;

	/** FortRepGraph.DSF.Enabled - use the dynamic spatial frequency node for dynamic actors. */
	int32 DSF_Enabled = 1;

	/** FortRepGraph.MaxZDormantNode - connections above this Z do not build per-connection dormancy nodes. */
	float MaxZDormantNode = 50000.f;

	/** FortRepGraph.ConnectionDormancy.NumFramesUntilObsolete */
	int32 ConnectionDormancy_NumFramesUntilObsolete = 990;

	/** FortRepGraph.PlayerPawnKBytesSec */
	float PlayerPawnKBytesSec = 30.f;

	/** FortRepGraph.AIPawnKBytesSec */
	float AIPawnKBytesSec = 15.f;

	/** FortRepGraph.MaxExpectedPlayerStates */
	int32 MaxExpectedPlayerStates = 200;

	/** FortRepGraph.Replay.MaxPlayerStatesPerFrame */
	int32 Replay_MaxPlayerStatesPerFrame = 10;

	/** FortRepGraph.HighFrequencyPawnUpdates */
	int32 HighFrequencyPawnUpdates = 1;

	/** FortRepGraph.DynamicSpatializedNodeForVehicles */
	bool DynamicSpatializedNodeForVehicles = true;

	/** FortRepGraph.CloseChannelsOnPawnsRelevantOnlyToOwner */
	bool CloseChannelsOnPawnsRelevantOnlyToOwner = true;

	/** FortRepGraph.InstantFlush.LargeTeamMode */
	int32 InstantFlush_LargeTeamMode = 1;

	/** FortRepGraph.Broadcast.RelevancyClassesFrameSpread */
	int32 Broadcast_RelevancyClassesFrameSpread = 2;

	/** FortRepGraph.ThrottleUnequippedWeapons */
	int32 ThrottleUnequippedWeapons = 0;

	/** FortRepGraph.FortVolumeGridCellSize */
	float FortVolumeGridCellSize = 10000.f;

	/** FortRepGraph.FortVolumeGrid.ExtraSpatialBias */
	float FortVolumeGrid_ExtraSpatialBias = 0.f;

	UReplicationGraph* ConditionalCreateReplicationDriver(UNetDriver* ForNetDriver, UWorld* World)
	{
		static const FName NAME_GameNetDriver(L"GameNetDriver");

		// Only the game net driver gets a graph. Beacon and demo drivers do their own thing.
		if (World && ForNetDriver && ForNetDriver->NetDriverName == NAME_GameNetDriver)
		{
			UE_LOG(LogFortReplicationGraph, Display, "Replication graph is enabled for %s in world %s.",
				ForNetDriver->GetName().c_str(), World->GetName().c_str());

			return new UFortReplicationGraph();
		}

		return nullptr;
	}
}

// ----------------------------------------------------------------------------------------------------------

namespace
{
	/** Whether a class is a pawn that carries dependent actors. Resolved once. */
	bool IsFortPawnClass(UClass* Class)
	{
		static UClass* FortPawnClass = UObject::FindClassFast("FortPawn");
		return Class != nullptr && FortPawnClass != nullptr && Class->IsSubclassOf(FortPawnClass);
	}
}

UFortReplicationGraph::UFortReplicationGraph()
{
	ReplicationConnectionManagerFactory = []() -> UNetReplicationGraphConnection* { return new UFortReplicationGraphConnection(); };
}

void UFortReplicationGraph::ResetGameWorldState()
{
	UReplicationGraph::ResetGameWorldState();

	AlwaysRelevantStreamingLevelActors.Reset();

	// The pawns and their dependents belonged to the world that just went away.
	TrackedPawns.Reset();
	TrackedPawnDependents.Reset();

	for (UNetReplicationGraphConnection* ConnManager : Connections)
	{
		for (UReplicationGraphNode* ConnectionNode : ConnManager->GetConnectionGraphNodes())
		{
			if (UFortReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = Cast<UFortReplicationGraphNode_AlwaysRelevant_ForConnection>(ConnectionNode))
			{
				AlwaysRelevantConnectionNode->ResetGameWorldState();
			}
		}
	}

	for (UNetReplicationGraphConnection* ConnManager : PendingConnections)
	{
		for (UReplicationGraphNode* ConnectionNode : ConnManager->GetConnectionGraphNodes())
		{
			if (UFortReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = Cast<UFortReplicationGraphNode_AlwaysRelevant_ForConnection>(ConnectionNode))
			{
				AlwaysRelevantConnectionNode->ResetGameWorldState();
			}
		}
	}
}

EClassRepNodeMapping UFortReplicationGraph::GetClassNodeMapping(UClass* Class) const
{
	if (!Class)
	{
		return EClassRepNodeMapping::NotRouted;
	}

	if (const EClassRepNodeMapping* Ptr = ClassRepNodePolicies.FindWithoutClassRecursion(Class))
	{
		return *Ptr;
	}

	AActor* ActorCDO = ToActor(static_cast<SDK::AActor*>(Class->ClassDefaultObject));
	if (!ActorCDO || !ActorCDO->GetIsReplicated())
	{
		return EClassRepNodeMapping::NotRouted;
	}

	auto ShouldSpatialize = [](const AActor* CDO)
	{
		return CDO->GetIsReplicated() && !(CDO->bAlwaysRelevant || CDO->bOnlyRelevantToOwner || CDO->bNetUseOwnerRelevancy);
	};

	// Only handle a class if it differs from its super: there is no reason to put every
	// child class explicitly in the mapping.
	UClass* SuperClass = static_cast<UClass*>(Class->SuperStruct);
	if (SuperClass)
	{
		if (AActor* SuperCDO = ToActor(static_cast<SDK::AActor*>(SuperClass->ClassDefaultObject)))
		{
			if (SuperCDO->GetIsReplicated() == ActorCDO->GetIsReplicated()
				&& SuperCDO->bAlwaysRelevant == ActorCDO->bAlwaysRelevant
				&& SuperCDO->bOnlyRelevantToOwner == ActorCDO->bOnlyRelevantToOwner
				&& SuperCDO->bNetUseOwnerRelevancy == ActorCDO->bNetUseOwnerRelevancy)
			{
				return GetClassNodeMapping(SuperClass);
			}
		}
	}

	if (ShouldSpatialize(ActorCDO))
	{
		return EClassRepNodeMapping::Spatialize_Dynamic;
	}

	if (ActorCDO->bAlwaysRelevant && !ActorCDO->bOnlyRelevantToOwner)
	{
		return EClassRepNodeMapping::RelevantAllConnections;
	}

	return EClassRepNodeMapping::NotRouted;
}

void UFortReplicationGraph::RegisterClassRepNodeMapping(UClass* Class)
{
	const EClassRepNodeMapping Mapping = GetClassNodeMapping(Class);
	ClassRepNodePolicies.Set(Class, Mapping);
}

void UFortReplicationGraph::InitClassReplicationInfo(FClassReplicationInfo& Info, UClass* Class, bool Spatialize) const
{
	AActor* CDO = ToActor(static_cast<SDK::AActor*>(Class->ClassDefaultObject));
	if (!CDO)
	{
		return;
	}

	if (Spatialize)
	{
		Info.SetCullDistanceSquared(CDO->GetNetCullDistanceSquared());
	}

	Info.ReplicationPeriodFrame = GetReplicationPeriodFrameForFrequency(CDO->GetNetUpdateFrequency());
}

bool UFortReplicationGraph::ConditionalInitClassReplicationInfo(UClass* ReplicatedClass, FClassReplicationInfo& ClassInfo)
{
	// Anything InitGlobalActorClassSettings set by hand keeps what it was given.
	if (ExplicitlySetClasses.FindByPredicate([&](const UClass* SetClass) { return ReplicatedClass->IsSubclassOf(SetClass); }) != nullptr)
	{
		return false;
	}

	const bool bClassIsSpatialized = IsSpatialized(ClassRepNodePolicies.GetChecked(ReplicatedClass));
	InitClassReplicationInfo(ClassInfo, ReplicatedClass, bClassIsSpatialized);
	return true;
}

void UFortReplicationGraph::AddClassRepInfo(UClass* Class, EClassRepNodeMapping Mapping)
{
	if (IsSpatialized(Mapping))
	{
		if (AActor* CDO = ToActor(static_cast<SDK::AActor*>(Class->ClassDefaultObject)))
		{
			if (CDO->bAlwaysRelevant)
			{
				UE_LOG(LogFortReplicationGraph, Warning, "Replicated Class %s is AlwaysRelevant but is initialized into a spatialized node (%d)",
					Class->GetName().c_str(), static_cast<int32>(Mapping));
			}
		}
	}

	ClassRepNodePolicies.Set(Class, Mapping);
}

void UFortReplicationGraph::RegisterClassReplicationInfo(UClass* ReplicatedClass)
{
	FClassReplicationInfo ClassInfo;
	if (ConditionalInitClassReplicationInfo(ReplicatedClass, ClassInfo))
	{
		GetGlobalActorReplicationInfoMap().SetClassInfo(ReplicatedClass, ClassInfo);
	}
}

/**
 * Epic does not hardcode the class policy table. The shipped
 * UFortReplicationGraph::InitGlobalActorClassSettings walks UGlobalRepGraphActorClassSettings::ClassSettings
 * three times and drives everything from it:
 *
 *   bAddClassRepInfoToMap                     -> AddClassRepInfo(Class, ClassNodeMapping)
 *   bUseCustomClassRepInfo                    -> SetClassInfo(Class, ClassRepInfo)
 *   bAddToRPC_Multicast_OpenChannelForClassMap-> RPC_Multicast_OpenChannelForClass.Set(Class, bRPC_Multicast_OpenChannelForClass)
 *
 * The only class it names in code is AFortSafeZoneIndicatorFuture (NotRouted). Everything else -
 * the list the explicit policies below try to reproduce by hand - comes from this array.
 *
 * Layout is from the 24.20 SDK, deliberately not from the 19.10 build the logic was read out of:
 * 19.10 stores the class as an FString with a 0xA0 stride, 24.20 an FName with 0x90.
 *
 *   UGlobalRepGraphActorClassSettings  0x38 TArray<FRepGraphActorClassSettings> ClassSettings
 *   FRepGraphActorSettingsBase         0x00 flag bitfield, 0x01 EClassRepNodeMapping,
 *                                      0x10 FClassReplicationInfo (0x70)
 *   FRepGraphActorClassSettings        0x88 FName ActorClassName, 0x8C FName MapToTemplate
 *
 * FClassReplicationInfo's layout is confirmed twice over: the shipped function builds one inline
 * as {1.0f, 1.0f, 0, 0x00010001, 4}, which pins the first six fields and matches this module's
 * field order exactly; and FortAthenaVehicle's entry carries 25000.0 at 0x64 with 625000000.0 at
 * 0x68, which pins CullDistance and CullDistanceSquared (25000 squared).
 */
namespace GlobalRepGraphClassSettings
{
	/** UGlobalRepGraphActorClassSettings::ClassSettings - a TArray: Data at +0x00, Num at +0x08. */
	constexpr int32 Offset_ClassSettings = 0x38;

	/** FRepGraphActorClassSettings. */
	constexpr int32 EntryStride           = 0x90;
	constexpr int32 Offset_ClassRepInfo   = 0x10;
	constexpr int32 Offset_ActorClassName = 0x88;
	constexpr int32 Offset_MapToTemplate  = 0x8C;
	constexpr int32 ClassRepInfoSize      = 0x70;

	/** FClassReplicationInfo, as the shipping binary lays it out. */
	constexpr int32 Info_DistancePriorityScale      = 0x00;
	constexpr int32 Info_StarvationPriorityScale    = 0x04;
	constexpr int32 Info_AccumulatedNetPriorityBias = 0x08;
	constexpr int32 Info_ReplicationPeriodFrame     = 0x0C;
	constexpr int32 Info_FastPathPeriodFrame        = 0x0E;
	constexpr int32 Info_ActorChannelFrameTimeout   = 0x10;
	constexpr int32 Info_CullDistanceSquared        = 0x68;

	enum EEntryFlags : uint8
	{
		AddClassRepInfoToMap  = 1 << 0,
		UseCustomClassRepInfo = 1 << 1,
		AddToMulticastMap     = 1 << 4,
		MulticastOpenChannel  = 1 << 5,
	};

	/** Returns the entry block, or nullptr when the config never reached the CDO. */
	static const uint8* GetEntries(int32& OutNum)
	{
		OutNum = 0;

		UClass* SettingsClass = UObject::FindClassFast("GlobalRepGraphActorClassSettings");
		if (!SettingsClass)
		{
			return nullptr;
		}

		const uint8* Settings = reinterpret_cast<const uint8*>(SettingsClass->ClassDefaultObject);
		if (!Settings)
		{
			return nullptr;
		}

		const uint8* Entries = *reinterpret_cast<const uint8* const*>(Settings + Offset_ClassSettings);
		const int32 Num = *reinterpret_cast<const int32*>(Settings + Offset_ClassSettings + 8);

		if (!Entries || Num <= 0)
		{
			return nullptr;
		}

		OutNum = Num;
		return Entries;
	}

	static const uint8* GetEntry(const uint8* Entries, int32 Index)
	{
		return Entries + (Index * EntryStride);
	}

	/**
	 * Only the fields whose offsets are pinned are copied across. Anything else in the shipping
	 * struct - the TFunction, the FastShared func name - is deliberately left at this module's
	 * defaults rather than guessed at.
	 */
	static FClassReplicationInfo ReadClassRepInfo(const uint8* Entry)
	{
		const uint8* Info = Entry + Offset_ClassRepInfo;

		FClassReplicationInfo Result;
		Result.DistancePriorityScale = *reinterpret_cast<const float*>(Info + Info_DistancePriorityScale);
		Result.StarvationPriorityScale = *reinterpret_cast<const float*>(Info + Info_StarvationPriorityScale);
		Result.AccumulatedNetPriorityBias = *reinterpret_cast<const float*>(Info + Info_AccumulatedNetPriorityBias);
		Result.ReplicationPeriodFrame = *reinterpret_cast<const uint16*>(Info + Info_ReplicationPeriodFrame);
		Result.FastPath_ReplicationPeriodFrame = *reinterpret_cast<const uint16*>(Info + Info_FastPathPeriodFrame);
		Result.ActorChannelFrameTimeout = *reinterpret_cast<const uint16*>(Info + Info_ActorChannelFrameTimeout);
		Result.SetCullDistanceSquared(*reinterpret_cast<const float*>(Info + Info_CullDistanceSquared));

		return Result;
	}

	/**
	 * Resolves every entry's class, in a single pass over the object table.
	 *
	 * Entries name classes as "Package.Class". UObject::FindClass walks the whole table per
	 * lookup and this game has millions of objects, so 82 of those is not affordable at startup;
	 * one pass matching on the cheap short name is. GetFullName only runs on a short-name hit,
	 * which is what rejects a same-named class from the wrong package.
	 *
	 * OutClasses is parallel to the entry array; unresolved entries are left null.
	 */
	static void ResolveClasses(const uint8* Entries, int32 Num, TArray<UClass*>& OutClasses)
	{
		TArray<std::string> ShortNames;

		OutClasses.Reset();

		for (int32 Index = 0; Index < Num; ++Index)
		{
			const std::string FullName = reinterpret_cast<const FName*>(GetEntry(Entries, Index) + Offset_ActorClassName)->ToString();
			const size_t Dot = FullName.rfind('.');

			ShortNames.Add(Dot == std::string::npos ? FullName : FullName.substr(Dot + 1));
			OutClasses.Add(nullptr);
		}

		static UClass* const ClassClass = UClass::StaticClass();

		const int32 NumObjects = UObject::GObjects->Num();
		for (int32 ObjectIndex = 0; ObjectIndex < NumObjects; ++ObjectIndex)
		{
			UObject* Object = UObject::GObjects->GetByIndex(ObjectIndex);
			if (!Object || !Object->IsA(ClassClass))
			{
				continue;
			}

			const std::string ObjectName = Object->GetName();

			for (int32 Index = 0; Index < Num; ++Index)
			{
				if (OutClasses[Index] || ShortNames[Index] != ObjectName)
				{
					continue;
				}

				// "Class /Script/Engine.Actor" has to end with the entry's "Engine.Actor".
				const std::string EntryName = reinterpret_cast<const FName*>(GetEntry(Entries, Index) + Offset_ActorClassName)->ToString();
				const std::string ObjectFullName = Object->GetFullName();

				if (ObjectFullName.size() >= EntryName.size()
					&& ObjectFullName.compare(ObjectFullName.size() - EntryName.size(), EntryName.size(), EntryName) == 0)
				{
					OutClasses[Index] = static_cast<UClass*>(Object);
				}
			}
		}
	}
}

static const char* ClassRepNodeMappingToString(uint8 Mapping)
{
	switch (static_cast<EClassRepNodeMapping>(Mapping))
	{
	case EClassRepNodeMapping::NotRouted:                   return "NotRouted";
	case EClassRepNodeMapping::RelevantAllConnections:      return "RelevantAllConnections";
	case EClassRepNodeMapping::RelevantAllInsideFortVolume: return "RelevantAllInsideFortVolume";
	case EClassRepNodeMapping::Custom:                      return "Custom";
	case EClassRepNodeMapping::Spatialize_Static:           return "Spatialize_Static";
	case EClassRepNodeMapping::Spatialize_Dynamic:          return "Spatialize_Dynamic";
	case EClassRepNodeMapping::Spatialize_Dormancy:         return "Spatialize_Dormancy";
	case EClassRepNodeMapping::Instance_Dynamic:            return "Instance_Dynamic";
	default:                                                return "Unknown";
	}
}

static void DumpGlobalRepGraphActorClassSettings()
{
	using namespace GlobalRepGraphClassSettings;

	int32 Num = 0;
	const uint8* Entries = GetEntries(Num);

	if (!Entries)
	{
		// ClassSettings is a Config property on a plain UObject, so it is only populated once
		// something has loaded the ini into this CDO. Empty here means that never happened.
		UE_LOG(LogFortReplicationGraph, Warning, "GlobalClassSettings: no entries - config never reached the CDO.");
		return;
	}

	UE_LOG(LogFortReplicationGraph, Display, "GlobalClassSettings: %d entries.", Num);

	for (int32 Index = 0; Index < Num; ++Index)
	{
		const uint8* Entry = GetEntry(Entries, Index);

		const uint8 Flags = Entry[0];
		const uint8 Mapping = Entry[1];

		const FName ActorClassName = *reinterpret_cast<const FName*>(Entry + Offset_ActorClassName);
		const FName MapToTemplate = *reinterpret_cast<const FName*>(Entry + Offset_MapToTemplate);

		const uint8* Info = Entry + Offset_ClassRepInfo;

		UE_LOG(LogFortReplicationGraph, Display,
			"GlobalClassSettings[%3d] %-52s Mapping=%u(%s) Flags=0x%02X [RepInfoToMap=%d CustomRepInfo=%d MulticastOpenChannel=%d(%d)] Template=%s",
			Index,
			ActorClassName.ToString().c_str(),
			Mapping,
			ClassRepNodeMappingToString(Mapping),
			Flags,
			(Flags >> 0) & 1,
			(Flags >> 1) & 1,
			(Flags >> 4) & 1,
			(Flags >> 5) & 1,
			MapToTemplate.ToString().c_str());

		if (((Flags >> 1) & 1) != 0)
		{
			// Only meaningful when bUseCustomClassRepInfo is set - that is the only bit that
			// makes the shipped function read this block.
			UE_LOG(LogFortReplicationGraph, Display,
				"    RepInfo DistScale=%.2f StarvScale=%.2f NetPriBias=%.2f Period=%u FastPathPeriod=%u ChannelTimeout=%u CullDistSq=%.0f",
				*reinterpret_cast<const float*>(Info + Info_DistancePriorityScale),
				*reinterpret_cast<const float*>(Info + Info_StarvationPriorityScale),
				*reinterpret_cast<const float*>(Info + Info_AccumulatedNetPriorityBias),
				*reinterpret_cast<const uint16*>(Info + Info_ReplicationPeriodFrame),
				*reinterpret_cast<const uint16*>(Info + Info_FastPathPeriodFrame),
				*reinterpret_cast<const uint16*>(Info + Info_ActorChannelFrameTimeout),
				*reinterpret_cast<const float*>(Info + Info_CullDistanceSquared));

			static const char HexDigits[] = "0123456789ABCDEF";

			std::string Hex;
			Hex.reserve(ClassRepInfoSize * 3);
			for (int32 ByteIndex = 0; ByteIndex < ClassRepInfoSize; ++ByteIndex)
			{
				Hex += HexDigits[Info[ByteIndex] >> 4];
				Hex += HexDigits[Info[ByteIndex] & 0xF];
				Hex += ' ';
			}

			UE_LOG(LogFortReplicationGraph, Display, "    RepInfo raw: %s", Hex.c_str());
		}
	}
}

void UFortReplicationGraph::InitGlobalActorClassSettings()
{
	UReplicationGraph::InitGlobalActorClassSettings();

	if (FortRepGraph::LogGlobalClassSettings != 0)
	{
		DumpGlobalRepGraphActorClassSettings();
	}

	// Classes that are not loaded yet get their policy and settings worked out the first
	// time the graph is asked about them.
	GetGlobalActorReplicationInfoMap().SetInitClassInfoFunc(
		[this](UClass* Class, FClassReplicationInfo& ClassInfo)
		{
			RegisterClassRepNodeMapping(Class); // Has to run before ConditionalInitClassReplicationInfo.

			const bool bHandled = ConditionalInitClassReplicationInfo(Class, ClassInfo);

			if (FortRepGraph::LogLazyInitClasses != 0)
			{
				UE_LOG(LogFortReplicationGraph, Warning, "%s was Lazy Initialized. (Parent: %s) %d.",
					Class->GetName().c_str(),
					Class->SuperStruct ? Class->SuperStruct->GetName().c_str() : "None",
					static_cast<int32>(bHandled ? ClassRepNodePolicies.GetChecked(Class) : EClassRepNodeMapping::NotRouted));
			}

			return bHandled;
		});

	ClassRepNodePolicies.InitNewElement = [this](UClass* Class, EClassRepNodeMapping& NodeMapping)
	{
		NodeMapping = GetClassNodeMapping(Class);
		return true;
	};

	// -----------------------------------------------------------------------------------
	//	Class policies
	//
	//	These come from UGlobalRepGraphActorClassSettings::ClassSettings, the same array the
	//	shipped UFortReplicationGraph reads. The hand-written list below is only a fallback for
	//	the case where that config never reached the CDO - it is a guess at this table and is
	//	known to be both incomplete (it covers 15 of the 82 entries) and wrong in places.
	// -----------------------------------------------------------------------------------

	auto SetClassPolicy = [this](const char* ClassName, EClassRepNodeMapping Mapping)
	{
		if (UClass* Class = UObject::FindClassFast(ClassName))
		{
			AddClassRepInfo(Class, Mapping);
		}
	};

	// Resolved once here and reused by the custom-rep-info and multicast passes further down,
	// which run at the points the shipped function runs them: after the class registration and
	// after this module's own SetClassInfo calls, so that the config wins over both.
	TArray<UClass*> ClassSettingClasses;

	int32 NumClassSettings = 0;
	const uint8* ClassSettingEntries = GlobalRepGraphClassSettings::GetEntries(NumClassSettings);

	if (ClassSettingEntries)
	{
		GlobalRepGraphClassSettings::ResolveClasses(ClassSettingEntries, NumClassSettings, ClassSettingClasses);

		int32 NumApplied = 0;
		int32 NumUnresolved = 0;

		for (int32 Index = 0; Index < NumClassSettings; ++Index)
		{
			const uint8* Entry = GlobalRepGraphClassSettings::GetEntry(ClassSettingEntries, Index);
			if ((Entry[0] & GlobalRepGraphClassSettings::AddClassRepInfoToMap) == 0)
			{
				continue;
			}

			UClass* Class = ClassSettingClasses[Index];
			if (!Class)
			{
				++NumUnresolved;

				UE_LOG(LogFortReplicationGraph, Warning, "GlobalClassSettings: could not resolve %s.",
					reinterpret_cast<const FName*>(Entry + GlobalRepGraphClassSettings::Offset_ActorClassName)->ToString().c_str());
				continue;
			}

			AddClassRepInfo(Class, static_cast<EClassRepNodeMapping>(Entry[1]));
			++NumApplied;
		}

		// The one policy the shipped function names in code rather than reading from the table.
		SetClassPolicy("FortSafeZoneIndicatorFuture", EClassRepNodeMapping::NotRouted);

		/*
		 * Deliberate divergence from the table, which marks this NotRouted.
		 *
		 * Epic can afford NotRouted because they have UFortReplicationGraphNode_SpecialRelevancy
		 * and UFortReplicationGraphNode_Custom to put it in; this module has neither, so NotRouted
		 * here means "never replicates to anyone". The reference server sends it to every
		 * connection (its classic path has no class policy at all), and matching that is closer to
		 * correct than dropping it. AFortGameStateAthena::BroadcastSpectatorInfo is a replicated
		 * pointer, so without this the client's copy is null.
		 */
		SetClassPolicy("FortBroadcastSpectatorInfo", EClassRepNodeMapping::RelevantAllConnections);

		UE_LOG(LogFortReplicationGraph, Display, "GlobalClassSettings: applied %d node mappings (%d unresolved).",
			NumApplied, NumUnresolved);
	}
	else
	{
		UE_LOG(LogFortReplicationGraph, Warning,
			"GlobalClassSettings: unavailable - falling back to the hand-written class policies, which are incomplete.");

	// Nothing is routed unless something below says otherwise.
	SetClassPolicy("Actor", EClassRepNodeMapping::NotRouted);

	// Not needed in Fortnite, and not worth a channel per level.
	SetClassPolicy("LevelScriptActor", EClassRepNodeMapping::NotRouted);

	// The graph replicates its own debug actor as a special case.
	SetClassPolicy("ReplicationGraphDebugActor", EClassRepNodeMapping::NotRouted);

	// Player states are handled entirely by the frequency limiter and the per-connection node.
	SetClassPolicy("PlayerState", EClassRepNodeMapping::NotRouted);

	// Controllers only ever matter to their own connection, which the per-connection node covers.
	SetClassPolicy("PlayerController", EClassRepNodeMapping::NotRouted);

	// Inventories, quickbars and the like hang off the controller and are gathered with it.
	SetClassPolicy("FortInventory", EClassRepNodeMapping::NotRouted);
	SetClassPolicy("FortQuickBars", EClassRepNodeMapping::NotRouted);

	// Weapons and parachutes never enter the graph on their own: they are replicated as
	// dependent actors of the pawn that owns them. See OnPawnWeaponChange.
	SetClassPolicy("FortWeapon", EClassRepNodeMapping::NotRouted);
	SetClassPolicy("FortPlayerParachute", EClassRepNodeMapping::NotRouted);

	// Non spatialized, relevant to all: AInfo covers the game state, the game session,
	// the safe zone indicator and everything else that has no position in the world.
	SetClassPolicy("Info", EClassRepNodeMapping::RelevantAllConnections);

	// Game state and its info actors are relevant to everyone, everywhere.
	SetClassPolicy("FortGameStateAthena", EClassRepNodeMapping::RelevantAllConnections);
	SetClassPolicy("FortTeamPrivateInfo", EClassRepNodeMapping::NotRouted);

	// Building actors do not move and go dormant once they settle, so they are treated as
	// static while dormant and dynamic while awake.
	SetClassPolicy("BuildingSMActor", EClassRepNodeMapping::Spatialize_Dormancy);
	SetClassPolicy("BuildingContainer", EClassRepNodeMapping::Spatialize_Dormancy);
	SetClassPolicy("BuildingGameplayActor", EClassRepNodeMapping::Spatialize_Dormancy);

	// Pickups are spawned, sit still, and are picked up. Dormancy driven for the same reason.
	SetClassPolicy("FortPickup", EClassRepNodeMapping::Spatialize_Dormancy);

	// Pawns, vehicles and projectiles move every frame.
	SetClassPolicy("FortPlayerPawnAthena", EClassRepNodeMapping::Spatialize_Dynamic);
	SetClassPolicy("FortAthenaVehicle", EClassRepNodeMapping::Spatialize_Dynamic);
	SetClassPolicy("FortProjectileBase", EClassRepNodeMapping::Spatialize_Dynamic);
	}

	// -----------------------------------------------------------------------------------
	//	Register every currently loaded replicated class
	// -----------------------------------------------------------------------------------

	/*
	 * One pass over the object table, which in this game has millions of entries, so the tests
	 * are ordered cheapest-and-most-selective first: nearly every object fails IsA(UClass), and
	 * of the classes that survive, most are not actors. The class name - which builds a
	 * std::string - is only asked for once a class has already proved to be a replicated actor.
	 */
	static UClass* const ClassClass = UClass::StaticClass();
	static UClass* const ActorClass = AActor::StaticClass();

	auto ShouldSpatialize = [](const AActor* CDO)
	{
		return CDO->GetIsReplicated() && !(CDO->bAlwaysRelevant || CDO->bOnlyRelevantToOwner || CDO->bNetUseOwnerRelevancy);
	};

	TArray<UClass*> AllReplicatedClasses;

	const int32 NumObjects = UObject::GObjects->Num();
	for (int32 Index = 0; Index < NumObjects; ++Index)
	{
		UObject* Object = UObject::GObjects->GetByIndex(Index);
		if (!Object || !Object->IsA(ClassClass))
		{
			continue;
		}

		UClass* Class = static_cast<UClass*>(Object);
		if (!Class->IsSubclassOf(ActorClass))
		{
			continue;
		}

		AActor* ActorCDO = ToActor(static_cast<SDK::AActor*>(Class->ClassDefaultObject));
		if (!ActorCDO || !ActorCDO->GetIsReplicated())
		{
			continue;
		}

		// Skip SKEL and REINST classes; they are editor scaffolding and never replicate.
		const std::string ClassName = Class->GetName();
		if (ClassName.rfind("SKEL_", 0) == 0 || ClassName.rfind("REINST_", 0) == 0)
		{
			continue;
		}

		AllReplicatedClasses.Add(Class);

		// A class that stops spatializing where its parent did wants to be recorded: the
		// routing is otherwise inherited silently and is a common source of surprise.
		if (UClass* SuperClass = static_cast<UClass*>(Class->SuperStruct))
		{
			if (AActor* SuperCDO = ToActor(static_cast<SDK::AActor*>(SuperClass->ClassDefaultObject)))
			{
				if (ShouldSpatialize(ActorCDO) == false && ShouldSpatialize(SuperCDO) == true)
				{
					NonSpatializedChildClasses.Add(Class);
				}
			}
		}

		RegisterClassRepNodeMapping(Class);
	}

	UE_LOG(LogFortReplicationGraph, Display, "Registered %d replicated classes (%d stopped spatializing where their parent did).",
		AllReplicatedClasses.Num(), NonSpatializedChildClasses.Num());

	// -----------------------------------------------------------------------------------
	//	FClassReplicationInfo: the per-class replication settings. Some are set explicitly
	//	here, the rest are derived from the legacy AActor settings.
	// -----------------------------------------------------------------------------------

	auto SetClassInfo = [&](UClass* Class, const FClassReplicationInfo& Info)
	{
		if (Class)
		{
			GetGlobalActorReplicationInfoMap().SetClassInfo(Class, Info);
			ExplicitlySetClasses.Add(Class);
		}
	};

	ExplicitlySetClasses.Reset();

	if (UClass* PawnClass = UObject::FindClassFast("FortPlayerPawn"))
	{
		FClassReplicationInfo PawnClassRepInfo;
		PawnClassRepInfo.DistancePriorityScale = 1.f;
		PawnClassRepInfo.StarvationPriorityScale = 1.f;
		PawnClassRepInfo.ActorChannelFrameTimeout = 4;

		if (AActor* PawnCDO = ToActor(static_cast<SDK::AActor*>(PawnClass->ClassDefaultObject)))
		{
			PawnClassRepInfo.SetCullDistanceSquared(PawnCDO->GetNetCullDistanceSquared());
		}

		/*
		 * FastSharedReplicationFunc is left null on purpose, even though FastShared replication is
		 * now running. The two are less related than the name suggests.
		 *
		 * That field means one specific thing to the graph: "this actor's movement is delivered by
		 * UReplicationGraph::ReplicateSingleActor_FastShared, via a shared FOutBunch trapped out of
		 * the actor's multicast". Setting it has two consequences, and neither is wanted here:
		 *
		 *   - UReplicationGraphNode_DynamicSpatialFrequency switches the pawn to its ZoneSettings
		 *     table, which is deliberately slow (1 Hz behind the viewer) because the shared-bunch
		 *     path is assumed to be underneath it carrying position at 10-20 Hz.
		 *   - ReplicateActorListsForConnections_FastShared starts running, and this module's
		 *     version of it re-calls UActorChannel::ReplicateActor rather than sending a shared
		 *     bunch, which is not the same thing at all.
		 *
		 * DriveFastSharedReplication delivers the movement a different way - by letting the pawn's
		 * own FastSharedReplication multicast go out through the engine - so the shared-bunch path
		 * stays switched off and the pawn keeps ZoneSettings_NonFastSharedActors, which is the
		 * table meant for a default path that carries its own weight.
		 *
		 * The upshot is that a pawn's movement arrives twice over: full property replication at
		 * 5-20 Hz on the default path, and the compact movement multicast at the FastShared
		 * cadence. That is more traffic than Epic would send for the same result, and it is the
		 * honest trade for not reproducing the shared-bunch machinery. If that machinery is ever
		 * built, set the func here and both consequences above become correct rather than unwanted.
		 *
		 * The name is still set, so the intent stays greppable.
		 */
		PawnClassRepInfo.FastSharedReplicationFuncName = FName(L"FastSharedReplication");

		SetClassInfo(PawnClass, PawnClassRepInfo);
	}

	if (UClass* PlayerStateClass = UObject::FindClassFast("PlayerState"))
	{
		FClassReplicationInfo PlayerStateRepInfo;

		// A player state has no position of its own, so distance must not enter its priority:
		// GlobalActorReplicationInfo::WorldLocation for one is the origin, and scaling by the
		// distance from the origin to the viewer would push it behind every nearby world actor.
		PlayerStateRepInfo.DistancePriorityScale = 0.f;
		PlayerStateRepInfo.ActorChannelFrameTimeout = 0;

		SetClassInfo(PlayerStateClass, PlayerStateRepInfo);
	}

	if (UClass* PlayerControllerClass = UObject::FindClassFast("PlayerController"))
	{
		FClassReplicationInfo PlayerControllerRepInfo;

		// Same reasoning as the player state, and then some: the owning connection's
		// controller carries the handshake the client needs before it will leave the loading
		// screen (bHasServerFinishedLoading), so it must never be starved behind the world.
		// The negative bias puts it at the head of the prioritized list unconditionally.
		PlayerControllerRepInfo.DistancePriorityScale = 0.f;
		PlayerControllerRepInfo.StarvationPriorityScale = 0.f;
		PlayerControllerRepInfo.AccumulatedNetPriorityBias = -10.f;
		PlayerControllerRepInfo.ActorChannelFrameTimeout = 0;

		SetClassInfo(PlayerControllerClass, PlayerControllerRepInfo);
	}

	if (UClass* GameStateClass = UObject::FindClassFast("GameStateBase"))
	{
		FClassReplicationInfo GameStateRepInfo;

		// The game state drives match state, the playlist and the game phase on the client.
		// It is one actor per match; there is no reason for it to ever wait behind buildings.
		GameStateRepInfo.DistancePriorityScale = 0.f;
		GameStateRepInfo.StarvationPriorityScale = 0.f;
		GameStateRepInfo.AccumulatedNetPriorityBias = -10.f;
		GameStateRepInfo.ActorChannelFrameTimeout = 0;

		SetClassInfo(GameStateClass, GameStateRepInfo);
	}

	/*
	 * The config's own FClassReplicationInfo entries, applied last so they win over the four
	 * above. This is where FortWeapon's ActorChannelFrameTimeout of 255 and PlayerState's
	 * DistancePriorityScale of 0 actually come from - the values above were reconstructed by hand
	 * and happen to agree with the table for PlayerState.
	 */
	if (ClassSettingEntries)
	{
		int32 NumApplied = 0;

		for (int32 Index = 0; Index < NumClassSettings; ++Index)
		{
			const uint8* Entry = GlobalRepGraphClassSettings::GetEntry(ClassSettingEntries, Index);
			if ((Entry[0] & GlobalRepGraphClassSettings::UseCustomClassRepInfo) == 0)
			{
				continue;
			}

			if (UClass* Class = ClassSettingClasses[Index])
			{
				SetClassInfo(Class, GlobalRepGraphClassSettings::ReadClassRepInfo(Entry));
				++NumApplied;
			}
		}

		UE_LOG(LogFortReplicationGraph, Display, "GlobalClassSettings: applied %d custom class replication infos.", NumApplied);
	}

	FastSharedPathConstants.MaxBitsPerFrame = FortRepGraph::TargetKBitsSecFastSharedPath / GetNetServerMaxTickRate();
	FastSharedPathConstants.DistanceRequirementPct = FortRepGraph::FastSharedPathCullDistPct;

	// ---------------------------------------------------------------------

	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.ListSize = 12;
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.NumBuckets = FortRepGraph::DynamicActorFrequencyBuckets;
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.BucketThresholds.Reset();
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.EnableFastPath = (FortRepGraph::EnableFastSharedPath > 0);
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.FastPathFrameModulo = FortRepGraph::FastSharedPathFrameModulo;

	UReplicationGraphNode_ConnectionDormancyNode::SetNumFramesUntilObsolete(static_cast<uint32>(FortRepGraph::ConnectionDormancy_NumFramesUntilObsolete));
	UReplicationGraphNode_DormancyNode::MaxZForConnection = static_cast<double>(FortRepGraph::MaxZDormantNode);

	// Set FClassReplicationInfo from the legacy settings on every replicated class.
	for (UClass* ReplicatedClass : AllReplicatedClasses)
	{
		RegisterClassReplicationInfo(ReplicatedClass);
	}

	DestructInfoMaxDistanceSquared = FortRepGraph::DestructionInfoMaxDist * FortRepGraph::DestructionInfoMaxDist;

	/*
	 * Which classes a multicast RPC may open a channel for.
	 *
	 * AActor defaults to *false* here, which is the opposite of the engine's own default that
	 * UReplicationGraph::InitGlobalActorClassSettings installs. Fortnite deliberately inverts it
	 * in config - Engine.Actor carries bAddToRPC_Multicast_OpenChannelForClassMap with the value
	 * bit clear - and then opts in the handful of classes that genuinely need it
	 * (BuildingActor, FortProjectileBase, FortStaticReplicatedActor, ...).
	 *
	 * With AActor left at true, any multicast on any actor a connection has no channel for forces
	 * one open on every connection, whether or not the graph considers that actor relevant.
	 *
	 * Controllers stay false regardless: opening a channel on a non-owner breaks that
	 * controller's replication.
	 */
	RPC_Multicast_OpenChannelForClass.Reset();
	RPC_Multicast_OpenChannelForClass.Set(AActor::StaticClass(), ClassSettingEntries == nullptr);
	RPC_Multicast_OpenChannelForClass.Set(AController::StaticClass(), false);

	if (ClassSettingEntries)
	{
		int32 NumApplied = 0;

		for (int32 Index = 0; Index < NumClassSettings; ++Index)
		{
			const uint8* Entry = GlobalRepGraphClassSettings::GetEntry(ClassSettingEntries, Index);
			if ((Entry[0] & GlobalRepGraphClassSettings::AddToMulticastMap) == 0)
			{
				continue;
			}

			if (UClass* Class = ClassSettingClasses[Index])
			{
				// AController is set after this loop rather than inside it, so nothing in the
				// table can turn controller channel-opening back on.
				if (Class != static_cast<UClass*>(AController::StaticClass()))
				{
					RPC_Multicast_OpenChannelForClass.Set(Class, (Entry[0] & GlobalRepGraphClassSettings::MulticastOpenChannel) != 0);
					++NumApplied;
				}
			}
		}

		RPC_Multicast_OpenChannelForClass.Set(AController::StaticClass(), false);

		UE_LOG(LogFortReplicationGraph, Display, "GlobalClassSettings: applied %d multicast channel-open policies.", NumApplied);
	}
}

void UFortReplicationGraph::InitGlobalGraphNodes()
{
	// -----------------------------------------------
	//	Spatial actors
	// -----------------------------------------------

	RootGridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
	RootGridNode->CellSize = FortRepGraph::CellSize;
	RootGridNode->SpatialBias = FVector2D(FortRepGraph::SpatialBiasX, FortRepGraph::SpatialBiasY);

	if (FortRepGraph::DisableSpatialRebuilds)
	{
		// Nothing may force a rebuild: an actor found outside the grid is clamped into it.
		RootGridNode->AddToClassRebuildDenyList(AActor::StaticClass());
	}

	if (FortRepGraph::DSF_Enabled)
	{
		// Dynamic actors in each cell get frequency by distance rather than a fixed bucket rotation.
		RootGridNode->CreateCellNodeOverride = [](UReplicationGraphNode_GridSpatialization2D* Parent) -> UReplicationGraphNode_GridCell*
		{
			UReplicationGraphNode_GridCell* CellNode = Parent->CreateChildNode<UReplicationGraphNode_GridCell>();

			CellNode->CreateDynamicNodeOverride = [](UReplicationGraphNode_GridCell* Cell) -> UReplicationGraphNode*
			{
				return Cell->CreateChildNode<UReplicationGraphNode_DynamicSpatialFrequency>();
			};

			return CellNode;
		};
	}

	AddGlobalGraphNode(RootGridNode);

	// -----------------------------------------------
	//	Always relevant (to everyone) actors
	// -----------------------------------------------

	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);

	// -----------------------------------------------
	//	Live spectators
	// -----------------------------------------------

	LiveSpectatorRelevancyNode = CreateNewNode<UFortReplicationGraphNode_LiveSpectator>();
	AddGlobalGraphNode(LiveSpectatorRelevancyNode);

	// -----------------------------------------------
	//	Player states: a rolling subset per frame
	// -----------------------------------------------

	PlayerStateLimiterNode = CreateNewNode<UFortReplicationGraphNode_PlayerStateFrequencyLimiter>();
	AddGlobalGraphNode(PlayerStateLimiterNode);

	ReplayPlayerStateLimiterNode = CreateNewNode<UFortReplicationGraphNode_PlayerStateFrequencyLimiter>();
	ReplayPlayerStateLimiterNode->TargetActorsPerFrame = FortRepGraph::Replay_MaxPlayerStatesPerFrame;
	AddGlobalGraphNode(ReplayPlayerStateLimiterNode);

	// The playlist dependent values need to hold before a playlist has ever loaded.
	OnPlaylistDataLoaded(nullptr);
}

void UFortReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* RepGraphConnection)
{
	UReplicationGraph::InitConnectionGraphNodes(RepGraphConnection);

	UFortReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = CreateNewNode<UFortReplicationGraphNode_AlwaysRelevant_ForConnection>();
	AlwaysRelevantConnectionNode->FortGraph = this;

	// This node has to know when a client's streaming levels come and go, so that the
	// always relevant actors that live in those levels are held back until the client has them.
	RepGraphConnection->OnClientVisibleLevelNameAdd.AddRaw(AlwaysRelevantConnectionNode, &UFortReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityAdd);
	RepGraphConnection->OnClientVisibleLevelNameRemove.AddRaw(AlwaysRelevantConnectionNode, &UFortReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityRemove);

	AddConnectionGraphNode(AlwaysRelevantConnectionNode, RepGraphConnection);
}

EClassRepNodeMapping UFortReplicationGraph::GetMappingPolicy(UClass* Class)
{
	EClassRepNodeMapping* PolicyPtr = ClassRepNodePolicies.Get(Class);
	return PolicyPtr ? *PolicyPtr : EClassRepNodeMapping::NotRouted;
}

EClassRepNodeMapping UFortReplicationGraph::GetInstanceMappingPolicy(const AActor* Actor) const
{
	// Recovered from the one routing helper that survived compilation into the client:
	// the actor is asked, through IFortReplicationGraphInterface, which node it wants,
	// and the answer is mapped onto EClassRepNodeMapping.
	static UClass* const FortReplicationGraphInterface = UObject::FindClassFast("FortReplicationGraphInterface");

	const bool bImplementsInterface = Actor != nullptr
		&& FortReplicationGraphInterface != nullptr
		&& Actor->Class->IsSubclassOf(FortReplicationGraphInterface);

	/*
	 * CANNOT BE RECREATED. The interface's routing accessor returns 1..4, which the shipped
	 * routing helper maps onto RelevantAllInsideFortVolume / Spatialize_Static /
	 * Spatialize_Dynamic / Spatialize_Dormancy, but the accessor itself is not exposed by the
	 * SDK and its implementations were compiled out. Anything else the shipped code treats as
	 * unknown and does not route, which is what happens here for every instance.
	 *
	 * Nothing currently maps a class to Instance_Dynamic, so this is unreachable in practice;
	 * it is kept so that the enum is handled exhaustively and so the gap is recorded where it
	 * would matter rather than in a comment somewhere else.
	 */
	UE_LOG(LogFortReplicationGraph, Warning, "UFortReplicationGraph %s instance dynamic routing for Actor %s. Not Routing",
		bImplementsInterface ? "unknown" : "unable to determine",
		GetActorRepListTypeDebugString(const_cast<AActor*>(Actor)).c_str());

	return EClassRepNodeMapping::NotRouted;
}

void UFortReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
	// Offered to the spectator node whatever the routing policy says: these actors are already
	// routed for the players, and the node takes only the classes a broadcast has to show.
	if (LiveSpectatorRelevancyNode)
	{
		LiveSpectatorRelevancyNode->NotifyAddNetworkActor(ActorInfo);
	}

	// Pawns carry weapons and a parachute that never enter the graph on their own.
	if (IsFortPawnClass(ActorInfo.Class))
	{
		TrackedPawns.AddUnique(ActorInfo.Actor);
	}

	EClassRepNodeMapping Policy = GetMappingPolicy(ActorInfo.Class);

	if (Policy == EClassRepNodeMapping::Instance_Dynamic)
	{
		Policy = GetInstanceMappingPolicy(ActorInfo.Actor);
	}

	switch (Policy)
	{
		case EClassRepNodeMapping::NotRouted:
		{
			/*
			 * Diagnostic for "the reference server sends this and we do not".
			 *
			 * The classic UNetDriver::ServerReplicateActors the reference server runs walks the
			 * whole network object list and asks AActor::IsNetRelevantFor per actor. The graph
			 * asks no such question: an actor whose policy lands here is simply never offered to
			 * any node, so it can never replicate. This is therefore the exact set the graph
			 * drops that the classic path would have considered.
			 *
			 * Plenty of entries here are correct - PlayerController, PlayerState, FortInventory,
			 * FortWeapon, FortQuickBars, FortTeamPrivateInfo and FortPlayerParachute are all
			 * re-added by UFortReplicationGraphNode_AlwaysRelevant_ForConnection, the player state
			 * limiter, or as pawn dependent actors. Anything else in the list is a real drop.
			 *
			 * One class is logged once. The static survives a world transition, which for a
			 * diagnostic is what you want: the second match does not re-log 60 lines.
			 */
			if (FortRepGraph::LogNotRoutedActors != 0 && ActorInfo.Class)
			{
				static TArray<UClass*> AlreadyLoggedNotRouted;

				if (!AlreadyLoggedNotRouted.Contains(ActorInfo.Class))
				{
					AlreadyLoggedNotRouted.Add(ActorInfo.Class);

					UE_LOG(LogFortReplicationGraph, Display, "NotRouted: %s (first instance %s)",
						ActorInfo.Class->GetName().c_str(),
						GetActorRepListTypeDebugString(ActorInfo.Actor).c_str());
				}
			}
			break;
		}

		case EClassRepNodeMapping::RelevantAllConnections:
		{
			if (ActorInfo.StreamingLevelName.IsNone())
			{
				AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
			}
			else
			{
				// Held per level: a connection only gets these once it has the level loaded.
				FActorRepListRefView& RepList = AlwaysRelevantStreamingLevelActors.FindOrAdd(ActorInfo.StreamingLevelName);
				RepList.ConditionalAdd(ActorInfo.Actor);

				// Once one of these wakes from dormancy the whole level's list has to be
				// offered to every connection again, otherwise the woken actor is never
				// gathered: the per-connection node drops a list whose actors are all dormant.
				//
				// Asked per object, not "is anything bound": a dormancy node and a grid cell
				// bind to this same event, so IsBound() is true for reasons that have nothing
				// to do with us and would leave the graph never subscribed at all.
				if (!GlobalInfo.Events.DormancyFlush.IsBoundToObject(this))
				{
					GlobalInfo.Events.DormancyFlush.AddRaw(this, &UFortReplicationGraph::OnAlwaysRelevantStreamingActorDormancyFlush);
				}
			}
			break;
		}

		case EClassRepNodeMapping::RelevantAllInsideFortVolume:
		{
			// Handled by the FortVolume grid the actor is inside. Without a volume there is
			// nothing to route to, so it falls back to being relevant to everyone.
			AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
			break;
		}

		case EClassRepNodeMapping::Custom:
		{
			if (UFortReplicationGraphNode_Custom** CustomNode = ClassToNodeToRouteTo.Find(FObjectKey(ActorInfo.Class)))
			{
				(*CustomNode)->NotifyAddNetworkActor(ActorInfo);
			}
			break;
		}

		case EClassRepNodeMapping::Spatialize_Static:
		{
			RootGridNode->AddActor_Static(ActorInfo, GlobalInfo);
			break;
		}

		case EClassRepNodeMapping::Spatialize_Dynamic:
		{
			RootGridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
			break;
		}

		case EClassRepNodeMapping::Spatialize_Dormancy:
		{
			RootGridNode->AddActor_Dormancy(ActorInfo, GlobalInfo);
			break;
		}

		default:
		{
			break;
		}
	}
}

void UFortReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	if (LiveSpectatorRelevancyNode)
	{
		LiveSpectatorRelevancyNode->NotifyRemoveNetworkActor(ActorInfo, false);
	}

	if (IsFortPawnClass(ActorInfo.Class))
	{
		TrackedPawns.RemoveSingleSwap(ActorInfo.Actor);
		TrackedPawnDependents.Remove(ActorInfo.Actor);
	}

	EClassRepNodeMapping Policy = GetMappingPolicy(ActorInfo.Class);

	if (Policy == EClassRepNodeMapping::Instance_Dynamic)
	{
		Policy = GetInstanceMappingPolicy(ActorInfo.Actor);
	}

	switch (Policy)
	{
		case EClassRepNodeMapping::NotRouted:
		{
			break;
		}

		case EClassRepNodeMapping::RelevantAllConnections:
		case EClassRepNodeMapping::RelevantAllInsideFortVolume:
		{
			if (ActorInfo.StreamingLevelName.IsNone())
			{
				AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
			}
			else if (FActorRepListRefView* RepList = AlwaysRelevantStreamingLevelActors.Find(ActorInfo.StreamingLevelName))
			{
				if (RepList->RemoveFast(ActorInfo.Actor) == false)
				{
					UE_LOG(LogFortReplicationGraph, Warning, "Actor %s was not found in AlwaysRelevantStreamingLevelActors list. LevelName: %s",
						GetActorRepListTypeDebugString(ActorInfo.Actor).c_str(), ActorInfo.StreamingLevelName.ToString().c_str());
				}
			}

			SetActorDestructionInfoToIgnoreDistanceCulling(ActorInfo.GetActor());
			break;
		}

		case EClassRepNodeMapping::Custom:
		{
			if (UFortReplicationGraphNode_Custom** CustomNode = ClassToNodeToRouteTo.Find(FObjectKey(ActorInfo.Class)))
			{
				(*CustomNode)->NotifyRemoveNetworkActor(ActorInfo);
			}
			break;
		}

		case EClassRepNodeMapping::Spatialize_Static:
		{
			RootGridNode->RemoveActor_Static(ActorInfo);
			break;
		}

		case EClassRepNodeMapping::Spatialize_Dynamic:
		{
			RootGridNode->RemoveActor_Dynamic(ActorInfo);
			break;
		}

		case EClassRepNodeMapping::Spatialize_Dormancy:
		{
			RootGridNode->RemoveActor_Dormancy(ActorInfo);
			break;
		}

		default:
		{
			break;
		}
	}
}

void UFortReplicationGraph::DriveFastSharedReplication()
{
	/*
	 * FastShared replication, done through the game's own multicast rather than through the
	 * engine's shared-bunch machinery.
	 *
	 * What Epic's version does: park a pointer to a shared FOutBunch on the graph, call the
	 * pawn's FastSharedReplication UFunction, trap that RPC inside
	 * UReplicationGraph::ProcessRemoteFunction, serialize its parameters into that one bunch, and
	 * then hand the same bytes to every connection in range. The win is that the movement is
	 * serialized once per frame instead of once per frame per connection.
	 *
	 * That trap is not reproducible here. It needs UNetDriver::NetCache and GetClassNetCache,
	 * GetFunctionRepLayout, FRepLayout::SendPropertiesForRPC, UActorChannel::WriteFieldHeaderAndPayload,
	 * UChannel::SendBunch, and the layout of FOutBunch and FNetBitWriter - six entry points and two
	 * non-reflected structs to recover in a stripped binary, each a chance to corrupt a send buffer.
	 *
	 * So only the optimisation is skipped, not the feature. AFortPlayerPawn::UpdateSharedReplication
	 * builds the FSharedRepMovement and fires the FastSharedReplication multicast itself; letting
	 * the engine deliver that multicast normally puts the same data on the same clients, just
	 * serialized per connection instead of once. The graph's relevancy still governs who receives
	 * it, because a multicast only reaches connections with an open channel for the actor.
	 *
	 * What matters for correctness is that pawn movement is carried by a fast path again, which is
	 * the assumption UReplicationGraphNode_DynamicSpatialFrequency's shipped zone tables are built
	 * on - the default schedule for a FastShared-capable actor is deliberately slow because this
	 * is meant to be underneath it.
	 *
	 * Cadence is Fortnite's own FastSharedPathFrameModulo. UpdateSharedReplication is additionally
	 * self-throttling: it only multicasts when the movement actually changed since last time.
	 */
	if (FortRepGraph::EnableFastSharedPath == 0)
	{
		return;
	}

	const int32 FrameModulo = FMath::Max(1, FortRepGraph::FastSharedPathFrameModulo);
	if ((GetReplicationGraphFrame() % static_cast<uint32>(FrameModulo)) != 0)
	{
		return;
	}

	/*
	 * AFortPlayerPawnAthena::UpdateSharedReplication. Called straight through rather than wrapped
	 * on Prism::AFortPlayerPawn, because every other use of the name AFortPlayerPawn in this file
	 * means the generated SDK class and pulling the extension header in would change all of them.
	 *
	 * It returns false when there is nothing to send at all - no authority, no root component, or
	 * the pawn is attached to something - and otherwise only fires the multicast when the movement
	 * differs from the last one sent. Both decisions are its own, which is what makes it safe to
	 * ask on every eligible frame.
	 */
	static auto UpdateSharedReplication = GetEngineFunction<bool (*)(SDK::AFortPlayerPawnAthena*)>(
		EngineAddresses::AFortPlayerPawnAthena_UpdateSharedReplication);

	// TrackedPawns holds every AFortPawn, AI included, and the recovered function is the Athena
	// override - it reads members well past AFortPlayerPawn - so the cast is to the class that
	// actually owns it rather than to the base.
	for (AActor* TrackedPawn : TrackedPawns)
	{
		if (!IsActorValidForReplication(TrackedPawn))
		{
			continue;
		}

		if (AFortPlayerPawnAthena* PlayerPawn = Cast<AFortPlayerPawnAthena>(static_cast<SDK::AActor*>(TrackedPawn)))
		{
			UpdateSharedReplication(PlayerPawn);
		}
	}
}

int32 UFortReplicationGraph::ServerReplicateActors(float DeltaSeconds)
{
	UpdatePawnDependentActors();

	// Before the gather, so that a movement multicast fired this frame is already queued on the
	// connections that are about to be serviced.
	DriveFastSharedReplication();

	return UReplicationGraph::ServerReplicateActors(DeltaSeconds);
}

void UFortReplicationGraph::UpdatePawnDependentActors()
{
	TArray<AActor*>& DesiredDependents = DesiredPawnDependents;

	for (int32 Index = TrackedPawns.Num() - 1; Index >= 0; --Index)
	{
		AActor* PawnActor = TrackedPawns[Index];

		if (!IsActorValidForReplication(PawnActor))
		{
			TrackedPawns.RemoveAtSwap(Index);
			TrackedPawnDependents.Remove(PawnActor);
			continue;
		}

		AFortPawn* FortPawn = Cast<AFortPawn>(PawnActor);
		if (!FortPawn)
		{
			TrackedPawns.RemoveAtSwap(Index);
			TrackedPawnDependents.Remove(PawnActor);
			continue;
		}

		DesiredDependents.Reset();

		// Every weapon the pawn is holding, not just the equipped one: the client needs the
		// others to exist before a swap, or the swap replicates a reference to nothing.
		for (AFortWeapon* Weapon : FortPawn->CurrentWeaponList)
		{
			if (Weapon)
			{
				DesiredDependents.AddUnique(ToActor(Weapon));
			}
		}

		if (AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(PawnActor))
		{
			if (AFortPlayerParachute* Parachute = PlayerPawn->ParachuteAttachment)
			{
				DesiredDependents.AddUnique(ToActor(Parachute));
			}

			// The vehicle a pawn is riding is deliberately not a dependent here. It is routed
			// to the grid in its own right and is at the same place as its driver, so a
			// dependency could only ever gather it a second time for the same connection.
		}

		TArray<AActor*>* PreviousDependents = TrackedPawnDependents.Find(PawnActor);

		// The common case by far: a pawn with nothing to carry, and nothing carried last frame.
		// Left out of the map entirely rather than holding an empty list for every pawn.
		if (DesiredDependents.Num() == 0 && (PreviousDependents == nullptr || PreviousDependents->Num() == 0))
		{
			continue;
		}

		if (PreviousDependents == nullptr)
		{
			PreviousDependents = &TrackedPawnDependents.Add(PawnActor);
		}

		for (AActor* Previous : *PreviousDependents)
		{
			if (!DesiredDependents.Contains(Previous))
			{
				GlobalActorReplicationInfoMap.RemoveDependentActor(PawnActor, Previous);
			}
		}

		for (AActor* Desired : DesiredDependents)
		{
			if (!PreviousDependents->Contains(Desired))
			{
				GlobalActorReplicationInfoMap.AddDependentActor(PawnActor, Desired);
			}
		}

		*PreviousDependents = DesiredDependents;
	}
}

// ------------------------------------------------------------------------------
// Notifications from game code
// ------------------------------------------------------------------------------

void UFortReplicationGraph::OnPawnWeaponChange(AActor* Pawn, AActor* NewWeapon, AActor* OldWeapon)
{
	if (!Pawn)
	{
		return;
	}

	if (OldWeapon)
	{
		GlobalActorReplicationInfoMap.RemoveDependentActor(Pawn, OldWeapon);
	}

	if (NewWeapon)
	{
		GlobalActorReplicationInfoMap.AddDependentActor(Pawn, NewWeapon);
	}
}

void UFortReplicationGraph::OnVehicleChanged(AActor* Pawn, AActor* NewVehicle, AActor* PrevVehicle)
{
	if (!Pawn)
	{
		return;
	}

	if (PrevVehicle)
	{
		GlobalActorReplicationInfoMap.RemoveDependentActor(Pawn, PrevVehicle);
	}

	if (NewVehicle)
	{
		GlobalActorReplicationInfoMap.AddDependentActor(Pawn, NewVehicle);
	}
}

void UFortReplicationGraph::OnParachuteStateChange(AActor* Pawn, bool bNewState)
{
	AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(Pawn);
	if (!PlayerPawn)
	{
		return;
	}

	AActor* Parachute = ToActor(PlayerPawn->ParachuteAttachment);
	if (!Parachute)
	{
		return;
	}

	if (bNewState)
	{
		GlobalActorReplicationInfoMap.AddDependentActor(Pawn, Parachute);
	}
	else
	{
		GlobalActorReplicationInfoMap.RemoveDependentActor(Pawn, Parachute);
	}
}

void UFortReplicationGraph::OnPlayerStateSquadAssign(AActor* PlayerState)
{
	AFortPlayerStateAthena* PS = Cast<AFortPlayerStateAthena>(PlayerState);
	if (!PS)
	{
		return;
	}

	// On the server a player state is owned by its controller, and the connection hangs
	// off that. The SDK exposes no GetNetConnection, so the chain is walked by hand.
	SDK::APlayerController* OwningController = Cast<SDK::APlayerController>(PS->Owner);
	if (!OwningController)
	{
		return;
	}

	UNetConnection* NetConnection = static_cast<UNetConnection*>(OwningController->NetConnection);
	if (!NetConnection)
	{
		return;
	}

	UFortReplicationGraphNode_AlwaysRelevantForSquad* SquadNode = GetNodeForSquad(PS->SquadId);
	SquadNode->PlayerStates.AddUnique(PlayerState);

	AddConnectionGraphNode(SquadNode, NetConnection);
}

void UFortReplicationGraph::OnPlayerPawnHighFrequencyChange(AActor* Pawn, bool bHighFreq)
{
	if (!Pawn || !AlwaysRelevantNode)
	{
		return;
	}

	FNewReplicatedActorInfo ActorInfo(Pawn);

	if (bHighFreq)
	{
		AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
	}
	else
	{
		AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo, false);
	}
}

void UFortReplicationGraph::OnPlaylistDataLoaded(AActor* GameState)
{
	bool bIsLargeTeamGame = false;

	if (AFortGameStateAthena* GSA = Cast<AFortGameStateAthena>(GameState))
	{
		bIsLargeTeamGame = GSA->bIsLargeTeamGame;
	}

	GlobalActorChannelFrameNumTimeout = FortRepGraph::DynamicActorFrequencyBuckets * 2;

	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.NumBuckets = FortRepGraph::DynamicActorFrequencyBuckets;
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.BucketThresholds.Reset();
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.EnableFastPath = (FortRepGraph::EnableFastSharedPath > 0);

	RPCSendPolicyMap.Reset();

	if (bIsLargeTeamGame)
	{
		// With a lot of pawns in one cell their frequency is throttled harder the more
		// of them share it, rather than every pawn paying the same fixed period.
		const int32 BigTeamBuckets = FortRepGraph::DynamicActorFrequencyBuckets_BigTeamMode;

		UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.BucketThresholds.Emplace(50, BigTeamBuckets);
		UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.BucketThresholds.Emplace(80, BigTeamBuckets + 1);
		UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.BucketThresholds.Emplace(999, BigTeamBuckets + 2);

		GlobalActorChannelFrameNumTimeout = BigTeamBuckets + 2;
	}

	if (!bIsLargeTeamGame || FortRepGraph::InstantFlush_LargeTeamMode > 0)
	{
		// Damage cues are the one multicast worth flushing immediately: held to the next
		// replication frame they arrive after the hit they are meant to describe.
		if (UClass* PawnClass = UObject::FindClassFast("FortPlayerPawnAthena"))
		{
			if (UFunction* DamageCue = PawnClass->GetFunction("FortPlayerPawnAthena", "NetMultiCast_Athena_BatchedDamageCue"))
			{
				RPCSendPolicyMap.Add(FObjectKey(DamageCue), FRPCSendPolicyInfo(true));
			}
		}
	}
}

void UFortReplicationGraph::OnCullDistSqChange(AActor* Actor, float DistSq)
{
	if (!IsActorValidForReplicationGather(Actor))
	{
		return;
	}

	// An always relevant actor is allowed to change its cull distance for rendering
	// reasons - supply drops do - without that meaning anything to the graph.
	if (IsSpatialized(GetMappingPolicy(Actor->Class)) == false)
	{
		return;
	}

	bool bWasCreated = false;
	FGlobalActorReplicationInfo& ActorInfo = GlobalActorReplicationInfoMap.Get(Actor, bWasCreated);

	const float OldDistSq = ActorInfo.Settings.GetCullDistanceSquared();
	if (FMath::IsNearlyZero(OldDistSq - DistSq))
	{
		return;
	}

	ActorInfo.Settings.SetCullDistanceSquared(DistSq);

	if (bWasCreated == false)
	{
		// The actor was already known, so every connection carries a stale copy of the
		// old distance and the grid has it filed under the old cell footprint.
		for (UNetReplicationGraphConnection* Connection : Connections)
		{
			if (FConnectionReplicationActorInfo* ConnectionActorInfo = Connection->ActorInfoMap.Find(Actor))
			{
				ConnectionActorInfo->SetCullDistanceSquared(DistSq);
			}
		}

		if (RootGridNode)
		{
			RootGridNode->NotifyActorCullDistChange(Actor, ActorInfo, OldDistSq);
		}
	}
}

void UFortReplicationGraph::OnPlayerPawnDistSqChange(AActor* Pawn, float DistSq)
{
	OnCullDistSqChange(Pawn, DistSq);
}

void UFortReplicationGraph::OnBuildingDistSqChange(AActor* Building, float Dist)
{
	// Buildings report a distance, not a squared one, and are given a little slack so a
	// client walking towards one has it before it is in view.
	OnCullDistSqChange(Building, FMath::Square(Dist + 1000.f));
}

void UFortReplicationGraph::OnPickupCullDistanceChanged(AActor* Pickup, float DistSq)
{
	OnCullDistSqChange(Pickup, DistSq);
}

void UFortReplicationGraph::OnAlwaysRelevantStreamingActorDormancyFlush(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo)
{
	FNewReplicatedActorInfo ActorInfo(Actor);

	if (ActorInfo.StreamingLevelName.IsNone())
	{
		return;
	}

	// Offer the level's list to every connection again, so the actor that just woke is
	// gathered instead of being skipped along with its still dormant neighbours.
	for (UNetReplicationGraphConnection* ConnManager : Connections)
	{
		for (UReplicationGraphNode* ConnectionNode : ConnManager->GetConnectionGraphNodes())
		{
			if (UFortReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = Cast<UFortReplicationGraphNode_AlwaysRelevant_ForConnection>(ConnectionNode))
			{
				AlwaysRelevantConnectionNode->FlushStreamingAlwaysRelevantList(ActorInfo);
			}
		}
	}
}

UFortReplicationGraphNode_AlwaysRelevantForSquad* UFortReplicationGraph::GetNodeForSquad(uint8 SquadId)
{
	UFortReplicationGraphNode_AlwaysRelevantForSquad** Existing = SquadMap.Find(SquadId);
	if (Existing && *Existing)
	{
		return *Existing;
	}

	UFortReplicationGraphNode_AlwaysRelevantForSquad* SquadNode = CreateNewNode<UFortReplicationGraphNode_AlwaysRelevantForSquad>();
	SquadNode->SquadID = SquadId;
	SquadMap.Add(SquadId, SquadNode);

	return SquadNode;
}

void UFortReplicationGraph::PrintRepNodePolicies()
{
	UE_LOG(LogFortReplicationGraph, Display, "====================================");
	UE_LOG(LogFortReplicationGraph, Display, "Fortnite Replication Routing Policies");
	UE_LOG(LogFortReplicationGraph, Display, "====================================");

	for (auto It = ClassRepNodePolicies.CreateIterator(); It; ++It)
	{
		UObject* Class = It.Key().ResolveObjectPtr();
		UE_LOG(LogFortReplicationGraph, Display, "%-40s --> %d", GetNameSafe(Class).c_str(), static_cast<int32>(It.Value()));
	}
}

// ------------------------------------------------------------------------------
// UFortReplicationGraphNode_AlwaysRelevant_ForConnection
// ------------------------------------------------------------------------------

void UFortReplicationGraphNode_AlwaysRelevant_ForConnection::ResetGameWorldState()
{
	ReplicationActorList.Reset();
	AlwaysRelevantStreamingLevelsNeedingReplication.Empty();
}

void UFortReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	ReplicationActorList.Reset();

	for (const FNetViewer& CurViewer : Params.Viewers)
	{
		ReplicationActorList.ConditionalAdd(CurViewer.InViewer);

		// FNetViewer falls back to the viewer when there is no view target, and a controller
		// with no pawn hits that. Adding both would put it in the list twice and replicate it
		// twice a frame.
		if (CurViewer.ViewTarget != CurViewer.InViewer)
		{
			ReplicationActorList.ConditionalAdd(CurViewer.ViewTarget);
		}

		AFortPlayerController* PC = Cast<AFortPlayerController>(CurViewer.InViewer);
		if (!PC)
		{
			continue;
		}

		// 50% throttling of player states: half the connections get theirs on even frames,
		// half on odd ones, which halves the peak cost without ever starving anyone.
		const bool bReplicatePS = (Params.ConnectionManager.ConnectionOrderNum % 2) == (Params.ReplicationFrameNum % 2);
		if (bReplicatePS)
		{
			// The owning connection's own player state always goes out at full rate; the
			// simulated ones are the frequency limiter's job.
			if (AActor* PlayerStateActor = ToActor(PC->PlayerState))
			{
				if (!bInitializedPlayerState)
				{
					bInitializedPlayerState = true;

					FConnectionReplicationActorInfo& ConnectionActorInfo = Params.ConnectionManager.ActorInfoMap.FindOrAdd(PlayerStateActor);
					ConnectionActorInfo.ReplicationPeriodFrame = 1;
				}

				ReplicationActorList.ConditionalAdd(PlayerStateActor);
			}
		}

		FCachedAlwaysRelevantActorInfo& LastData = PastRelevantActorMap.FindOrAdd(TObjectKey<UNetConnection>(CurViewer.Connection));

		/*
		 * The connection's own pawn and whatever it is looking through replicate every frame, at
		 * any range.
		 *
		 * They are also in the spatialization grid, and the grid's dynamic node rewrites
		 * ReplicationPeriodFrame for every actor it handles, every frame, from the dot product
		 * between the viewer's facing and the direction to the actor. For a connection's own pawn
		 * that direction is the zero vector, so the dot product is 0 and the pawn lands in the
		 * "behind the viewer" zone - the slowest one there is. A player's own pawn was being
		 * scheduled as though it were something they had their back to.
		 *
		 * This node is gathered after the global ones, so pinning here is what the connection
		 * ends up with. It has to be re-pinned every frame because the grid overwrites it every
		 * frame.
		 */
		auto PinToFullRate = [&Params](AActor* Actor)
		{
			if (Actor)
			{
				FConnectionReplicationActorInfo& Info = Params.ConnectionManager.ActorInfoMap.FindOrAdd(Actor);
				Info.ReplicationPeriodFrame = 1;
				Info.SetCullDistanceSquared(0.f);

				/*
				 * The period alone is not enough. The dynamic node writes NextReplicationFrameNum
				 * as well, from the period it just chose, so pinning only the period leaves the
				 * actor gated until the frame that node picked - it was still being skipped as
				 * "not ready" with a period of 1. Pull the schedule back to this frame; that is
				 * what a period of one means.
				 */
				Info.NextReplicationFrameNum = FMath::Min(Info.NextReplicationFrameNum, Params.ReplicationFrameNum);
			}
		};

		PinToFullRate(CurViewer.ViewTarget);

		if (AFortPlayerPawn* Pawn = PC->MyFortPawn)
		{
			AActor* PawnActor = ToActor(Pawn);

			PinToFullRate(PawnActor);

			UpdateCachedRelevantActor(Params, PawnActor, LastData.LastViewer);

			if (PawnActor != CurViewer.ViewTarget)
			{
				ReplicationActorList.ConditionalAdd(PawnActor);
			}

			// The owner's own weapons are gathered here rather than being routed, so that they
			// are never held back by the pawn's own frequency. Everyone else's arrive as
			// dependent actors of the pawn holding them; see UpdatePawnDependentActors.
			for (AFortWeapon* Weapon : Pawn->CurrentWeaponList)
			{
				ReplicationActorList.ConditionalAdd(ToActor(Weapon));
			}
		}

		if (CurViewer.ViewTarget)
		{
			UpdateCachedRelevantActor(Params, CurViewer.ViewTarget, LastData.LastViewTarget);
		}

		// The connection's own inventory only exists for it, so it is gathered here rather
		// than being routed into a node every connection would walk.
		if (AFortInventory* Inventory = PC->WorldInventory)
		{
			ReplicationActorList.ConditionalAdd(ToActor(Inventory));
		}

		// AFortInventoryOutpost is a second, separate inventory actor hanging off the same
		// controller, and is owner-only for the same reason WorldInventory is.
		if (AFortInventory* OutpostInventory = PC->OutpostInventory)
		{
			ReplicationActorList.ConditionalAdd(ToActor(OutpostInventory));
		}

		/*
		 * AFortBroadcastRemoteClientInfo is bOnlyRelevantToOwner, so GetClassNodeMapping lands it
		 * on NotRouted and nothing else ever picks it up - the graph has no equivalent of the
		 * owner test AActor::IsNetRelevantFor does for the classic path, which is why the
		 * reference server replicates this and the graph did not.
		 *
		 * It is not cosmetic. AFortPlayerControllerAthena::BroadcastRemoteClientInfo is a
		 * replicated pointer, so leaving the actor unreplicated leaves that pointer null on the
		 * client, and the fullscreen map talks to it: ServerSetPlayerFullScreenMapActive and
		 * OnPlayerFullScreenMapActiveChanged both live on this class. With it null the map screen
		 * builds its whole widget tree and then tears down again in the same frame.
		 */
		if (SDK::AFortPlayerControllerAthena* AthenaPC = Cast<SDK::AFortPlayerControllerAthena>(PC))
		{
			if (AthenaPC->BroadcastRemoteClientInfo)
			{
				ReplicationActorList.ConditionalAdd(ToActor(AthenaPC->BroadcastRemoteClientInfo));
			}
		}

		// The team's private info is only relevant to that team.
		if (AFortPlayerState* FortPS = Cast<AFortPlayerState>(PC->PlayerState))
		{
			if (AFortTeamPrivateInfo* TeamInfo = FortPS->PlayerTeamPrivate.Get())
			{
				ReplicationActorList.ConditionalAdd(ToActor(TeamInfo));
			}
		}
	}

	CleanupCachedRelevantActors(PastRelevantActorMap);

	// -----------------------------------------------
	//	Always relevant actors that live in streaming levels
	// -----------------------------------------------

	FPerConnectionActorInfoMap& ConnectionActorInfoMap = Params.ConnectionManager.ActorInfoMap;

	if (FortGraph)
	{
		TMap<FName, FActorRepListRefView>& AlwaysRelevantStreamingLevelActors = FortGraph->AlwaysRelevantStreamingLevelActors;

		for (int32 Idx = AlwaysRelevantStreamingLevelsNeedingReplication.Num() - 1; Idx >= 0; --Idx)
		{
			const FName& StreamingLevel = AlwaysRelevantStreamingLevelsNeedingReplication[Idx];

			FActorRepListRefView* Ptr = AlwaysRelevantStreamingLevelActors.Find(StreamingLevel);
			if (Ptr == nullptr)
			{
				// No always relevant list for that level at all.
				if (FortRepGraph::DisplayClientLevelStreaming > 0)
				{
					UE_LOG(LogFortReplicationGraph, Display, "CLIENTSTREAMING Removing %s from AlwaysRelevantStreamingLevelActors because FActorRepListRefView is null.",
						StreamingLevel.ToString().c_str());
				}

				AlwaysRelevantStreamingLevelsNeedingReplication.RemoveAtSwap(Idx);
				continue;
			}

			FActorRepListRefView& RepList = *Ptr;

			if (RepList.Num() > 0)
			{
				bool bAllDormant = true;
				for (FActorRepListType Actor : RepList)
				{
					FConnectionReplicationActorInfo& ConnectionActorInfo = ConnectionActorInfoMap.FindOrAdd(Actor);
					if (ConnectionActorInfo.bDormantOnConnection == false)
					{
						bAllDormant = false;
						break;
					}
				}

				if (bAllDormant)
				{
					// Everything in the level has gone dormant on this connection, so the list
					// does not need visiting again until something wakes.
					if (FortRepGraph::DisplayClientLevelStreaming > 0)
					{
						UE_LOG(LogFortReplicationGraph, Display, "CLIENTSTREAMING All AlwaysRelevant Actors Dormant on StreamingLevel %s. Removing list.",
							StreamingLevel.ToString().c_str());
					}

					AlwaysRelevantStreamingLevelsNeedingReplication.RemoveAtSwap(Idx);
				}
				else
				{
					Params.OutGatheredReplicationLists.AddReplicationActorList(RepList);
				}
			}
		}
	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);
}

void UFortReplicationGraphNode_AlwaysRelevant_ForConnection::FlushStreamingAlwaysRelevantList(const FNewReplicatedActorInfo& ActorInfo)
{
	if (FortRepGraph::DisplayClientLevelStreaming > 0)
	{
		UE_LOG(LogFortReplicationGraph, Display, "CLIENTSTREAMING ::FlushStreamingAlwaysRelevantList - %s", ActorInfo.StreamingLevelName.ToString().c_str());
	}

	AlwaysRelevantStreamingLevelsNeedingReplication.AddUnique(ActorInfo.StreamingLevelName);
}

void UFortReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityAdd(FName LevelName, UWorld* StreamingWorld)
{
	if (FortRepGraph::DisplayClientLevelStreaming > 0)
	{
		UE_LOG(LogFortReplicationGraph, Display, "CLIENTSTREAMING ::OnClientLevelVisibilityAdd - %s", LevelName.ToString().c_str());
	}

	AlwaysRelevantStreamingLevelsNeedingReplication.Add(LevelName);
}

void UFortReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityRemove(FName LevelName)
{
	if (FortRepGraph::DisplayClientLevelStreaming > 0)
	{
		UE_LOG(LogFortReplicationGraph, Display, "CLIENTSTREAMING ::OnClientLevelVisibilityRemove - %s", LevelName.ToString().c_str());
	}

	AlwaysRelevantStreamingLevelsNeedingReplication.Remove(LevelName);
}

void UFortReplicationGraphNode_AlwaysRelevant_ForConnection::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();

	PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, InNodeName, ReplicationActorList);

	if (FortGraph)
	{
		for (const FName& LevelName : AlwaysRelevantStreamingLevelsNeedingReplication)
		{
			if (FActorRepListRefView* RepList = FortGraph->AlwaysRelevantStreamingLevelActors.Find(LevelName))
			{
				PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "AlwaysRelevant StreamingLevel List: " + LevelName.ToString(), *RepList);
			}
		}
	}

	DebugInfo.PopIndent();
}

// ------------------------------------------------------------------------------
// UFortReplicationGraphNode_PlayerStateFrequencyLimiter
// ------------------------------------------------------------------------------

UFortReplicationGraphNode_PlayerStateFrequencyLimiter::UFortReplicationGraphNode_PlayerStateFrequencyLimiter()
{
	bRequiresPrepareForReplicationCall = true;
}

void UFortReplicationGraphNode_PlayerStateFrequencyLimiter::PrepareForReplication()
{
	ReplicationActorLists.Reset();
	ForceNetUpdateReplicationActorList.Reset();

	ReplicationActorLists.AddDefaulted();
	FActorRepListRefView* CurrentList = &ReplicationActorLists[0];

	// The lists are rebuilt every frame. That is not the cheapest thing possible, but it is
	// the simplest way to handle players disconnecting and to keep the lists compact -
	// persistent lists would need defragmenting as players leave.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	static UClass* PlayerStateClass = APlayerState::StaticClass();

	for (ULevel* Level : World->Levels)
	{
		if (!Level)
		{
			continue;
		}

		for (SDK::AActor* LevelActor : Level->Actors)
		{
			if (!LevelActor || !LevelActor->IsA(PlayerStateClass))
			{
				continue;
			}

			AActor* PS = ToActor(LevelActor);
			if (IsActorValidForReplicationGather(PS) == false)
			{
				continue;
			}

			if (CurrentList->Num() >= TargetActorsPerFrame)
			{
				ReplicationActorLists.AddDefaulted();
				CurrentList = &ReplicationActorLists.Last();
			}

			CurrentList->Add(PS);
		}
	}
}

void UFortReplicationGraphNode_PlayerStateFrequencyLimiter::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	if (ReplicationActorLists.Num() == 0)
	{
		return;
	}

	const int32 ListIdx = Params.ReplicationFrameNum % ReplicationActorLists.Num();
	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorLists[ListIdx]);

	if (ForceNetUpdateReplicationActorList.Num() > 0)
	{
		Params.OutGatheredReplicationLists.AddReplicationActorList(ForceNetUpdateReplicationActorList);
	}
}

void UFortReplicationGraphNode_PlayerStateFrequencyLimiter::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();

	int32 Index = 0;
	for (const FActorRepListRefView& List : ReplicationActorLists)
	{
		PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "Bucket[" + std::to_string(Index++) + "]", List);
	}

	DebugInfo.PopIndent();
}

// ------------------------------------------------------------------------------
// UFortReplicationGraphNode_LiveSpectator
// ------------------------------------------------------------------------------

UFortReplicationGraphNode_LiveSpectator::UFortReplicationGraphNode_LiveSpectator()
{
	// Nothing qualifies unless it is named here; TClassMap hands the answer down the hierarchy,
	// so one entry covers every subclass and blueprint below it.
	ClassesForSpectatorRelevancy.Set(AActor::StaticClass(), false);

	if (UClass* PlayerPawnClass = UObject::FindClassFast("FortPlayerPawn"))
	{
		ClassesForSpectatorRelevancy.Set(PlayerPawnClass, true);
	}

	if (UClass* SupplyDropClass = UObject::FindClassFast("FortAthenaSupplyDrop"))
	{
		ClassesForSpectatorRelevancy.Set(SupplyDropClass, true);
	}
}

void UFortReplicationGraphNode_LiveSpectator::NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo)
{
	if (ClassesForSpectatorRelevancy.GetChecked(ActorInfo.Class))
	{
		UReplicationGraphNode_ActorList::NotifyAddNetworkActor(ActorInfo);
	}
}

bool UFortReplicationGraphNode_LiveSpectator::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	if (!ClassesForSpectatorRelevancy.GetChecked(ActorInfo.Class))
	{
		return false;
	}

	// Order does not matter in this list, so the cheap removal is the right one.
	return RemoveNetworkActorFast(ActorInfo);
}

void UFortReplicationGraphNode_LiveSpectator::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	// Only spectating connections pay for this node at all.
	const UFortReplicationGraphConnection* FortConnection = Cast<UFortReplicationGraphConnection>(&Params.ConnectionManager);
	if (FortConnection == nullptr || !FortConnection->bIsLiveSpectator)
	{
		return;
	}

	// A broadcast is showing these actors to an audience, so they may not be culled away from
	// the spectator's camera wherever it happens to be. Written per connection because it is
	// per-connection state; the node's population is small and only spectators reach this.
	for (FActorRepListType Actor : ReplicationActorList)
	{
		FConnectionReplicationActorInfo& ConnectionActorInfo = Params.ConnectionManager.ActorInfoMap.FindOrAdd(Actor);
		ConnectionActorInfo.SetCullDistanceSquared(0.f);
	}

	UReplicationGraphNode_ActorList::GatherActorListsForConnection(Params);
}

// ------------------------------------------------------------------------------
// UFortReplicationGraphNode_Custom
// ------------------------------------------------------------------------------

void UFortReplicationGraphNode_Custom::NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo)
{
	ReplicationActorList.ConditionalAdd(ActorInfo.Actor);
}

bool UFortReplicationGraphNode_Custom::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	return ReplicationActorList.RemoveFast(ActorInfo.Actor);
}

void UFortReplicationGraphNode_Custom::NotifyResetAllNetworkActors()
{
	ReplicationActorList.Reset();
}

void UFortReplicationGraphNode_Custom::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	if (ShouldGatherForConnection && !ShouldGatherForConnection(Params))
	{
		return;
	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);
}

// ------------------------------------------------------------------------------
// UFortReplicationGraphNode_AlwaysRelevantForTeam / ForSquad
// ------------------------------------------------------------------------------

void UFortReplicationGraphNode_AlwaysRelevantForTeam::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	UReplicationGraphNode_ActorList::GatherActorListsForConnection(Params);
}

UFortReplicationGraphNode_AlwaysRelevantForSquad::UFortReplicationGraphNode_AlwaysRelevantForSquad()
{
	bRequiresPrepareForReplicationCall = true;
}

void UFortReplicationGraphNode_AlwaysRelevantForSquad::NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo)
{
	AlwaysRelevantActors.AddUnique(ActorInfo.Actor);
}

bool UFortReplicationGraphNode_AlwaysRelevantForSquad::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	return AlwaysRelevantActors.RemoveSingleSwap(ActorInfo.Actor) > 0;
}

void UFortReplicationGraphNode_AlwaysRelevantForSquad::NotifyResetAllNetworkActors()
{
	PlayerStates.Reset();
	AlwaysRelevantActors.Reset();
	GatheredList.Reset();
}

void UFortReplicationGraphNode_AlwaysRelevantForSquad::PrepareForReplication()
{
	// In solo this node has nothing to say. Deleting it outright would be marginally
	// better than skipping it, but a squad of one is not worth the bookkeeping.
	if (PlayerStates.Num() <= 1)
	{
		return;
	}

	const int32 PreviousNum = GatheredList.Num();

	GatheredList.Reset(PlayerStates.Num() + AlwaysRelevantActors.Num());

	for (AActor* PlayerState : PlayerStates)
	{
		AFortPlayerStateAthena* PS = Cast<AFortPlayerStateAthena>(PlayerState);
		if (!PS)
		{
			continue;
		}

		// The squad wants each other's pawns at any distance; the player states themselves
		// go out through the frequency limiter like everyone else's.
		if (AFortPlayerPawn* Pawn = PS->GetCurrentPawn())
		{
			GatheredList.ConditionalAdd(ToActor(Pawn));
		}
	}

	for (AActor* Actor : AlwaysRelevantActors)
	{
		GatheredList.ConditionalAdd(Actor);
	}

	bUpdatePerConnectionCullDistances = (PreviousNum != GatheredList.Num());
}

void UFortReplicationGraphNode_AlwaysRelevantForSquad::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	if (GatheredList.Num() == 0)
	{
		return;
	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(GatheredList);

	if (bUpdatePerConnectionCullDistances)
	{
		// Only when the set changed: writing this every frame for every connection is
		// exactly the per-connection work the graph exists to avoid.
		for (FActorRepListType Actor : GatheredList)
		{
			FConnectionReplicationActorInfo& ConnectionActorInfo = Params.ConnectionManager.ActorInfoMap.FindOrAdd(Actor);
			ConnectionActorInfo.SetCullDistanceSquared(0.f);
		}
	}
}

void UFortReplicationGraphNode_AlwaysRelevantForSquad::LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& InNodeName) const
{
	DebugInfo.Log(InNodeName);
	DebugInfo.PushIndent();
	PRISM_NAMESPACE_NAME::LogActorRepList(DebugInfo, "Squad: " + std::to_string(SquadID), GatheredList);
	DebugInfo.PopIndent();
}

PRISM_NAMESPACE_END
