// Temporary boot-debugging hooks: log who calls a function and with which arguments, then call the original.
// Each entry says which crash it is chasing; remove it once the cause is fixed. Empty while no crash needs one.
//
// Pattern:
//   extern "C" REX_FUNC(__imp__sub_XXXXXXXX);
//   REX_HOOK_RAW(sub_XXXXXXXX) {
//     REXLOG_WARN("[dbg] sub_XXXXXXXX(r3={:08X}) from lr={:08X}", ctx.r3.u32, uint32_t(ctx.lr));
//     __imp__sub_XXXXXXXX(ctx, base);
//   }

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/xthread.h>

namespace {
inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// Heap operations still to log in full by the tracker below: all of them while the first singleton setup
// (sub_822C5C08) runs, then 3000 more, to compare the PC and Switch histories op by op.
std::atomic<int> g_heap_trace{0};
std::atomic<bool> g_heap_trace_started{false};
bool HeapTraced() { return g_heap_trace.load(std::memory_order_relaxed) > 0 && g_heap_trace.fetch_sub(1) > 0; }
}  // namespace

// Switch crash 2026-10-05 (rex_crash.log: RtlEnterCriticalSection on a null critical section, from sub_822E9420 <-
// sub_822E8E60 <- sub_82145670 at 0x821456B4): the release of a ref-counted string handle whose block has no lock.
// On the PC the field released there holds the empty-string sentinel 0x828F6340 (no lock is taken); on the Switch it
// held a block whose lock pointer is 0. Diagnostic workaround until the cause is known: such a release is logged with
// the memory around it and skipped (the string is leaked) instead of handing a null lock to RtlEnterCriticalSection.
extern "C" REX_FUNC(__imp__sub_822E9420);
REX_HOOK_RAW(sub_822E9420) {
  static std::atomic<int> logged{0};
  const uint32_t obj = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const bool plausible = obj >= 0x40000000u && obj < 0xA0000000u;
  const uint32_t handle = plausible ? Load32(base, obj) : 0;
  const bool real_block = handle && handle != 0x828F6340u &&
                          ((handle >= 0x40000000u && handle < 0xA0000000u) || (handle >= 0x82000000u && handle < 0x83000000u));
  const uint32_t cs = real_block ? Load32(base, handle) : 0;
  const bool bad = handle != 0x828F6340u && (!real_block || !cs);
  if (bad) {
    std::string around, block;
    for (int i = -8; i < 8 && plausible; ++i) around += fmt::format(" {:08X}", Load32(base, obj + 4 * i));
    for (int i = 0; i < 6 && real_block; ++i) block += fmt::format(" {:08X}", Load32(base, handle + 4 * i));
    REXLOG_ERROR("[dbg] string release with no lock: obj {:08X} handle {:08X} lock {:08X} from lr {:08X} r1 {:08X}; "
                 "obj-32..obj+28:{}; block:{} -- release skipped", obj, handle, cs, lr, ctx.r1.u32, around, block);
    rex::FlushLogging();
    return;
  }
  if (lr == 0x822E8E78u && logged.fetch_add(1) < 8) {
    REXLOG_INFO("[dbg] string release obj {:08X} handle {:08X} lock {:08X} from lr {:08X}", obj, handle, cs, lr);
  }
  __imp__sub_822E9420(ctx, base);
}

// Same crash: is field +96 of the 392-byte object right after its constructor (sub_822E1B50 stores the empty-string
// sentinel there), and which objects does the singleton setup (sub_822C5C08, first call of sub_82145670) create? On
// the PC: inner object at 0x414B9AF0, field +96 = 0x828F6340. Logged for the first few objects only.
extern "C" REX_FUNC(__imp__sub_822E1B50);
REX_HOOK_RAW(sub_822E1B50) {
  static std::atomic<int> logged{0};
  const uint32_t self = ctx.r3.u32;
  __imp__sub_822E1B50(ctx, base);
  // Publish the constructed object before anything can publish a pointer to it (see sub_822CAF38 below).
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const uint32_t field = (self >= 0x40000000u && self < 0xA0000000u) ? Load32(base, self + 96) : 0;
  if (field != 0x828F6340u || logged.fetch_add(1) < 4) {
    REXLOG_INFO("[dbg] 392-byte object constructed at {:08X}: field +96 = {:08X}{}", self, field,
                field != 0x828F6340u ? " *** NOT THE SENTINEL ***" : "");
    if (field != 0x828F6340u) rex::FlushLogging();
  }
}

