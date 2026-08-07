#pragma once

/*=============================================================================
	SharedPointer.h: Engine-shaped smart pointers and TFunction.

	The Replication Graph stores TSharedPtr<FReplicationGraphGlobalData>,
	TUniquePtr<FGlobalActorReplicationInfo> and TFunction<> callbacks. Those are
	ordinary C++ ownership models, so they are spelled the engine's way here and
	implemented with the standard library underneath.
=============================================================================*/

#include <functional>
#include <memory>
#include <utility>

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

/*-----------------------------------------------------------------------------
	TFunction
-----------------------------------------------------------------------------*/

/**
 * A callable, held behind one pointer.
 *
 * This is deliberately not a std::function typedef, and the reason is a hard requirement of
 * the containers rather than a preference.
 *
 * TSet and TMap are built on TSparseArray, which keeps its elements as opaque aligned bytes
 * inside a TArray and relocates them with a single memmove when it grows. That is the engine's
 * design and it is what makes those containers fast, but it is only correct for types that are
 * trivially relocatable - types whose bytes fully describe them, with no pointer into their own
 * storage.
 *
 * MSVC's std::function is 64 bytes and stores small callables inline, with an implementation
 * pointer aimed at that inline buffer. Move its bytes and the pointer still refers to where the
 * object used to be. FClassReplicationInfo holds one of these (FastSharedReplicationFunc) and
 * lives in a TMap, so the first time that map grew, every entry in it was quietly corrupted and
 * the next copy of one dereferenced a stale pointer.
 *
 * Putting the callable on the heap makes this object exactly one pointer, so relocating it is
 * moving that pointer - which memmove does correctly. Copy still deep-copies, so the value
 * semantics callers already rely on are unchanged.
 */
template<typename FuncType>
class TFunction
{
public:
	TFunction() = default;
	TFunction(std::nullptr_t) { }

	/** Wraps any callable. Constrained so it does not hide the copy and move constructors. */
	template<typename CallableType,
		typename = std::enable_if_t<!std::is_same_v<std::decay_t<CallableType>, TFunction>>>
	TFunction(CallableType&& InCallable)
		: Impl(std::make_unique<std::function<FuncType>>(std::forward<CallableType>(InCallable)))
	{
	}

	TFunction(const TFunction& Other)
		: Impl(Other.Impl ? std::make_unique<std::function<FuncType>>(*Other.Impl) : nullptr)
	{
	}

	TFunction(TFunction&&) = default;
	TFunction& operator=(TFunction&&) = default;

	TFunction& operator=(const TFunction& Other)
	{
		if (this != &Other)
		{
			Impl = Other.Impl ? std::make_unique<std::function<FuncType>>(*Other.Impl) : nullptr;
		}
		return *this;
	}

	TFunction& operator=(std::nullptr_t)
	{
		Impl.reset();
		return *this;
	}

	template<typename CallableType,
		typename = std::enable_if_t<!std::is_same_v<std::decay_t<CallableType>, TFunction>>>
	TFunction& operator=(CallableType&& InCallable)
	{
		Impl = std::make_unique<std::function<FuncType>>(std::forward<CallableType>(InCallable));
		return *this;
	}

	template<typename... ArgTypes>
	decltype(auto) operator()(ArgTypes&&... Args) const { return (*Impl)(std::forward<ArgTypes>(Args)...); }

	bool IsSet() const { return Impl != nullptr && static_cast<bool>(*Impl); }
	explicit operator bool() const { return IsSet(); }

	friend bool operator==(const TFunction& Lhs, std::nullptr_t) { return !Lhs.IsSet(); }
	friend bool operator!=(const TFunction& Lhs, std::nullptr_t) { return Lhs.IsSet(); }
	friend bool operator==(std::nullptr_t, const TFunction& Rhs) { return !Rhs.IsSet(); }
	friend bool operator!=(std::nullptr_t, const TFunction& Rhs) { return Rhs.IsSet(); }

private:

	std::unique_ptr<std::function<FuncType>> Impl;
};

/*-----------------------------------------------------------------------------
	TUniquePtr
-----------------------------------------------------------------------------*/

