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

#include <algorithm>
#include <type_traits>

#include <boost/intrusive_ptr.hpp>

#include "mongo/platform/atomic_word.h"

#include "mongo/base/data_view.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class UniqueBuffer;
/**
 * A mutable, ref-counted buffer.
 */
class SharedBuffer {
public:
    SharedBuffer() = default;
    explicit SharedBuffer(UniqueBuffer&& uniqueBuf);

    void swap(SharedBuffer& other) {
        _holder.swap(other._holder);
    }

    static SharedBuffer allocate(size_t bytes) {
        return takeOwnership(mongoMalloc(kHolderSize + bytes), bytes);
    }

    /**
     * Resizes the buffer, copying the current contents.
     *
     * Like ::realloc() this can be called on a null SharedBuffer.
     *
     * This method is illegal to call if any other SharedBuffer instances share this buffer since
     * they wouldn't be updated and would still try to delete the original buffer.
     */
    void realloc(size_t size) {
        invariant(!_holder || !_holder->isShared());

        const size_t realSize = size + kHolderSize;
        void* newPtr = mongoRealloc(_holder.get(), realSize);

        // Get newPtr into _holder with a ref-count of 1 without touching the current pointee of
        // _holder which is now invalid.
        auto tmp = SharedBuffer::takeOwnership(newPtr, size);
        _holder.detach();
        _holder = std::move(tmp._holder);
    }

    /**
     * Resizes the buffer, copying the current contents. If shared, an exclusive copy is made.
     */
    void reallocOrCopy(size_t size) {
        if (isShared()) {
            auto tmp = SharedBuffer::allocate(size);
            memcpy(tmp._holder->data(),
                   _holder->data(),
                   std::min(size, static_cast<size_t>(_holder->_capacity)));
            swap(tmp);
        } else if (_holder) {
            realloc(size);
        } else {
            *this = SharedBuffer::allocate(size);
        }
    }

    char* get() const {
        return _holder ? _holder->data() : nullptr;
    }

    explicit operator bool() const {
        return bool(_holder);
    }

    /**
     * Returns true if this object has exclusive access to the underlying buffer.
     * (That is, reference count == 1).
     */
    bool isShared() const {
        return _holder && _holder->isShared();
    }

    /**
     * Returns the allocation size of the underlying buffer.
     * Users of this type must maintain the "used" size separately.
     */
    size_t capacity() const {
        return _holder ? _holder->_capacity : 0;
    }

private:
    class Holder {
    public:
        explicit Holder(unsigned initial, size_t capacity)
            : _refCount(initial), _capacity(capacity) {
            invariant(capacity == _capacity);
        }

        // these are called automatically by boost::intrusive_ptr
        friend void intrusive_ptr_add_ref(Holder* h) {
            h->_refCount.fetchAndAdd(1);
        }

        friend void intrusive_ptr_release(Holder* h) {
            if (h->_refCount.subtractAndFetch(1) == 0) {
                // We placement new'ed a Holder in takeOwnership above,
                // so we must destroy the object here.
                h->~Holder();
                free(h);
            }
        }

        char* data() {
            return reinterpret_cast<char*>(this + 1);
        }

        const char* data() const {
            return reinterpret_cast<const char*>(this + 1);
        }

        bool isShared() const {
            return _refCount.load() > 1;
        }

        AtomicWord<unsigned> _refCount;
        uint32_t _capacity;
    };

    explicit SharedBuffer(Holder* holder) : _holder(holder, /*add_ref=*/false) {
        // NOTE: The 'false' above is because we have already initialized the Holder with a
        // refcount of '1' in takeOwnership below. This avoids an atomic increment.
    }

    /**
     * Given a pointer to a region of un-owned data, prefixed by sufficient space for a
     * SharedBuffer::Holder object, return an SharedBuffer that owns the memory.
     *
     * This class will call free(holderPrefixedData), so it must have been allocated in a way
     * that makes that valid.
     */
    static SharedBuffer takeOwnership(void* holderPrefixedData, size_t capacity) {
        // Initialize the refcount to 1 so we don't need to increment it in the constructor
        // (see private Holder* constructor above).
        //
        // TODO: Should dassert alignment of holderPrefixedData here if possible.
        return SharedBuffer(new (holderPrefixedData) Holder(1U, capacity));
    }

    boost::intrusive_ptr<Holder> _holder;

public:
    // Declared here so definition of 'Holder' is available.
    static constexpr size_t kHolderSize = sizeof(Holder);
};

