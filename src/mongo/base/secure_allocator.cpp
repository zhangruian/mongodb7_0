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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/secure_allocator.h"

#include <memory>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "mongo/base/init.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/secure_zero_memory.h"
#include "mongo/util/text.h"

namespace mongo {

namespace {

/**
 * NOTE(jcarey): Why not new/delete?
 *
 * As a general rule, mlock/virtuallock lack any kind of recursive semantics
 * (they free any locks on the underlying page if called once). While some
 * platforms do offer those semantics, they're not available globally, so we
 * have to flow all allocations through page based allocations.
 */
#ifdef _WIN32

/**
 * Enable a privilege in the current process.
 */
void EnablePrivilege(const wchar_t* name) {
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        auto str = errnoWithPrefix("Failed to LookupPrivilegeValue");
        LOGV2_WARNING(23704, "{str}", "str"_attr = str);
        return;
    }

    // Get the access token for the current process.
    HANDLE accessToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &accessToken)) {
        auto str = errnoWithPrefix("Failed to OpenProcessToken");
        LOGV2_WARNING(23705, "{str}", "str"_attr = str);
        return;
    }

    const auto accessTokenGuard = makeGuard([&] { CloseHandle(accessToken); });

    TOKEN_PRIVILEGES privileges = {0};

    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(
            accessToken, false, &privileges, sizeof(privileges), nullptr, nullptr)) {
        auto str = errnoWithPrefix("Failed to AdjustTokenPrivileges");
        LOGV2_WARNING(23706, "{str}", "str"_attr = str);
    }

    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        LOGV2_WARNING(23707,
                      "Failed to adjust token privilege for privilege '{toUtf8String_name}'",
                      "toUtf8String_name"_attr = toUtf8String(name));
    }
}

/**
 * Lock to ensure calls to grow our working set size are serialized.
 *
 * The lock is needed since we are doing a two step process of querying the currently working set
 * size, and then raising the working set. This is the same reason that "i++" has race conditions
 * across multiple threads.
 */
stdx::mutex workingSizeMutex;  // NOLINT

/**
 * There is a minimum gap between the minimum working set size and maximum working set size.
 * On Windows 2008 R2, it is 0x9000 bytes. On Windows 10, 0x7000 bytes.
 */
constexpr size_t minGap = 0x9000;

/**
 * Grow the minimum working set size of the process to the specified size.
 */
void growWorkingSize(std::size_t bytes) {
    size_t minWorkingSetSize;
    size_t maxWorkingSetSize;

    stdx::lock_guard<stdx::mutex> lock(workingSizeMutex);

    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &minWorkingSetSize, &maxWorkingSetSize)) {
        auto str = errnoWithPrefix("Failed to GetProcessWorkingSetSize");
        LOGV2_FATAL(40285, "{str}", "str"_attr = str);
    }

    // Since allocation request is aligned to page size, we can just add it to the current working
    // set size.
    maxWorkingSetSize = std::max(minWorkingSetSize + bytes + minGap, maxWorkingSetSize);

    // Increase the working set size minimum to the new lower bound.
    if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(),
                                    minWorkingSetSize + bytes,
                                    maxWorkingSetSize,
                                    QUOTA_LIMITS_HARDWS_MIN_ENABLE |
                                        QUOTA_LIMITS_HARDWS_MAX_DISABLE)) {
        auto str = errnoWithPrefix("Failed to SetProcessWorkingSetSizeEx");
        LOGV2_FATAL(40286, "{str}", "str"_attr = str);
    }
}

void* systemAllocate(std::size_t bytes) {
    // Flags:
    //
    // MEM_COMMIT - allocates the memory charges and zeros the underlying
    //              memory
    // MEM_RESERVE - Reserves space in the process's virtual address space
    //
    // The two flags together give us bytes that are attached to the process
    // that we can actually write to.
    //
    // PAGE_READWRITE - allows read/write access to the page
    auto ptr = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!ptr) {
        auto str = errnoWithPrefix("Failed to VirtualAlloc");
        LOGV2_FATAL(28835, "{str}", "str"_attr = str);
    }

    if (VirtualLock(ptr, bytes) == 0) {
        DWORD gle = GetLastError();

        // Try to grow the working set if we have hit our quota.
        if (gle == ERROR_WORKING_SET_QUOTA) {
            growWorkingSize(bytes);

            if (VirtualLock(ptr, bytes) != 0) {
                return ptr;
            }
        }

        auto str = errnoWithPrefix("Failed to VirtualLock");
        LOGV2_FATAL(28828, "{str}", "str"_attr = str);
    }

    return ptr;
}

void systemDeallocate(void* ptr, std::size_t bytes) {
    if (VirtualUnlock(ptr, bytes) == 0) {
        auto str = errnoWithPrefix("Failed to VirtualUnlock");
        LOGV2_FATAL(28829, "{str}", "str"_attr = str);
    }

    // VirtualFree needs to take 0 as the size parameter for MEM_RELEASE
    // (that's how the api works).
    if (VirtualFree(ptr, 0, MEM_RELEASE) == 0) {
        auto str = errnoWithPrefix("Failed to VirtualFree");
        LOGV2_FATAL(28830, "{str}", "str"_attr = str);
    }
}

#else

// See https://github.com/libressl-portable/portable/issues/24 for the table
// that suggests this approach. This assumes that MAP_ANONYMOUS and MAP_ANON are
// macro definitions, but that seems plausible on all platforms we care about.

#if defined(MAP_ANONYMOUS)
#define MONGO_MAP_ANONYMOUS MAP_ANONYMOUS
#else
#if defined(MAP_ANON)
#define MONGO_MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if !defined(MONGO_MAP_ANONYMOUS)
#error "Could not determine a way to map anonymous memory, required for secure allocation"
#endif

