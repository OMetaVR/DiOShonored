#include "testing.h"
#include "dish/sync.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace dish;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

TEST(critical_section_mutual_exclusion) {
    t::setup_runtime("sync");
    uint32_t cs = t::galloc(24), counter = t::galloc(8);
    t::hle("kernel32.dll", "InitializeCriticalSection", {cs});
    CHECK_EQ(rd32(cs + 4), 0xFFFFFFFFu);                 // LockCount -1 = free
    std::vector<std::thread> ts;
    for (int k = 0; k < 8; k++)
        ts.emplace_back([&] {
            RtThread *me = thread_attach("cs", 0x20000, true);
            for (int i = 0; i < 20000; i++) {
                cs_enter(cs);
                uint32_t v = rd32(counter);       // deliberately non-atomic read-modify-write
                wr32(counter, v + 1);
                cs_leave(cs);
            }
            thread_detach(me);
        });
    for (auto &x : ts) x.join();
    CHECK_EQ(rd32(counter), 160000u);
    CHECK_EQ(rd32(cs + 4), 0xFFFFFFFFu);
}

TEST(critical_section_recursion_and_try) {
    t::setup_runtime("sync");
    uint32_t cs = t::galloc(24);
    cs_init(cs, 0);
    cs_enter(cs);
    cs_enter(cs);
    CHECK_EQ(rd32(cs + 8), 2u);
    CHECK_EQ(rd32(cs + 12), cur_tid());
    CHECK(cs_try_enter(cs));
    cs_leave(cs); cs_leave(cs); cs_leave(cs);
    CHECK_EQ(rd32(cs + 12), 0u);
    std::atomic<int> got{-1};
    cs_enter(cs);
    std::thread th([&] {
        RtThread *me = thread_attach("try", 0x20000, true);
        got = cs_try_enter(cs) ? 1 : 0;
        thread_detach(me);
    });
    th.join();
    CHECK_EQ(got.load(), 0);
    cs_leave(cs);
    auto r = t::hle("kernel32.dll", "TryEnterCriticalSection", {cs});
    CHECK_EQ(r.eax(), 1u);
    t::hle("kernel32.dll", "LeaveCriticalSection", {cs});
}

TEST(events_manual_and_auto) {
    t::setup_runtime("sync");
    uint32_t ev_auto = t::hle("kernel32.dll", "CreateEventW", {0, 0, 0, 0}).eax();
    uint32_t ev_man = t::hle("kernel32.dll", "CreateEventW", {0, 1, 1, 0}).eax();
    CHECK(ev_auto && ev_man);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_auto, 0}).eax(), (uint32_t)WAIT_TIMEOUT);
    t::hle("kernel32.dll", "SetEvent", {ev_auto});
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_auto, 0}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_auto, 0}).eax(), (uint32_t)WAIT_TIMEOUT);   // auto-reset consumed
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_man, 0}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_man, 0}).eax(), 0u);                        // stays signaled
    t::hle("kernel32.dll", "ResetEvent", {ev_man});
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_man, 0}).eax(), (uint32_t)WAIT_TIMEOUT);
    auto t0 = Clock::now();
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_man, 60}).eax(), (uint32_t)WAIT_TIMEOUT);
    CHECK(ms_since(t0) >= 55);
    // wake a blocked waiter from another thread
    std::thread th([&] { sleep_ms(50); auto e = handle_ref<KEvent>(ev_man, KType::Event); event_set(e.get()); });
    t0 = Clock::now();
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {ev_man, 5000}).eax(), 0u);
    CHECK(ms_since(t0) < 2000);
    th.join();
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {0xDEAD0, 0}).eax(), WAIT_FAILED);
    CHECK_EQ(t::hle("kernel32.dll", "CloseHandle", {ev_auto}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "CloseHandle", {ev_auto}).eax(), 0u);        // already closed
    CHECK_EQ(get_last_error(), 6u);
}

