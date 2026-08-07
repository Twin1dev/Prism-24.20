#pragma once

/*=============================================================================
	ActorChannel.h: The slice of UActorChannel the Replication Graph drives.

	The graph is the only thing that opens, feeds and closes actor channels once
	it owns replication, so these three calls plus the channel's actor are all
	it needs.

	The generated UActorChannel is 'final', so this extends UChannel and reaches
	the actor at the offset the generated class records for it. The layout is
	identical either way; only the spelling differs.
=============================================================================*/

#include "CoreGlobals.h"
#include "Engine/NetConnection.h"
#include "Net/EngineAddresses.h"

PRISM_NAMESPACE_START

class AActor;

class UChannel : public UE_EXTENDS_SDK_CLASS(UChannel)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UChannel)

	UNetConnection* GetConnection() const { return static_cast<UNetConnection*>(Connection); }

	/** Virtual, so it is reached through the slot the surviving replication path dispatches on. */
	bool Close(EChannelCloseReason Reason)
	{
		using FCloseFunction = bool (*)(UChannel*, EChannelCloseReason);

		void** VTable = *reinterpret_cast<void***>(this);
		return reinterpret_cast<FCloseFunction>(VTable[EngineAddresses::UChannel_Close_VTableSlot])(this, Reason);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(UChannel);
UE_DECLARE_PRISM_TYPE(UChannel);

class UActorChannel : public UChannel
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UActorChannel)

	/** Offset of UActorChannel::Actor, straight out of the generated class. */
	static constexpr int32 ActorOffset = 0x68;

	AActor*& GetActorRef() { return GetEngineMember<AActor*>(this, ActorOffset); }

	AActor* GetActor() const { return GetEngineMember<AActor*>(this, ActorOffset); }

	bool SetChannelActor(AActor* InActor, ESetChannelActorFlags Flags)
	{
		static auto Function = GetEngineFunction<bool (*)(UActorChannel*, AActor*, ESetChannelActorFlags)>(
			EngineAddresses::UActorChannel_SetChannelActor);

		return Function(this, InActor, Flags);
	}

	/** Writes the actor's dirty properties into the connection. Returns the number of bits sent. */
	int64 ReplicateActor()
	{
		static auto Function = GetEngineFunction<int64 (*)(UActorChannel*)>(EngineAddresses::UActorChannel_ReplicateActor);
		return Function(this);
	}
};
static_assert(sizeof(UActorChannel) == sizeof(UChannel),
	"UActorChannel may not add data members - it is a view onto the generated UActorChannel's memory.");
UE_DECLARE_PRISM_VIEW_TYPE(UActorChannel);

PRISM_NAMESPACE_END