// Candidate fix for the Switch crash: the singleton setup sub_822C5C08 is not thread-safe (check the pointer at
// 0x82D8D79C, new + construct, store the pointer; no barrier) and is reached from two places (sub_82140460,
// sub_82145670), both through pointers. On x86 another thread always sees the constructor's stores before the
// pointer; on the Switch's ARM cores the pointer can become visible first, and a reader then finds field +96 of the
// inner object unwritten: exactly the crash. A full fence at the end of both constructors orders every store of the
// object before the pointer store that follows them (the reader's loads through the pointer are address-dependent,
// which ARM keeps in order).
extern "C" REX_FUNC(__imp__sub_822CAF38);
REX_HOOK_RAW(sub_822CAF38) {  // the singleton's constructor (builds the 392-byte inner object first)
  __imp__sub_822CAF38(ctx, base);
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

extern "C" REX_FUNC(__imp__sub_822C5C08);
REX_HOOK_RAW(sub_822C5C08) {
  const bool first = !g_heap_trace_started.exchange(true);
  if (first) g_heap_trace.store(100000);
  __imp__sub_822C5C08(ctx, base);
  if (first) g_heap_trace.store(3000);
  const uint32_t singleton = Load32(base, 0x82D8D79Cu);
  const uint32_t inner = (singleton >= 0x40000000u && singleton < 0xA0000000u) ? Load32(base, singleton) : 0;
  const uint32_t field = (inner >= 0x40000000u && inner < 0xA0000000u) ? Load32(base, inner + 96) : 0;
  REXLOG_INFO("[dbg] singleton {:08X}: inner object {:08X}, its field +96 = {:08X}", singleton, inner, field);
  // Heap headers of both (16 bytes before each: size in 16-byte units, previous size, segment, flags, unused bytes)
  // and the inner object's first 128 bytes, to compare with the same dump on the PC.
  if (inner >= 0x40000010u && inner < 0xA0000000u && singleton >= 0x40000010u && singleton < 0xA0000000u) {
    std::string words;
    for (uint32_t i = 0; i < 36; ++i) words += fmt::format(" {:08X}", Load32(base, inner - 16 + 4 * i));
    REXLOG_INFO("[dbg] singleton header {:08X} {:08X}; inner object from its header:{}", Load32(base, singleton - 16),
                Load32(base, singleton - 12), words);
  }
  rex::FlushLogging();
}

// Switch crash 2026-10-06 (second console log): the guest heap hands out memory that is still in use. The 392-byte
// object above and the 36-byte singleton get the same addresses as on the PC (0x414B9AF0, 0x4155F990), but on the
// Switch a 128-byte block's heap header sits inside the live 392-byte object, and XAudio2's 44-byte XMemAlloc
// buffer ("Xbox 360 audio device") lands on the singleton. malloc (sub_825EEA68) and XMemAlloc's heap path
// (sub_823198C0) both end in the game's own NT heap: RtlAllocateHeap sub_8231BAE0(heap, flags, size),
// RtlFreeHeap sub_8231C3D8(heap, flags, ptr), RtlReAllocateHeap sub_8231C6D0(heap, flags, ptr, size), serialised by
// RtlEnterCriticalSection on *(heap + 1408) unless HEAP_NO_SERIALIZE (bit 0 of the flags at heap + 24).
//
// This tracker keeps every live block on the host side and reports, with both owners:
//  - an allocation that overlaps a block that is still live (the heap returned memory twice);
//  - a free of a block that is not live (double free, or a pointer the heap never returned);
//  - a heap call entered while the heap's critical section already names this thread as owner although the thread
//    is not inside a heap call (a leaked lock, or two threads with the same guest identity: either one lets two
//    threads into the heap at once);
// and logs every allocation and free touching the two objects above, so the PC and Switch histories can be compared.
namespace heaptrack {

struct Block {
  uint32_t size;
  uint32_t heap;
  uint32_t lr;
  uint32_t thread;
  uint32_t seq;
};

std::mutex g_mutex;
std::map<uint32_t, Block> g_live;
uint32_t g_seq = 0;
std::atomic<int> g_overlaps{0}, g_bad_frees{0}, g_lock_reports{0};
thread_local int t_depth = 0;
thread_local uint32_t t_outer_lr = 0;
// The caller of the outermost allocator entry point (operator new, malloc, free, XMemAlloc, XMemFree): the lr seen
// by the heap functions is inside malloc or free, which says nothing.
thread_local int t_api_depth = 0;
thread_local uint32_t t_api_lr = 0;

struct ApiScope {
  explicit ApiScope(uint32_t lr) {
    if (t_api_depth++ == 0) t_api_lr = lr;
  }
  ~ApiScope() { --t_api_depth; }
};

uint32_t CallerLr(uint32_t heap_lr) { return t_api_depth ? t_api_lr : heap_lr; }

constexpr int kMaxReports = 24;

uint32_t ThreadHandle() {
  return rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread()->handle() : 0;
}
uint32_t ThreadObject() {
  return rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread()->guest_object() : 0;
}

// Return addresses from the guest stack back chain (back chain word at 0(r1), saved LR at back chain - 8). Functions
// that save LR with the __savegprlr helpers have no saved LR there (the recompiler drops the helper), hence zeros.
// Only inside this thread's stack (KTHREAD stack_base +0x5C, the high end, and stack_limit +0x60): the frame above
// the outermost one points at the unmapped page past the stack.
std::string GuestStack(const uint8_t* base, uint32_t sp) {
  // The KTHREAD is in the kernel's system heap (0x3002A018 for the main thread on the PC): virtual memory.
  const uint32_t kthread = ThreadObject();
  if (kthread < 0x10000u || kthread >= 0x7F000000u) return "";
  const uint32_t high = Load32(base, kthread + 0x5C), low = Load32(base, kthread + 0x60);
  std::string s;
  for (int i = 0; i < 8; ++i) {
    if (sp < low || sp >= high) break;
    const uint32_t next = Load32(base, sp);
    if (next <= sp || next >= high) break;
    s += fmt::format(" {:08X}", Load32(base, next - 8));
    sp = next;
  }
  return s;
}

bool Watched(uint32_t addr, uint32_t size) {
  const uint32_t end = addr + (size ? size : 1);
  return (end > 0x414B9A00u && addr < 0x414B9D00u) || (end > 0x4155F900u && addr < 0x4155FA00u);
}

std::string Header(const uint8_t* base, uint32_t user) {
  if (user < 0x40000010u || user >= 0xA0000000u) return "-";
  return fmt::format("{:08X} {:08X} {:08X} {:08X}", Load32(base, user - 16), Load32(base, user - 12),
                     Load32(base, user - 8), Load32(base, user - 4));
}

// Before a heap call made from outside the heap: does the heap's lock already name this thread?
void CheckLockOnEntry(const char* what, const uint8_t* base, uint32_t heap, uint32_t lr) {
  if (t_depth != 0 || heap < 0x40000000u || heap >= 0xA0000000u) return;
  if (Load32(base, heap + 24) & 1) return;  // HEAP_NO_SERIALIZE: the caller locks
  const uint32_t cs = Load32(base, heap + 1408);
  if (cs < 0x40000000u || cs >= 0xA0000000u) return;
  const uint32_t owner = Load32(base, cs + 24), self = ThreadObject();
  if (!owner || owner != self) return;
  if (g_lock_reports.fetch_add(1) < kMaxReports) {
    int32_t lock_count;
    std::memcpy(&lock_count, base + cs + 16, 4);  // host byte order (xboxkrnl_rtl.cpp)
    REXLOG_ERROR("[heap] {} entered from lr {:08X} on thread {:08X} (guest object {:08X}) while heap {:08X}'s lock "
                 "{:08X} already names this thread as owner (lock_count {}, recursion {}) and the thread is not "
                 "inside a heap call: leaked lock or shared guest identity",
                 what, lr, ThreadHandle(), self, heap, cs, lock_count, Load32(base, cs + 20));
    rex::FlushLogging();
  }
}

void NoteAlloc(const uint8_t* base, const char* what, uint32_t heap, uint32_t addr, uint32_t size, uint32_t lr,
               uint32_t sp) {
  const uint32_t thread = ThreadHandle();
  std::string report;
  uint32_t seq;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    seq = ++g_seq;
    const uint32_t end = addr + (size ? size : 1);
    auto next = g_live.upper_bound(addr);
    const Block* other = nullptr;
    uint32_t other_addr = 0;
    if (next != g_live.begin()) {
      auto prev = std::prev(next);
      if (prev->first + (prev->second.size ? prev->second.size : 1) > addr) {
        other = &prev->second;
        other_addr = prev->first;
      }
    }
    if (!other && next != g_live.end() && next->first < end) {
      other = &next->second;
      other_addr = next->first;
    }
    if (other && g_overlaps.fetch_add(1) < kMaxReports) {
      report = fmt::format(
          "[heap] OVERLAP #{}: {} returned {:08X} (+{:X}, heap {:08X}) to lr {:08X} on thread {:08X}, but block "
          "{:08X} (+{:X}, heap {:08X}) from lr {:08X} on thread {:08X} (op #{}) is still live; headers: new [{}] "
          "live [{}]; guest stack:{}",
          seq, what, addr, size, heap, lr, thread, other_addr, other->size, other->heap, other->lr, other->thread,
          other->seq, Header(base, addr), Header(base, other_addr), GuestStack(base, sp));
    }
    g_live[addr] = Block{size, heap, lr, thread, seq};
  }
  if (!report.empty()) {
    REXLOG_ERROR("{}", report);
    rex::FlushLogging();
  }
  if (Watched(addr, size) || HeapTraced()) {
    REXLOG_INFO("[heap] #{} {} {:08X} +{:X} heap {:08X} lr {:08X} thread {:08X} header [{}] stack{}", seq, what, addr,
                size, heap, lr, thread, Header(base, addr), GuestStack(base, sp));
  }
}

// Called before the heap frees `addr`: forgets it, and reports a block that is not live.
void NoteFree(const uint8_t* base, const char* what, uint32_t heap, uint32_t addr, uint32_t lr, uint32_t sp) {
  if (!addr) return;
  const uint32_t thread = ThreadHandle();
  bool known;
  Block block{};
  uint32_t seq;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    seq = ++g_seq;
    auto it = g_live.find(addr);
    known = it != g_live.end();
    if (known) {
      block = it->second;
      g_live.erase(it);
    }
  }
  if (!known && g_bad_frees.fetch_add(1) < kMaxReports) {
    REXLOG_ERROR("[heap] FREE OF A BLOCK THAT IS NOT LIVE #{}: {} {:08X} (heap {:08X}) from lr {:08X} on thread "
                 "{:08X}; header [{}]; guest stack:{}",
                 seq, what, addr, heap, lr, thread, Header(base, addr), GuestStack(base, sp));
    rex::FlushLogging();
  }
  if (Watched(addr, known ? block.size : 1) || HeapTraced()) {
    REXLOG_INFO("[heap] #{} {} {:08X} +{:X} heap {:08X} lr {:08X} thread {:08X} (allocated by lr {:08X} thread "
                "{:08X} op #{}) header [{}] stack{}",
                seq, what, addr, block.size, heap, lr, thread, block.lr, block.thread, block.seq, Header(base, addr),
                GuestStack(base, sp));
  }
}

