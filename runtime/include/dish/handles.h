// Kernel objects and the process-wide handle table.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <vector>

namespace dish {

enum class KType : uint8_t { None, Event, Mutex, Semaphore, Thread, Process, File, Mapping, Find, Timer, Key, Other };

constexpr uint32_t HANDLE_CUR_PROCESS = 0xFFFFFFFFu;
constexpr uint32_t HANDLE_CUR_THREAD = 0xFFFFFFFEu;
constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFFu;

struct Waiter;

struct KObject {
    std::atomic<int> ref{1};
    KType type = KType::None;
    std::string name;              // for named objects (lower-cased); empty = anonymous
    std::vector<Waiter *> waiters; // guarded by the sync mutex
    explicit KObject(KType t) : type(t) {}
    virtual ~KObject() = default;
    virtual bool waitable() const { return false; }
    // Called with the sync mutex held. Returns true if the wait is satisfied for thread `tid` (and consumes the signal).
    virtual bool try_acquire(uint32_t tid) { (void)tid; return false; }
    // Called with the sync mutex held. Would try_acquire succeed? (no side effects)
    virtual bool is_signaled(uint32_t tid) const { (void)tid; return false; }
    void add_ref() { ref.fetch_add(1, std::memory_order_relaxed); }
    void release();
};

// Intrusive smart pointer
template <class T> class KRef {
    T *p_ = nullptr;
  public:
    KRef() = default;
    explicit KRef(T *p) : p_(p) {}   // adopts one reference
    KRef(const KRef &o) : p_(o.p_) { if (p_) p_->add_ref(); }
    KRef(KRef &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    KRef &operator=(KRef o) { std::swap(p_, o.p_); return *this; }
    ~KRef() { if (p_) p_->release(); }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T *detach() { T *t = p_; p_ = nullptr; return t; }
};

// Handle table. Handles are small multiples of 4. A table entry owns one reference.
uint32_t handle_alloc(KObject *obj);                    // adopts the caller's reference; returns the handle
KObject *handle_get(uint32_t h);                        // returns a new reference or nullptr
template <class T> KRef<T> handle_ref(uint32_t h, KType t) {
    KObject *o = handle_get(h);
    if (o && o->type != t) { o->release(); o = nullptr; }
    return KRef<T>(static_cast<T *>(o));
}
bool handle_close(uint32_t h);
uint32_t handle_dup(uint32_t h);                        // new handle for the same object, 0 on failure
size_t handle_count();
void handle_table_reset();                              // tests

// Named-object namespace (CreateEvent/Mutex/Semaphore/FileMapping with a name)
KObject *named_lookup(KType t, const std::string &lname);   // new reference or nullptr
void named_insert(KObject *obj);
void named_remove(KObject *obj);

}  // namespace dish