void* systemAllocate(std::size_t bytes) {
    // Flags:
    //
    // PROT_READ | PROT_WRITE - allows read write access to the page
    //
    // MAP_PRIVATE - Ensure that the mapping is copy-on-write. Otherwise writes
    //               made in this process can be seen in children.
    //
    // MAP_ANONYMOUS - The mapping is not backed by a file. fd must be -1 on
    //                 some platforms, offset is ignored (so 0).
    //
    // skipping flags like MAP_LOCKED and MAP_POPULATE as linux-isms
    auto ptr =
        mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MONGO_MAP_ANONYMOUS, -1, 0);

    if (!ptr) {
        auto str = errnoWithPrefix("Failed to mmap");
        LOGV2_FATAL(23714, "{str}", "str"_attr = str);
        fassertFailed(28831);
    }

    if (mlock(ptr, bytes) != 0) {
        auto str = errnoWithPrefix(
            "Failed to mlock: Cannot allocate locked memory. For more details see: "
            "https://dochub.mongodb.org/core/cannot-allocate-locked-memory");
        LOGV2_FATAL(23715, "{str}", "str"_attr = str);
        fassertFailed(28832);
    }

#if defined(MADV_DONTDUMP)
    // We deliberately ignore the return value since if the Linux version is < 3.4, madvise
    // will fail since MADV_DONTDUMP is not supported.
    (void)madvise(ptr, bytes, MADV_DONTDUMP);
#endif

    return ptr;
}

void systemDeallocate(void* ptr, std::size_t bytes) {
#if defined(MADV_DONTDUMP) && defined(MADV_DODUMP)
    // See comment above madvise in systemAllocate().
    (void)madvise(ptr, bytes, MADV_DODUMP);
#endif

    if (munlock(ptr, bytes) != 0) {
        LOGV2_FATAL(28833,
                    "{errnoWithPrefix_Failed_to_munlock}",
                    "errnoWithPrefix_Failed_to_munlock"_attr =
                        errnoWithPrefix("Failed to munlock"));
    }

    if (munmap(ptr, bytes) != 0) {
        LOGV2_FATAL(28834,
                    "{errnoWithPrefix_Failed_to_munmap}",
                    "errnoWithPrefix_Failed_to_munmap"_attr = errnoWithPrefix("Failed to munmap"));
    }
}

#endif

/**
 * Each page represent one call to mmap+mlock / VirtualAlloc+VirtualLock on construction and will
 * match with an equivalent unlock and unmap on destruction.  Pages are rounded up to the nearest
 * page size and allocate returns aligned pointers.
 */
class Allocation {
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;

public:
    explicit Allocation(std::size_t initialAllocation) {
        auto pageSize = ProcessInfo::getPageSize();
        std::size_t remainder = initialAllocation % pageSize;

        _size = _remaining =
            remainder ? initialAllocation + pageSize - remainder : initialAllocation;
        _start = _ptr = systemAllocate(_size);
    }

    ~Allocation() {
        systemDeallocate(_start, _size);
    }

    /**
     * Allocates an aligned pointer of given size from the locked page we have a pointer to.
     *
     * Returns null if the request can't be satisifed.
     */
    void* allocate(std::size_t size, std::size_t alignOf) {
        if (std::align(alignOf, size, _ptr, _remaining)) {
            auto result = _ptr;
            _ptr = static_cast<char*>(_ptr) + size;
            _remaining -= size;

            return result;
        }

        // We'll make a new allocation if this one doesn't have enough room
        return nullptr;
    }

private:
    void* _start;            // Start of the allocation
    void* _ptr;              // Start of unallocated bytes
    std::size_t _size;       // Total size
    std::size_t _remaining;  // Remaining bytes
};

// See secure_allocator_details::allocate for a more detailed comment on what these are used for
stdx::mutex allocatorMutex;  // Protects the values below
stdx::unordered_map<void*, std::shared_ptr<Allocation>> secureTable;
std::shared_ptr<Allocation> lastAllocation = nullptr;

}  // namespace

MONGO_INITIALIZER_GENERAL(SecureAllocator, (), ())
(InitializerContext* context) {
#if _WIN32
    // Enable the increase working set size privilege in our access token.
    EnablePrivilege(SE_INC_WORKING_SET_NAME);
#endif
}

namespace secure_allocator_details {

/**
 * To save on allocations, we try to serve multiple requests out of the same mlocked page where
 * possible.  We do this by invoking the system allocator in multiples of a full page, and keep the
 * last page around, giving out pointers from that page if its possible to do so.  We also keep an
 * unordered_map of all the allocations we've handed out, which hold shared_ptrs that get rid of
 * pages when we're not using them anymore.
 */
void* allocate(std::size_t bytes, std::size_t alignOf) {
    stdx::lock_guard<stdx::mutex> lk(allocatorMutex);

    if (lastAllocation) {
        auto out = lastAllocation->allocate(bytes, alignOf);

        if (out) {
            secureTable[out] = lastAllocation;
            return out;
        }
    }

    lastAllocation = std::make_shared<Allocation>(bytes);
    auto out = lastAllocation->allocate(bytes, alignOf);
    secureTable[out] = lastAllocation;
    return out;
}

/**
 * Deallocates a secure allocation.
 *
 * We zero memory before derefing the associated allocation.
 */
void deallocate(void* ptr, std::size_t bytes) {
    secureZeroMemory(ptr, bytes);

    stdx::lock_guard<stdx::mutex> lk(allocatorMutex);

    secureTable.erase(ptr);
}

}  // namespace secure_allocator_details

constexpr StringData SecureAllocatorAuthDomainTrait::DomainType;

}  // namespace mongo
