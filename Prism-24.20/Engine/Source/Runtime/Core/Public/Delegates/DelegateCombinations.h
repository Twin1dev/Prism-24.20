#pragma once

/*=============================================================================
	DelegateCombinations.h: Multicast delegates, spelled the engine's way.

	The Replication Graph broadcasts a handful of per-actor events (dormancy
	changed, dormancy flushed, ForceNetUpdate) and per-connection events (client
	level visibility). Those are declared with the engine's DECLARE_MULTICAST_*
	macros, so the macros exist here and expand onto a small TFunction based
	implementation.

	Bindings are identified by an FDelegateHandle, which is what lets a node
	unbind the exact registration it made.
=============================================================================*/

#include <utility>

#include "Containers/Array.h"
#include "Templates/SharedPointer.h"

PRISM_NAMESPACE_START

class FDelegateHandle
{
public:
	enum EGenerateNewHandleType { GenerateNewHandle };

	FDelegateHandle() = default;
	explicit FDelegateHandle(EGenerateNewHandleType) : Id(++NextId) { }

	bool IsValid() const { return Id != 0; }
	void Reset() { Id = 0; }

	friend bool operator==(const FDelegateHandle& Lhs, const FDelegateHandle& Rhs) { return Lhs.Id == Rhs.Id; }
	friend bool operator!=(const FDelegateHandle& Lhs, const FDelegateHandle& Rhs) { return Lhs.Id != Rhs.Id; }

private:

	static inline uint64 NextId = 0;

	uint64 Id = 0;
};

template<typename... ParamTypes>
class TMulticastDelegate
{
public:
	using FDelegateType = TFunction<void(ParamTypes...)>;

	FDelegateHandle Add(FDelegateType&& Delegate, const void* BoundObject = nullptr)
	{
		FBinding& Binding = Bindings.Emplace_GetRef();
		Binding.Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
		Binding.Object = BoundObject;
		Binding.Delegate = std::move(Delegate);

		return Binding.Handle;
	}

	/** Matches the engine's AddUObject/AddLambda spelling for call sites copied from it. */
	FDelegateHandle AddLambda(FDelegateType Delegate) { return Add(std::move(Delegate)); }

	template<typename ObjectType, typename MethodType>
	FDelegateHandle AddUObject(ObjectType* Object, MethodType Method)
	{
		return AddRaw(Object, Method);
	}

	template<typename ObjectType, typename MethodType>
	FDelegateHandle AddRaw(ObjectType* Object, MethodType Method)
	{
		return Add([Object, Method](ParamTypes... Params) { (Object->*Method)(Params...); }, Object);
	}

	bool Remove(const FDelegateHandle& Handle)
	{
		return Bindings.RemoveAll([&Handle](const FBinding& Binding) { return Binding.Handle == Handle; }) > 0;
	}

	/**
	 * Removes every binding made against one object.
	 *
	 * This is what a listener uses to stop listening, and it is not the same thing as Clear():
	 * the per-actor events in the Replication Graph carry bindings from several nodes at once -
	 * a dormancy node, a grid cell, and the game's own graph - so a node that cleared the whole
	 * delegate to unsubscribe itself would silently unsubscribe the others too.
	 */
	int32 RemoveAll(const void* BoundObject)
	{
		return Bindings.RemoveAll([BoundObject](const FBinding& Binding) { return Binding.Object == BoundObject; });
	}

	/** Drops every binding, whoever made it. */
	void Clear() { Bindings.Reset(); }

	bool IsBound() const { return Bindings.Num() > 0; }

	/** Whether this object already has a binding here, so that it does not add a second one. */
	bool IsBoundToObject(const void* BoundObject) const
	{
		return Bindings.ContainsByPredicate([BoundObject](const FBinding& Binding) { return Binding.Object == BoundObject; });
	}

	void Broadcast(ParamTypes... Params) const
	{
		// Copied first: a listener is allowed to unbind itself from inside its own callback.
		TArray<FBinding> LocalBindings = Bindings;
		for (const FBinding& Binding : LocalBindings)
		{
			if (Binding.Delegate)
			{
				Binding.Delegate(Params...);
			}
		}
	}

private:

	struct FBinding
	{
		FDelegateHandle Handle;

		/** The object the binding calls into, or null for a lambda. Only ever compared, never used. */
		const void* Object = nullptr;

		FDelegateType Delegate;
	};

	TArray<FBinding> Bindings;
};

#define DECLARE_MULTICAST_DELEGATE(DelegateName) \
	using DelegateName = PRISM_NAMESPACE_NAME::TMulticastDelegate<>

#define DECLARE_MULTICAST_DELEGATE_OneParam(DelegateName, Param1Type) \
	using DelegateName = PRISM_NAMESPACE_NAME::TMulticastDelegate<Param1Type>

#define DECLARE_MULTICAST_DELEGATE_TwoParams(DelegateName, Param1Type, Param2Type) \
	using DelegateName = PRISM_NAMESPACE_NAME::TMulticastDelegate<Param1Type, Param2Type>

#define DECLARE_MULTICAST_DELEGATE_ThreeParams(DelegateName, Param1Type, Param2Type, Param3Type) \
	using DelegateName = PRISM_NAMESPACE_NAME::TMulticastDelegate<Param1Type, Param2Type, Param3Type>

#define DECLARE_MULTICAST_DELEGATE_FourParams(DelegateName, Param1Type, Param2Type, Param3Type, Param4Type) \
	using DelegateName = PRISM_NAMESPACE_NAME::TMulticastDelegate<Param1Type, Param2Type, Param3Type, Param4Type>

PRISM_NAMESPACE_END