template<typename ObjectType>
class TUniquePtr
{
public:
	TUniquePtr() = default;
	TUniquePtr(std::nullptr_t) { }
	explicit TUniquePtr(ObjectType* InPtr) : Ptr(InPtr) { }

	TUniquePtr(TUniquePtr&&) = default;
	TUniquePtr& operator=(TUniquePtr&&) = default;

	TUniquePtr(const TUniquePtr&) = delete;
	TUniquePtr& operator=(const TUniquePtr&) = delete;

	TUniquePtr& operator=(std::nullptr_t) { Ptr.reset(); return *this; }

	ObjectType* Get() const { return Ptr.get(); }
	ObjectType* operator->() const { return Ptr.get(); }
	ObjectType& operator*() const { return *Ptr; }

	bool IsValid() const { return Ptr != nullptr; }
	explicit operator bool() const { return IsValid(); }

	void Reset(ObjectType* InPtr = nullptr) { Ptr.reset(InPtr); }
	ObjectType* Release() { return Ptr.release(); }

private:

	std::unique_ptr<ObjectType> Ptr;
};

template<typename ObjectType, typename... ArgTypes>
TUniquePtr<ObjectType> MakeUnique(ArgTypes&&... Args)
{
	return TUniquePtr<ObjectType>(new ObjectType(std::forward<ArgTypes>(Args)...));
}

/*-----------------------------------------------------------------------------
	TSharedPtr
-----------------------------------------------------------------------------*/

template<typename ObjectType>
class TSharedPtr
{
public:
	TSharedPtr() = default;
	TSharedPtr(std::nullptr_t) { }
	explicit TSharedPtr(ObjectType* InPtr) : Ptr(InPtr) { }
	TSharedPtr(const std::shared_ptr<ObjectType>& InPtr) : Ptr(InPtr) { }

	ObjectType* Get() const { return Ptr.get(); }
	ObjectType* operator->() const { return Ptr.get(); }
	ObjectType& operator*() const { return *Ptr; }

	bool IsValid() const { return Ptr != nullptr; }
	explicit operator bool() const { return IsValid(); }

	void Reset() { Ptr.reset(); }

private:

	std::shared_ptr<ObjectType> Ptr;
};

template<typename ObjectType, typename... ArgTypes>
TSharedPtr<ObjectType> MakeShared(ArgTypes&&... Args)
{
	return TSharedPtr<ObjectType>(std::make_shared<ObjectType>(std::forward<ArgTypes>(Args)...));
}

template<typename ObjectType>
TSharedPtr<ObjectType> MakeShareable(ObjectType* InPtr)
{
	return TSharedPtr<ObjectType>(InPtr);
}

PRISM_NAMESPACE_END

/*-----------------------------------------------------------------------------
	Relocatability

	TSet and TMap relocate their elements with a memmove, so a type stored in
	one has to be trivially relocatable - its bytes must fully describe it, with
	no pointer into its own storage. The default answer to that question is
	std::is_trivially_copyable, which is false for every type below because each
	has a user-provided copy or destructor.

	They are all relocatable regardless, and for the same reason: each is one or
	two pointers to something else. Moving those bytes and not destroying the
	source is exactly a move. Saying so here is what lets them be map values -
	FGlobalActorReplicationInfoMap and FPerConnectionActorInfoMap are both maps
	of smart pointers, and every one of the graph's TFunction callbacks reaches
	a container eventually.
-----------------------------------------------------------------------------*/

namespace UC
{
	template<typename FuncType>
	struct TIsTriviallyRelocatable<PRISM_NAMESPACE_NAME::TFunction<FuncType>>
	{
		static constexpr bool Value = true;
	};

	template<typename ObjectType>
	struct TIsTriviallyRelocatable<PRISM_NAMESPACE_NAME::TUniquePtr<ObjectType>>
	{
		static constexpr bool Value = true;
	};

	template<typename ObjectType>
	struct TIsTriviallyRelocatable<PRISM_NAMESPACE_NAME::TSharedPtr<ObjectType>>
	{
		static constexpr bool Value = true;
	};
}
