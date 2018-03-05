/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "ExecutableAllocator.h"

#include "JSCInlines.h"

#if ENABLE(EXECUTABLE_ALLOCATOR_DEMAND)
#include "CodeProfiling.h"
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MetaAllocator.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PageReservation.h>
#include <wtf/VMTags.h>
#endif

// Uncomment to create an artificial executable memory usage limit. This limit
// is imperfect and is primarily useful for testing the VM's ability to handle
// out-of-executable-memory situations.
// #define EXECUTABLE_MEMORY_LIMIT 1000000

#if ENABLE(ASSEMBLER)

using namespace WTF;

namespace JSC {

#if ENABLE(EXECUTABLE_ALLOCATOR_DEMAND)

class DemandExecutableAllocator : public MetaAllocator {
public:
    DemandExecutableAllocator()
        : MetaAllocator(jitAllocationGranule)
    {
        std::lock_guard<StaticLock> lock(allocatorsMutex());
        allocators().add(this);
        // Don't preallocate any memory here.
    }
    
    virtual ~DemandExecutableAllocator()
    {
        {
            std::lock_guard<StaticLock> lock(allocatorsMutex());
            allocators().remove(this);
        }
        for (unsigned i = 0; i < reservations.size(); ++i)
            reservations.at(i).deallocate();
    }

    static size_t bytesAllocatedByAllAllocators()
    {
        size_t total = 0;
        std::lock_guard<StaticLock> lock(allocatorsMutex());
        for (HashSet<DemandExecutableAllocator*>::const_iterator allocator = allocators().begin(); allocator != allocators().end(); ++allocator)
            total += (*allocator)->bytesAllocated();
        return total;
    }

    static size_t bytesCommittedByAllocactors()
    {
        size_t total = 0;
        std::lock_guard<StaticLock> lock(allocatorsMutex());
        for (HashSet<DemandExecutableAllocator*>::const_iterator allocator = allocators().begin(); allocator != allocators().end(); ++allocator)
            total += (*allocator)->bytesCommitted();
        return total;
    }

#if ENABLE(META_ALLOCATOR_PROFILE)
    static void dumpProfileFromAllAllocators()
    {
        std::lock_guard<StaticLock> lock(allocatorsMutex());
        for (HashSet<DemandExecutableAllocator*>::const_iterator allocator = allocators().begin(); allocator != allocators().end(); ++allocator)
            (*allocator)->dumpProfile();
    }
#endif

protected:
    virtual void* allocateNewSpace(size_t& numPages)
    {
        size_t newNumPages = (((numPages * pageSize() + JIT_ALLOCATOR_LARGE_ALLOC_SIZE - 1) / JIT_ALLOCATOR_LARGE_ALLOC_SIZE * JIT_ALLOCATOR_LARGE_ALLOC_SIZE) + pageSize() - 1) / pageSize();
        
        ASSERT(newNumPages >= numPages);
        
        numPages = newNumPages;
        
#ifdef EXECUTABLE_MEMORY_LIMIT
        if (bytesAllocatedByAllAllocators() >= EXECUTABLE_MEMORY_LIMIT)
            return 0;
#endif
        
        PageReservation reservation = PageReservation::reserve(numPages * pageSize(), OSAllocator::JSJITCodePages, EXECUTABLE_POOL_WRITABLE, true);
        RELEASE_ASSERT(reservation);
        
        reservations.append(reservation);
        
        return reservation.base();
    }
    
    virtual void notifyNeedPage(void* page)
    {
        OSAllocator::commit(page, pageSize(), EXECUTABLE_POOL_WRITABLE, true);
    }
    
    virtual void notifyPageIsFree(void* page)
    {
        OSAllocator::decommit(page, pageSize());
    }

private:
    Vector<PageReservation, 16> reservations;
    static HashSet<DemandExecutableAllocator*>& allocators()
    {
        static NeverDestroyed<HashSet<DemandExecutableAllocator*>> set;
        return set;
    }

    static StaticLock& allocatorsMutex()
    {
        static StaticLock mutex;

        return mutex;
    }
};

static DemandExecutableAllocator* gAllocator;

namespace {
static inline DemandExecutableAllocator* allocator()
{
    return gAllocator;
}
}

void ExecutableAllocator::initializeAllocator()
{
    ASSERT(!gAllocator);
    gAllocator = new DemandExecutableAllocator();
    CodeProfiling::notifyAllocator(gAllocator);
}

ExecutableAllocator::ExecutableAllocator(VM&)
{
    ASSERT(allocator());
}

ExecutableAllocator::~ExecutableAllocator()
{
}

bool ExecutableAllocator::isValid() const
{
    return true;
}

bool ExecutableAllocator::underMemoryPressure()
{
#ifdef EXECUTABLE_MEMORY_LIMIT
    return DemandExecutableAllocator::bytesAllocatedByAllAllocators() > EXECUTABLE_MEMORY_LIMIT / 2;
#else
    return false;
#endif
}

double ExecutableAllocator::memoryPressureMultiplier(size_t addedMemoryUsage)
{
    double result;
#ifdef EXECUTABLE_MEMORY_LIMIT
    size_t bytesAllocated = DemandExecutableAllocator::bytesAllocatedByAllAllocators() + addedMemoryUsage;
    if (bytesAllocated >= EXECUTABLE_MEMORY_LIMIT)
        bytesAllocated = EXECUTABLE_MEMORY_LIMIT;
    result = static_cast<double>(EXECUTABLE_MEMORY_LIMIT) /
        (EXECUTABLE_MEMORY_LIMIT - bytesAllocated);
#else
    UNUSED_PARAM(addedMemoryUsage);
    result = 1.0;
#endif
    if (result < 1.0)
        result = 1.0;
    return result;

}

RefPtr<ExecutableMemoryHandle> ExecutableAllocator::allocate(VM&, size_t sizeInBytes, void* ownerUID, JITCompilationEffort effort)
{
    RefPtr<ExecutableMemoryHandle> result = allocator()->allocate(sizeInBytes, ownerUID);
    RELEASE_ASSERT(result || effort != JITCompilationMustSucceed);
    return result;
}

size_t ExecutableAllocator::committedByteCount()
{
    return DemandExecutableAllocator::bytesCommittedByAllocactors();
}

#if ENABLE(META_ALLOCATOR_PROFILE)
void ExecutableAllocator::dumpProfile()
{
    DemandExecutableAllocator::dumpProfileFromAllAllocators();
}
#endif

Lock& ExecutableAllocator::getLock() const
{
    return gAllocator->getLock();
}

bool ExecutableAllocator::isValidExecutableMemory(const LockHolder& locker, void* address)
{
    return gAllocator->isInAllocatedMemory(locker, address);
}

#endif // ENABLE(EXECUTABLE_ALLOCATOR_DEMAND)

}

#endif // HAVE(ASSEMBLER)
