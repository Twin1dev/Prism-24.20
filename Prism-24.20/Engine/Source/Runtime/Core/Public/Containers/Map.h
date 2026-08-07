#pragma once

/*=============================================================================
	Map.h: The engine's associative containers, as this module names them.

	Same reasoning as Array.h: UnrealContainers.hpp ports the engine's TSet and
	TMap - a TSparseArray of elements plus a hash of indices into it - onto the
	game's FMalloc, and this header names them.

	The Replication Graph keeps a great deal of associative state (per-actor
	global info, per-connection actor info, class policy maps, per-node
	dormancy maps), all of it walked every frame, so it matters that these are
	the engine's containers rather than node-based standard-library ones: the
	elements sit contiguously, iteration is a walk over set bits, and removal
	during iteration is a bit clear rather than a rehash.

	TSortedMap is defined here because the port does not carry it. It is the
	engine's: a sorted array of pairs, for maps small enough that a binary
	search beats a hash and stable ordering is worth having.
=============================================================================*/

#include "Containers/Array.h"

PRISM_NAMESPACE_START

/*-----------------------------------------------------------------------------
	Hashing
-----------------------------------------------------------------------------*/

/**
 * The engine's pointer hash.
 *
 * Scalars, enums and pointers are already covered by UC::GetTypeHash, which is
 * an exact match for them and so wins overload resolution against this. This
 * overload exists for the key types in this module that hash by identity and
 * reach it explicitly - FObjectKey and TObjectKey - so that they do not each
 * have to name the port's function.
 */
inline uint32 GetTypeHash(const void* Pointer) { return UC::PointerHash(Pointer); }

/*-----------------------------------------------------------------------------
	TPair / TSet / TMap
-----------------------------------------------------------------------------*/

template<typename InKeyType, typename InValueType>
using TPair = UC::TPair<InKeyType, InValueType>;

template<typename InElementType,
	typename KeyFuncs = UC::DefaultKeyFuncs<InElementType>,
	typename Allocator = UC::FDefaultSetAllocator>
using TSet = UC::TSet<InElementType, KeyFuncs, Allocator>;

template<typename KeyType, typename ValueType,
	typename SetAllocator = UC::FDefaultSetAllocator,
	typename KeyFuncs = UC::TDefaultMapKeyFuncs<KeyType, ValueType, false>>
using TMap = UC::TMap<KeyType, ValueType, SetAllocator, KeyFuncs>;

template<typename KeyType, typename ValueType,
	typename SetAllocator = UC::FDefaultSetAllocator,
	typename KeyFuncs = UC::TDefaultMapKeyFuncs<KeyType, ValueType, true>>
using TMultiMap = UC::TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>;

/*-----------------------------------------------------------------------------
	TSortedMap
-----------------------------------------------------------------------------*/

/**
 * A map held as a sorted array of pairs.
 *
 * For a handful of entries this beats a hash map on every axis that matters
 * here: one allocation instead of one per element, a binary search that stays
 * in cache, and an iteration order that does not depend on hash values. The
 * Replication Graph uses it to key per-connection nodes off the connection,
 * where the count is the number of players and the map is walked far more often
 * than it is written.
 */
template<typename InKeyType, typename InValueType, typename InAllocator = FDefaultAllocator>
class TSortedMap
{
public:
	using KeyType = InKeyType;
	using ValueType = InValueType;
	using ElementType = TPair<KeyType, ValueType>;

	/** Engine-style iterator: contextually convertible to bool, Key()/Value(), removes in place. */
	template<bool bConst>
	class TBaseIterator
	{
	public:
		using MapType = std::conditional_t<bConst, const TSortedMap, TSortedMap>;
		using ITValueType = std::conditional_t<bConst, const ValueType, ValueType>;

		TBaseIterator(MapType& InMap, int32 InIndex) : Map(&InMap), Index(InIndex) { }

		explicit operator bool() const { return Index < Map->Pairs.Num(); }

		TBaseIterator& operator++() { ++Index; return *this; }

		const KeyType& Key() const { return Map->Pairs[Index].Key; }
		ITValueType& Value() const { return Map->Pairs[Index].Value; }

		/** Removes the pair the iterator is on, keeping the rest sorted. */
		void RemoveCurrent()
		{
			static_assert(!bConst, "Cannot call RemoveCurrent on a const iterator.");

			const_cast<TSortedMap*>(Map)->Pairs.RemoveAt(Index);

			// The next element has slid into this slot, so stay put; ++ moves past it otherwise.
			--Index;
		}

	private:

		MapType* Map;
		int32 Index;
	};

	using TIterator = TBaseIterator<false>;
	using TConstIterator = TBaseIterator<true>;

	// -----------------------------------------------------------

	int32 Num() const { return Pairs.Num(); }
	bool IsEmpty() const { return Pairs.Num() == 0; }

	ValueType* Find(const KeyType& Key)
	{
		const int32 Index = FindIndex(Key);
		return Index != INDEX_NONE ? &Pairs[Index].Value : nullptr;
	}

	const ValueType* Find(const KeyType& Key) const { return const_cast<TSortedMap*>(this)->Find(Key); }

	ValueType& FindOrAdd(const KeyType& Key)
	{
		const int32 InsertAt = LowerBound(Key);
		if (InsertAt < Pairs.Num() && !(Key < Pairs[InsertAt].Key))
		{
			return Pairs[InsertAt].Value;
		}

		Pairs.Insert(ElementType(Key, ValueType()), InsertAt);
		return Pairs[InsertAt].Value;
	}

	ValueType& Add(const KeyType& Key, const ValueType& Value)
	{
		ValueType& Slot = FindOrAdd(Key);
		Slot = Value;
		return Slot;
	}

	int32 Remove(const KeyType& Key)
	{
		const int32 Index = FindIndex(Key);
		if (Index == INDEX_NONE)
		{
			return 0;
		}

		Pairs.RemoveAt(Index);
		return 1;
	}

	bool Contains(const KeyType& Key) const { return FindIndex(Key) != INDEX_NONE; }

	void Reset() { Pairs.Reset(); }
	void Empty(int32 Slack = 0) { Pairs.Empty(Slack); }

	TIterator CreateIterator() { return TIterator(*this, 0); }
	TConstIterator CreateConstIterator() const { return TConstIterator(*this, 0); }

	auto begin() { return Pairs.begin(); }
	auto begin() const { return Pairs.begin(); }
	auto end() { return Pairs.end(); }
	auto end() const { return Pairs.end(); }

private:

	/** Index of the first pair whose key is not less than Key - the insertion point. */
	int32 LowerBound(const KeyType& Key) const
	{
		int32 Low = 0;
		int32 High = Pairs.Num();

		while (Low < High)
		{
			const int32 Mid = Low + ((High - Low) >> 1);
			if (Pairs[Mid].Key < Key)
			{
				Low = Mid + 1;
			}
			else
			{
				High = Mid;
			}
		}

		return Low;
	}

	int32 FindIndex(const KeyType& Key) const
	{
		const int32 Index = LowerBound(Key);
		return (Index < Pairs.Num() && !(Key < Pairs[Index].Key)) ? Index : INDEX_NONE;
	}

	TArray<ElementType, InAllocator> Pairs;
};

PRISM_NAMESPACE_END