struct Depth {
  explicit Depth(uint32_t lr) {
    if (t_depth++ == 0) t_outer_lr = lr;
  }
  ~Depth() { --t_depth; }
};

}  // namespace heaptrack

// The heap tracker below is OFF unless asked for (2026-10-13). It was written for the console boot crash of
// 2026-10-05, whose cause turned out to be the network start-up, and stayed on in every build: each heap call of the
// game took one process-wide mutex and a node of a std::map (a host malloc or free). Console profile of the first 20 s
// (logs4): 16 % of the main thread's samples in NoteAlloc / NoteFree / the mutex, and 71 % of the samples of Wwise's
// event thread in _malloc_r / _free_r / mutexLock: the audio stuttered until the loading was over.
REXCVAR_DEFINE_BOOL(peggle2_debug_heap_track, false, "Peggle2",
                    "Diagnostic: track every block of the game's heaps (overlaps, frees of blocks that are not live, a "
                    "heap lock that already names the thread) and log the watched ones. Costs a process-wide lock and "
                    "a host allocation per heap call")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace heaptrack {
bool On() {
  static const bool on = REXCVAR_GET(peggle2_debug_heap_track);
  return on;
}
}  // namespace heaptrack

extern "C" REX_FUNC(__imp__sub_8231BAE0);
REX_HOOK_RAW(sub_8231BAE0) {  // RtlAllocateHeap(heap, flags, size)
  if (!heaptrack::On()) {
    __imp__sub_8231BAE0(ctx, base);
    return;
  }
  const uint32_t heap = ctx.r3.u32, size = ctx.r5.u32, lr = uint32_t(ctx.lr);
  heaptrack::CheckLockOnEntry("RtlAllocateHeap", base, heap, lr);
  uint32_t outer_lr;
  {
    heaptrack::Depth depth(lr);
    outer_lr = heaptrack::t_outer_lr;
    __imp__sub_8231BAE0(ctx, base);
  }
  if (ctx.r3.u32) {
    heaptrack::NoteAlloc(base, "alloc", heap, ctx.r3.u32, size, heaptrack::CallerLr(outer_lr), ctx.r1.u32);
  }
}

