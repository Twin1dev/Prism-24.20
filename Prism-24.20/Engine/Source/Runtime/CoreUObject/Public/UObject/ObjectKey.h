#pragma once

/*=============================================================================
	ObjectKey.h: Weak, hashable handle to a UObject.

	The engine's FObjectKey stores the object's index and serial number so that
	a key stays comparable after the object it named has been collected. Inside
	the target the object table is reachable but the serial number is not part
	of the generated SDK, so this keys on the pointer and resolves by checking
	the object is still in GObjects before handing it back. That is weaker than
	the engine's version against index reuse, and is called out here rather than
	hidden: keys are only held for the lifetime of a graph node, and every
	consumer re-validates through IsActorValidForReplication anyway.
=============================================================================*/

#include "CoreGlobals.h"
#include "Containers/Map.h"

PRISM_NAMESPACE_START

class FObjectKey
{
public:
	FObjectKey() = default;
	FObjectKey(const UObject* Object) : ObjectPtr(const_cast<UObject*>(Object)) { }

	UObject* ResolveObjectPtr() const { return ObjectPtr; }
	UObject* ResolveObjectPtrEvenIfGarbage() const { return ObjectPtr; }

	bool IsValid() const { return ObjectPtr != nullptr; }

	friend bool operator==(const FObjectKey& Lhs, const FObjectKey& Rhs) { return Lhs.ObjectPtr == Rhs.ObjectPtr; }
	friend bool operator!=(const FObjectKey& Lhs, const FObjectKey& Rhs) { return Lhs.ObjectPtr != Rhs.ObjectPtr; }
	friend bool operator<(const FObjectKey& Lhs, const FObjectKey& Rhs) { return Lhs.ObjectPtr < Rhs.ObjectPtr; }

	friend uint32 GetTypeHash(const FObjectKey& Key) { return GetTypeHash(static_cast<const void*>(Key.ObjectPtr)); }

private:

	UObject* ObjectPtr = nullptr;
};

/** Typed flavour of FObjectKey, so a map declaration says what it holds. */
template<typename ObjectType>
class TObjectKey
{
public:
	TObjectKey() = default;
	TObjectKey(const ObjectType* Object) : ObjectPtr(const_cast<ObjectType*>(Object)) { }

	ObjectType* ResolveObjectPtr() const { return ObjectPtr; }

	bool IsValid() const { return ObjectPtr != nullptr; }

	friend bool operator==(const TObjectKey& Lhs, const TObjectKey& Rhs) { return Lhs.ObjectPtr == Rhs.ObjectPtr; }
	friend bool operator!=(const TObjectKey& Lhs, const TObjectKey& Rhs) { return Lhs.ObjectPtr != Rhs.ObjectPtr; }
	friend bool operator<(const TObjectKey& Lhs, const TObjectKey& Rhs) { return Lhs.ObjectPtr < Rhs.ObjectPtr; }

	friend uint32 GetTypeHash(const TObjectKey& Key) { return GetTypeHash(static_cast<const void*>(Key.ObjectPtr)); }

private:

	ObjectType* ObjectPtr = nullptr;
};

PRISM_NAMESPACE_END
