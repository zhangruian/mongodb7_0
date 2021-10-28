/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <fmt/format.h>
#include <list>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * This class tracks a size of entries in 'LRUKeyValue'.
 * The size can be understood as a number of the entries, an amount of memory they occupied,
 * or any other value defined by the template parameter 'Estimator'.
 * The 'Estimator' must be deterministic and always return the same value for the same entry.
 */
template <typename V, typename Estimator>
class LRUBudgetTracker {
public:
    LRUBudgetTracker(size_t maxBudget) : _max(maxBudget), _current(0) {}

    void onAdd(const V& v) {
        _current += _estimator(v);
    }

    void onRemove(const V& v) {
        using namespace fmt::literals;
        size_t budget = _estimator(v);
        tassert(5968300,
                "LRU budget underflow: current={}, budget={} "_format(_current, budget),
                _current >= budget);
        _current -= budget;
    }

    void onClear() {
        _current = 0;
    }

    // Returns true if the cache runs over budget.
    bool isOverBudget() const {
        return _current > _max;
    }

    size_t currentBudget() const {
        return _current;
    }

    void reset(size_t newMaxSize) {
        _max = newMaxSize;
    }

private:
    size_t _max;
    size_t _current;
    Estimator _estimator;
};

/**
 * A key-value store structure with a least recently used (LRU) replacement
 * policy. The size allowed in the kv-store is controlled by 'LRUBudgetTracker'
 * set in the constructor.
 *
 * Caveat:
 * This kv-store is NOT thread safe! The client to this utility is responsible
 * for protecting concurrent access to the LRU store if used in a threaded
 * context.
 *
 * Implemented as a doubly-linked list with a hash map for quickly locating the kv-store entries.
 * The add(), get(), and remove() operations are all O(1).
 *
 * The keys of generic type K map to values of type V*. The V*
 * pointers are owned by the kv-store.
 *
 * TODO: We could move this into the util/ directory and do any cleanup necessary to make it
 * fully general.
 */
template <class K, class V, class BudgetEstimator, class KeyHasher = std::hash<K>>
class LRUKeyValue {
public:
    LRUKeyValue(size_t maxSize) : _budgetTracker{maxSize} {}

    ~LRUKeyValue() {
        clear();
    }

    typedef std::pair<K, V*> KVListEntry;

    typedef std::list<KVListEntry> KVList;
    typedef typename KVList::iterator KVListIt;
    typedef typename KVList::const_iterator KVListConstIt;

    typedef stdx::unordered_map<K, KVListIt, KeyHasher> KVMap;
    typedef typename KVMap::const_iterator KVMapConstIt;

    // These type declarations are required by the 'Partitioned' utility.
    using key_type = typename KVMap::key_type;
    using mapped_type = typename KVMap::mapped_type;
    using value_type = typename KVMap::value_type;

    /**
     * Add an (K, V*) pair to the store, where 'key' can be used to retrieve value 'entry' from the
     * store. Takes ownership of 'entry'. If 'key' already exists in the kv-store, 'entry' will
     * simply replace what is already there. If after the add() operation the kv-store exceeds its
     * budget, then the least recently used entries will be evicted until the size is again
     * under-budget. Returns the number of evicted entries.
     */
    size_t add(const K& key, V* entry) {
        // If the key already exists, delete it first.
        KVMapConstIt i = _kvMap.find(key);
        if (i != _kvMap.end()) {
            KVListIt found = i->second;
            _budgetTracker.onRemove(*found->second);
            delete found->second;
            _kvMap.erase(i);
            _kvList.erase(found);
        }

        _kvList.push_front(std::make_pair(key, entry));
        _kvMap[key] = _kvList.begin();
        _budgetTracker.onAdd(*entry);

        return evict();
    }

    /**
     * Retrieve the value associated with 'key' from the kv-store. The kv-store retains ownership of
     * 'entryOut', so it should not be deleted by the caller. As a side effect, the retrieved entry
     * is promoted to the most recently used.
     */
    StatusWith<V*> get(const K& key) const {
        KVMapConstIt i = _kvMap.find(key);
        if (i == _kvMap.end()) {
            return Status(ErrorCodes::NoSuchKey, "no such key in LRU key-value store");
        }
        KVListIt found = i->second;
        V* foundEntry = found->second;

        // Promote the kv-store entry to the front of the list. It is now the most recently used.
        _kvMap.erase(i);
        _kvList.erase(found);
        _kvList.push_front(std::make_pair(key, foundEntry));
        _kvMap[key] = _kvList.begin();

        return foundEntry;
    }

    /**
     * Remove the kv-store entry keyed by 'key'.
     * Returns false if there doesn't exist such 'key', otherwise returns true.
     */
    bool erase(const K& key) {
        KVMapConstIt i = _kvMap.find(key);
        if (i == _kvMap.end()) {
            return false;
        }
        KVListIt found = i->second;
        _budgetTracker.onRemove(*i->second->second);
        delete found->second;
        _kvMap.erase(i);
        _kvList.erase(found);
        return true;
    }

    /**
     * Deletes all entries in the kv-store.
     */
    void clear() {
        for (KVListIt i = _kvList.begin(); i != _kvList.end(); i++) {
            delete i->second;
        }

        _budgetTracker.onClear();
        _kvList.clear();
        _kvMap.clear();
    }

    /**
     * Reset the kv-store with new budget tracker. Returns the number of evicted entries.
     */
    size_t reset(size_t newMaxSize) {
        _budgetTracker.reset(newMaxSize);
        return evict();
    }

    /**
     * Returns true if entry is found in the kv-store.
     */
    bool hasKey(const K& key) const {
        return _kvMap.find(key) != _kvMap.end();
    }

    /**
     * Returns the size (current budget) of the kv-store.
     */
    size_t size() const {
        return _budgetTracker.currentBudget();
    }

    /**
     * TODO: The kv-store should implement its own iterator. Calling through to the underlying
     * iterator exposes the internals, and forces the caller to make a horrible type declaration.
     */
    KVListConstIt begin() const {
        return _kvList.begin();
    }

    KVListConstIt end() const {
        return _kvList.end();
    }

private:
    /**
     * If the kv-store is over its budget this function evicts the least recently used entries until
     * the size is again under-budget. Returns the number of evicted entries
     */
    size_t evict() {
        size_t nEvicted = 0;
        while (_budgetTracker.isOverBudget()) {
            invariant(!_kvList.empty());
            std::unique_ptr<V> evictedEntry{_kvList.back().second};
            invariant(evictedEntry);
            _budgetTracker.onRemove(*evictedEntry);

            _kvMap.erase(_kvList.back().first);
            _kvList.pop_back();

            ++nEvicted;
        }

        return nEvicted;
    }

    LRUBudgetTracker<V, BudgetEstimator> _budgetTracker;

    // (K, V*) pairs are stored in this std::list. They are sorted in order of use, where the front
    // is the most recently used and the back is the least recently used.
    mutable KVList _kvList;

    // Maps from a key to the corresponding std::list entry.
    mutable KVMap _kvMap;
};

}  // namespace mongo