extern "C" REX_FUNC(__imp__sub_8231C3D8);
REX_HOOK_RAW(sub_8231C3D8) {  // RtlFreeHeap(heap, flags, ptr)
  if (!heaptrack::On()) {
    __imp__sub_8231C3D8(ctx, base);
    return;
  }
  const uint32_t heap = ctx.r3.u32, ptr = ctx.r5.u32, lr = uint32_t(ctx.lr);
  heaptrack::CheckLockOnEntry("RtlFreeHeap", base, heap, lr);
  heaptrack::Depth depth(lr);
  heaptrack::NoteFree(base, "free", heap, ptr, heaptrack::CallerLr(heaptrack::t_outer_lr), ctx.r1.u32);
  __imp__sub_8231C3D8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_8231C6D0);
REX_HOOK_RAW(sub_8231C6D0) {  // RtlReAllocateHeap(heap, flags, ptr, size)
  if (!heaptrack::On()) {
    __imp__sub_8231C6D0(ctx, base);
    return;
  }
  const uint32_t heap = ctx.r3.u32, ptr = ctx.r5.u32, size = ctx.r6.u32, sp = ctx.r1.u32;
  const uint32_t lr = heaptrack::CallerLr(uint32_t(ctx.lr));
  heaptrack::CheckLockOnEntry("RtlReAllocateHeap", base, heap, lr);
  {
    heaptrack::Depth depth(lr);
    __imp__sub_8231C6D0(ctx, base);
  }
  const uint32_t result = ctx.r3.u32;
  if (!result) return;
  if (result == ptr) {
    // Resized in place: the old entry goes and the new size comes back under the same address.
    heaptrack::NoteFree(base, "realloc-in-place(old)", heap, ptr, lr, sp);
    heaptrack::NoteAlloc(base, "realloc-in-place", heap, result, size, lr, sp);
    return;
  }
  // Moved: RtlReAllocateHeap allocated and freed through the two hooks above; check that it did.
  bool new_known, old_known;
  {
    std::lock_guard<std::mutex> lock(heaptrack::g_mutex);
    new_known = heaptrack::g_live.count(result) != 0;
    old_known = heaptrack::g_live.count(ptr) != 0;
  }
  if (!new_known) heaptrack::NoteAlloc(base, "realloc-moved", heap, result, size, lr, sp);
  if (old_known) heaptrack::NoteFree(base, "realloc-moved(old)", heap, ptr, lr, sp);
}