TEST(named_objects_are_shared) {
    t::setup_runtime("sync");
    uint32_t name = t::gstr("Local\\DishTestMutex");
    uint32_t h1 = t::hle("kernel32.dll", "CreateMutexA", {0, 0, name}).eax();
    CHECK_EQ(get_last_error(), 0u);
    uint32_t h2 = t::hle("kernel32.dll", "CreateMutexA", {0, 0, name}).eax();
    CHECK_EQ(get_last_error(), 183u);                     // ERROR_ALREADY_EXISTS
    CHECK(h1 != h2);
    CHECK(handle_get(h1) == handle_get(h2));
    // a name is case-insensitive
    uint32_t h3 = t::hle("kernel32.dll", "CreateMutexA", {0, 0, t::gstr("local\\dishtestmutex")}).eax();
    CHECK(h3 != 0);
    CHECK_EQ(get_last_error(), 183u);
    t::hle("kernel32.dll", "CloseHandle", {h1});
    t::hle("kernel32.dll", "CloseHandle", {h2});
    t::hle("kernel32.dll", "CloseHandle", {h3});
}

TEST(mutex_ownership_and_recursion) {
    t::setup_runtime("sync");
    uint32_t m = t::hle("kernel32.dll", "CreateMutexW", {0, 1, 0}).eax();       // initially owned
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {m, 0}).eax(), 0u);  // recursive acquire
    std::atomic<uint32_t> other{99};
    std::thread th([&] {
        RtThread *me = thread_attach("mx", 0x20000, true);
        auto k = handle_ref<KMutex>(m, KType::Mutex);
        KObject *o = k.get();
        other = wait_objects(&o, 1, false, 30, cur_tid());
        thread_detach(me);
    });
    th.join();
    CHECK_EQ(other.load(), (uint32_t)WAIT_TIMEOUT);                              // owned by us
    CHECK_EQ(t::hle("kernel32.dll", "ReleaseMutex", {m}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "ReleaseMutex", {m}).eax(), 1u);
    CHECK_EQ(t::hle("kernel32.dll", "ReleaseMutex", {m}).eax(), 0u);             // not owned any more
    CHECK_EQ(get_last_error(), 288u);
}

TEST(semaphore_counts) {
    t::setup_runtime("sync");
    uint32_t s = t::hle("kernel32.dll", "CreateSemaphoreW", {0, 1, 3, 0}).eax();
    uint32_t prev = t::galloc(4);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {s, 0}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {s, 0}).eax(), (uint32_t)WAIT_TIMEOUT);
    CHECK_EQ(t::hle("kernel32.dll", "ReleaseSemaphore", {s, 2, prev}).eax(), 1u);
    CHECK_EQ(rd32(prev), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "ReleaseSemaphore", {s, 2, prev}).eax(), 0u);   // would exceed max 3
    CHECK_EQ(get_last_error(), 298u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {s, 0}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {s, 0}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForSingleObject", {s, 0}).eax(), (uint32_t)WAIT_TIMEOUT);
}

TEST(wait_multiple_any_and_all) {
    t::setup_runtime("sync");
    uint32_t e1 = t::hle("kernel32.dll", "CreateEventW", {0, 1, 0, 0}).eax();
    uint32_t e2 = t::hle("kernel32.dll", "CreateEventW", {0, 1, 0, 0}).eax();
    uint32_t arr = t::galloc(8);
    wr32(arr, e1);
    wr32(arr + 4, e2);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForMultipleObjects", {2, arr, 0, 0}).eax(), (uint32_t)WAIT_TIMEOUT);
    t::hle("kernel32.dll", "SetEvent", {e2});
    CHECK_EQ(t::hle("kernel32.dll", "WaitForMultipleObjects", {2, arr, 0, 0}).eax(), 1u);         // any -> index
    CHECK_EQ(t::hle("kernel32.dll", "WaitForMultipleObjects", {2, arr, 1, 0}).eax(), (uint32_t)WAIT_TIMEOUT);   // all needs both
    std::thread th([&] { sleep_ms(40); auto e = handle_ref<KEvent>(e1, KType::Event); event_set(e.get()); });
    CHECK_EQ(t::hle("kernel32.dll", "WaitForMultipleObjects", {2, arr, 1, 5000}).eax(), 0u);
    th.join();
    CHECK_EQ(t::hle("kernel32.dll", "WaitForMultipleObjectsEx", {2, arr, 0, 0, 0}).eax(), 0u);
    CHECK_EQ(t::hle("kernel32.dll", "WaitForMultipleObjects", {0, arr, 0, 0}).eax(), WAIT_FAILED);
}