MONGO_STATIC_ASSERT(std::is_nothrow_move_constructible_v<SharedBuffer>);
MONGO_STATIC_ASSERT(std::is_nothrow_move_assignable_v<SharedBuffer>);

inline void swap(SharedBuffer& one, SharedBuffer& two) {
    one.swap(two);
}

/**
 * A constant view into a ref-counted buffer.
 *
 * Use SharedBuffer to allocate since allocating a const buffer is useless.
 */
class ConstSharedBuffer {
public:
    ConstSharedBuffer() = default;
    /*implicit*/ ConstSharedBuffer(SharedBuffer source) : _buffer(std::move(source)) {}

    void swap(ConstSharedBuffer& other) {
        _buffer.swap(other._buffer);
    }

    const char* get() const {
        return _buffer.get();
    }

    explicit operator bool() const {
        return bool(_buffer);
    }

    bool isShared() const {
        return _buffer.isShared();
    }

    size_t capacity() const {
        return _buffer.capacity();
    }

    /**
     * Converts to a mutable SharedBuffer.
     * This is only legal to call if you have exclusive access to the underlying buffer.
     */
    SharedBuffer constCast() && {
        invariant(!isShared());
        return std::move(_buffer);
    }

    // The buffer holder size for 'ConstSharedBuffer' is the same as the one for 'SharedBuffer'
    static constexpr size_t kHolderSize = SharedBuffer::kHolderSize;

private:
    SharedBuffer _buffer;
};

inline void swap(ConstSharedBuffer& one, ConstSharedBuffer& two) {
    one.swap(two);
}

/**
 * A uniquely owned buffer. Has the same memory layout as SharedBuffer so that it
 * can be easily converted into a SharedBuffer.
 *
 * Layout:
 * | <size (4 bytes)> <unused (4 bytes)> | <data> |
 *
 * When converting to SharedBuffer, the entire prefix region is turned into a Holder.
 */
class UniqueBuffer {
public:
    static UniqueBuffer allocate(uint32_t sz) {
        return UniqueBuffer(mongoMalloc(SharedBuffer::kHolderSize + sz), sz);
    }

    /**
     * Given memory which was released from a UniqueBuffer using the release() method,
     * returns a UniqueBuffer owning that memory.
     */
    static UniqueBuffer reclaim(char* data) {
        return UniqueBuffer(data - SharedBuffer::kHolderSize);
    }

    UniqueBuffer() = default;
    UniqueBuffer(const UniqueBuffer&) = delete;
    UniqueBuffer(UniqueBuffer&& other) : _data(other._data) {
        other._data = nullptr;
    }
    ~UniqueBuffer() {
        free(_data);
    }

    UniqueBuffer& operator=(const UniqueBuffer&) = delete;
    UniqueBuffer& operator=(UniqueBuffer&& other) {
        UniqueBuffer temp(std::move(other));
        swap(*this, temp);
        return *this;
    }

    friend void swap(UniqueBuffer& lhs, UniqueBuffer& rhs) {
        using std::swap;
        swap(lhs._data, rhs._data);
    }

    void realloc(uint32_t size) {
        size_t realSize = size + SharedBuffer::kHolderSize;
        _data = reinterpret_cast<char*>(mongoRealloc(_data, realSize));
        DataView(_data).write<uint32_t>(size);
    }

    char* get() const {
        return _data ? _data + SharedBuffer::kHolderSize : nullptr;
    }

    explicit operator bool() const {
        return _data != nullptr;
    }

    size_t capacity() const {
        return _data ? ConstDataView(_data).read<uint32_t>() : 0;
    }

    /**
     * Releases the buffer to the caller. The caller may not free the buffer themselves,
     * and must eventually turn it back into a UniqueBuffer using the reclaim() method.
     */
    char* release() {
        auto ret = _data;
        _data = nullptr;
        return ret + SharedBuffer::kHolderSize;
    }

    // The buffer holder size for 'UniqueBuffer' is the same as the one for 'SharedBuffer'
    static constexpr size_t kHolderSize = SharedBuffer::kHolderSize;

private:
    friend class SharedBuffer;

    // Assumes the size has already been initialized.
    UniqueBuffer(void* buffer) : _data(static_cast<char*>(buffer)) {}

    UniqueBuffer(void* buffer, uint32_t sz) : _data(static_cast<char*>(buffer)) {
        DataView(_data).write<uint32_t>(sz);
    }

    char* _data = nullptr;
};

inline SharedBuffer::SharedBuffer(UniqueBuffer&& other) {
    *this = takeOwnership(other._data, other.capacity());
    other._data = nullptr;
}
}  // namespace mongo