// Which threads the game starts, from where, with which routine and parameter, and whether suspended
// (sub_82318E18: CreateThread over ExCreateThread; r4 stack size, r5 routine, r6 parameter, r7 flags). A race
// between a new thread and its creator is the other way a Switch-only heap corruption can start (the Mushihimesama
// port had one: a thread created running read a member its creator had not set yet).
extern "C" REX_FUNC(__imp__sub_82318E18);
REX_HOOK_RAW(sub_82318E18) {
  const uint32_t routine = ctx.r5.u32, parameter = ctx.r6.u32, flags = ctx.r7.u32, lr = uint32_t(ctx.lr);
  const uint32_t sp = ctx.r1.u32;
  __imp__sub_82318E18(ctx, base);
  REXLOG_INFO("[thread] created handle {:08X} by thread {:08X} from lr {:08X}: routine {:08X} parameter {:08X} flags "
              "{:X}{}; guest stack{}",
              ctx.r3.u32, heaptrack::ThreadHandle(), lr, routine, parameter, flags, (flags & 1) ? " (suspended)" : "",
              heaptrack::GuestStack(base, sp));
}

// Race test (PC): sub_82305188(routine, parameter, processor) starts a game thread running (_beginthreadex, flags 0);
// the singleton's constructor starts two this way (sub_822ECF80: routines 0x822FC418 and 0x822F8D18) and goes on
// configuring their objects. With peggle2_debug_race_delay_ms the creator sleeps that long after each start, so the
// new thread gets ahead the way it may on the Switch's slower cores. 0 = off.
REXCVAR_DEFINE_INT32(peggle2_debug_race_delay_ms, 0, "Peggle2",
                     "Debug: the creator of a game thread (sub_82305188) sleeps this long after starting it");
