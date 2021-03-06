/*

Copyright (c) 2014, NVIDIA Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this 
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, 
this list of conditions and the following disclaimer in the documentation 
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED 
OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef TEST_HPP
#define TEST_HPP

#include <synchronic>
#include <mutex>

template <bool truly>
struct dumb_mutex {

    dumb_mutex() : locked(false) {
    }

	dumb_mutex(const dumb_mutex&) = delete;
	dumb_mutex& operator=(const dumb_mutex&) = delete;

    void lock() {

        while(1) {
            bool state = false;
            if(locked.compare_exchange_weak(state,true,std::memory_order_acquire))
                return;
            while(locked.load(std::memory_order_relaxed))
                if(!truly)
                    std::this_thread::yield();
        };
    }

    void unlock() {

        locked.store(false,std::memory_order_release);
    }

private :
    std::atomic<bool> locked;
};

#ifdef WIN32
#include <windows.h>
#include <synchapi.h>
struct srw_mutex {

    srw_mutex () {
        InitializeSRWLock(&_lock);
    }

    void lock() {
        AcquireSRWLockExclusive(&_lock);
    }
    void unlock() {
        ReleaseSRWLockExclusive(&_lock);
    }

private :
    SRWLOCK _lock;
};
#endif

#if defined(__linux__) || (defined(WIN32) && _WIN32_WINNT >= 0x0602)

class simple_mutex
{
    std::atomic<int> word;
public:
    simple_mutex() : word(0) { }
    void lock()
    {
        // try to atimically swap 0 -> 1
        int value1 = 0;
        if (word.compare_exchange_strong(value1, 1, std::memory_order_acquire, std::memory_order_relaxed))
            return; // success
        // wasn't zero -- somebody held the lock
        do {
            int value2 = 1;
            // assume lock is still taken, try to make it 2 and wait
            if (value1 == 2 || word.compare_exchange_strong(value2, 2, std::memory_order_acquire, std::memory_order_relaxed))
                // let's wait, but only if the value is still 2
                std::experimental::__synchronic_wait(&word, 2);    
            // try (again) assuming the lock is free
            value1 = 0;
        } while (!word.compare_exchange_strong(value1, 2, std::memory_order_acquire, std::memory_order_relaxed));
        // we are here only if transition 0 -> 2 succeeded
    }
    void unlock() {
        if (word.fetch_add(-1, std::memory_order_release) != 1) {
             word.store(0, std::memory_order_release);
             std::experimental::__synchronic_wake_one(&word);
        }
    }
};

#endif

struct alignas(64) ttas_mutex {
    ttas_mutex() :  locked(0) { }
	ttas_mutex(const ttas_mutex&) = delete;
	ttas_mutex& operator=(const ttas_mutex&) = delete;
    void lock() {
        while(1) {
            int state = 0;
            if (locked.compare_exchange_weak(state, 1, std::memory_order_acquire))
                return;
            sync.wait_for_change(locked, state, std::memory_order_relaxed);
        }
    }
    void unlock() {
        sync.notify_one(locked, 0, std::memory_order_release);
    }
private :
    std::atomic<int> locked;
    std::experimental::synchronic<int> sync;
};

struct ticket_mutex {
    
    ticket_mutex() : active(0), queue(0) {
    }

	ticket_mutex(const ticket_mutex&) = delete;
	ticket_mutex& operator=(const ticket_mutex&) = delete;

    void lock() {

        int const me = queue.fetch_add(1, std::memory_order_relaxed);
        sync.wait(active, me, std::memory_order_acquire);
    }

    void unlock() {

        sync.notify_all(active, [](std::atomic<int>& atom) {
            atom.fetch_add(1, std::memory_order_release);
        });
    }
private :
    std::atomic<int> active, queue;
    std::experimental::synchronic<int> sync;
};

struct mcs_mutex {
    
    mcs_mutex() : head(nullptr) {
    }

	mcs_mutex(const mcs_mutex&) = delete;
	mcs_mutex& operator=(const mcs_mutex&) = delete;

    struct unique_lock {

        unique_lock(mcs_mutex & m) : m(m), next(nullptr), ready(false) {

            unique_lock * const head = m.head.exchange(this,std::memory_order_acquire);
            if(head != nullptr) {
                
                head->sync_next.notify_one(head->next, this);
                sync_ready.wait(ready, true);
            }
        }
        
	    unique_lock(const unique_lock&) = delete;
	    unique_lock& operator=(const unique_lock&) = delete;

        ~unique_lock() {

            unique_lock * head = this;
            if(!m.head.compare_exchange_strong(head,nullptr,std::memory_order_release, std::memory_order_relaxed)) {

                unique_lock * n = next.load(std::memory_order_acquire);
                if(n == nullptr) {

                    sync_next.wait_for_change(next, nullptr, std::memory_order_acquire);
                }
                n->sync_ready.notify_one(n->ready, true);
            }
        }

    private:
        mcs_mutex & m;
        std::atomic<unique_lock*> next;
        std::atomic<bool> ready;
        std::experimental::synchronic<unique_lock*> sync_next;
        std::experimental::synchronic<bool> sync_ready;
    };

private :
    std::atomic<unique_lock*> head;
};

namespace std {
    template<>
    struct unique_lock<mcs_mutex> : mcs_mutex::unique_lock {
        unique_lock(mcs_mutex & m) : mcs_mutex::unique_lock(m) {
        }
	    unique_lock(const unique_lock&) = delete;
	    unique_lock& operator=(const unique_lock&) = delete;
    };
}

#endif //TEST_HPP