TEST(producer_consumer_with_semaphore_and_events) {
    t::setup_runtime("sync");
    uint32_t sem = t::hle("kernel32.dll", "CreateSemaphoreW", {0, 0, 1000000, 0}).eax();
    std::atomic<int> consumed{0};
    std::vector<std::thread> cons;
    for (int k = 0; k < 4; k++)
        cons.emplace_back([&] {
            RtThread *me = thread_attach("c", 0x20000, true);
            auto s = handle_ref<KSemaphore>(sem, KType::Semaphore);
            KObject *o = s.get();
            while (wait_objects(&o, 1, false, 2000, cur_tid()) == 0) consumed++;
            thread_detach(me);
        });
    auto s = handle_ref<KSemaphore>(sem, KType::Semaphore);
    for (int i = 0; i < 4000; i++) semaphore_release(s.get(), 1, nullptr);
    for (auto &x : cons) x.join();
    CHECK_EQ(consumed.load(), 4000);
}

TEST(sleep_and_tick_counters) {
    t::setup_runtime("sync");
    uint32_t a = t::hle("kernel32.dll", "GetTickCount", {}).eax();
    auto t0 = Clock::now();
    t::hle("kernel32.dll", "Sleep", {80});
    CHECK(ms_since(t0) >= 75);
    uint32_t b = t::hle("kernel32.dll", "GetTickCount", {}).eax();
    CHECK(b - a >= 70 && b - a < 500);
    uint32_t qf = t::galloc(8), qc = t::galloc(16);
    t::hle("kernel32.dll", "QueryPerformanceFrequency", {qf});
    CHECK_EQ(rd64(qf), 10000000ull);
    t::hle("kernel32.dll", "QueryPerformanceCounter", {qc});
    t::hle("kernel32.dll", "Sleep", {20});
    t::hle("kernel32.dll", "QueryPerformanceCounter", {qc + 8});
    uint64_t dt = rd64(qc + 8) - rd64(qc);
    CHECK(dt >= 150000 && dt < 5000000);                     // 20 ms at 10 MHz
    uint32_t tg = t::hle("winmm.dll", "timeGetTime", {}).eax();
    CHECK(tg >= b);
}

TEST(interlocked_and_slist) {
    t::setup_runtime("sync");
    uint32_t v = t::galloc(8);
    wr32(v, 5);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedIncrement", {v}).eax(), 6u);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedDecrement", {v}).eax(), 5u);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedExchange", {v, 9}).eax(), 5u);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedExchangeAdd", {v, 3}).eax(), 9u);
    CHECK_EQ(rd32(v), 12u);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedCompareExchange", {v, 100, 12}).eax(), 12u);   // returns the old value
    CHECK_EQ(rd32(v), 100u);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedCompareExchange", {v, 1, 5}).eax(), 100u);      // mismatch: unchanged
    CHECK_EQ(rd32(v), 100u);
    // SLIST
    uint32_t head = t::galloc(16), e1 = t::galloc(8), e2 = t::galloc(8);
    head = (head + 7) & ~7u;
    t::hle("kernel32.dll", "InitializeSListHead", {head});
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedPopEntrySList", {head}).eax(), 0u);
    t::hle("kernel32.dll", "InterlockedPushEntrySList", {head, e1});
    t::hle("kernel32.dll", "InterlockedPushEntrySList", {head, e2});
    CHECK_EQ(t::hle("kernel32.dll", "QueryDepthSList", {head}).eax(), 2u);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedPopEntrySList", {head}).eax(), e2);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedFlushSList", {head}).eax(), e1);
    CHECK_EQ(t::hle("kernel32.dll", "InterlockedPopEntrySList", {head}).eax(), 0u);
}

TEST(srw_lock_and_condvar) {
    t::setup_runtime("sync");
    uint32_t lock = t::galloc(4), counter = t::galloc(4);
    t::hle("kernel32.dll", "InitializeSRWLock", {lock});
    std::vector<std::thread> ts;
    for (int k = 0; k < 4; k++)
        ts.emplace_back([&] {
            RtThread *me = thread_attach("srw", 0x20000, true);
            for (int i = 0; i < 5000; i++) {
                t::hle("kernel32.dll", "AcquireSRWLockExclusive", {lock});
                wr32(counter, rd32(counter) + 1);
                t::hle("kernel32.dll", "ReleaseSRWLockExclusive", {lock});
            }
            thread_detach(me);
        });
    for (auto &x : ts) x.join();
    CHECK_EQ(rd32(counter), 20000u);
}