extern "C" REX_FUNC(__imp__sub_82305188);
REX_HOOK_RAW(sub_82305188) {
  const uint32_t routine = ctx.r3.u32, parameter = ctx.r4.u32;
  __imp__sub_82305188(ctx, base);
  if (const int32_t delay = REXCVAR_GET(peggle2_debug_race_delay_ms); delay > 0) {
    REXLOG_INFO("[thread] race test: creator sleeps {} ms after starting routine {:08X} ({:08X})", delay, routine,
                parameter);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
}

// The opposite race test: the two threads the singleton's constructor starts (routines 0x822FC418, 0x822F8D18) sleep
// peggle2_debug_thread_start_delay_ms before their first instruction, so the creator gets far ahead of them.
REXCVAR_DEFINE_INT32(peggle2_debug_thread_start_delay_ms, 0, "Peggle2",
                     "Debug: the threads started by the singleton's constructor wait this long before running");
extern "C" REX_FUNC(__imp__sub_822FC418);
REX_HOOK_RAW(sub_822FC418) {
  if (const int32_t delay = REXCVAR_GET(peggle2_debug_thread_start_delay_ms); delay > 0) {
    REXLOG_INFO("[thread] race test: routine 822FC418 ({:08X}) waits {} ms before running", ctx.r3.u32, delay);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  __imp__sub_822FC418(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_822F8D18);
REX_HOOK_RAW(sub_822F8D18) {
  if (const int32_t delay = REXCVAR_GET(peggle2_debug_thread_start_delay_ms); delay > 0) {
    REXLOG_INFO("[thread] race test: routine 822F8D18 ({:08X}) waits {} ms before running", ctx.r3.u32, delay);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  __imp__sub_822F8D18(ctx, base);
}

// The allocator entry points, only to know who called them (heaptrack::CallerLr).
#define PEGGLE2_ALLOCATOR_ENTRY(name)            \
  extern "C" REX_FUNC(__imp__##name);            \
  REX_HOOK_RAW(name) {                           \
    heaptrack::ApiScope scope(uint32_t(ctx.lr)); \
    __imp__##name(ctx, base);                    \
  }
PEGGLE2_ALLOCATOR_ENTRY(sub_82369B48)  // operator new(size)
PEGGLE2_ALLOCATOR_ENTRY(sub_825EEA68)  // malloc(size)
PEGGLE2_ALLOCATOR_ENTRY(sub_825EE8C0)  // free(ptr)
PEGGLE2_ALLOCATOR_ENTRY(sub_823188B0)  // XMemAlloc(size, attributes)
PEGGLE2_ALLOCATOR_ENTRY(sub_823189E8)  // XMemFree(ptr, attributes)
#undef PEGGLE2_ALLOCATOR_ENTRY
