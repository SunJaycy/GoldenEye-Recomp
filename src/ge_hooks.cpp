// ge - project mid-ASM hooks: 0x830E0xxx fragment reconstruction.
#include <thread>
//
// 8 functions branch to 0x830E0xxx, ZERO in the static XEX (rexglue codegen
// stubs it) but at runtime real PPC code (identical to fragments IDA
// mis-coalesced into sub_821A9720). codegen prunes the code after the
// unconditional `b 0x830E0xxx`, so each continuation point is declared as its
// own ge_cont_* function. Each [[midasm_hook]] (return = true) replicates the
// fragment's register/memory effect, tail-invokes the continuation function,
// and the recompiled source function then returns.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "ge_init.h"   // PPCRegister/PPCContext + generated function decls
#include <rex/cvar.h>  // REXCVAR_* (mouse-look settings)
#include <rex/ui/keybinds.h>     // ParseVirtualKey (keyboard rebinding)
#include <rex/ui/virtual_key.h>
#include <rex/hook.h>  // ThreadState, kernel_state, memory
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/command_processor.h>
#include <rex/system/xthread.h>
#include <rex/system/kernel_state.h>
#include <cstdio>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW (WIN32_LEAN_AND_MEAN excludes it)
#include <string>

namespace ge {
// Relaunch this same executable as a fresh, detached process. Used by the ONLINE
// pause-menu tab's "Save & Restart": the new instance reads the just-written
// ge.toml (new username / server / online-enable) at boot, then the caller tears
// the current process down. Launching a second instance of a running exe is fine
// on Windows -- the image file is opened share-read.
void LaunchSelfDetached() {
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return;  // can't resolve our own path; skip relaunch (caller still quits)
  }
  // Start it from the exe's own directory so a normal boot's relative paths hold.
  std::wstring full(exe_path, exe_path + n);
  size_t slash = full.find_last_of(L"\\/");
  std::wstring workdir = (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
  ShellExecuteW(nullptr, L"open", exe_path, nullptr,
                workdir.empty() ? nullptr : workdir.c_str(), SW_SHOWNORMAL);
}
}  // namespace ge

// Probe to read CommandProcessor's protected ring read pointer (legal: a
// derived class may touch protected base members; we only reinterpret an
// existing CP* and call a non-virtual accessor -- no construction, no
// vtable use, layout-compatible single inheritance).
namespace {
struct CPProbe : rex::graphics::CommandProcessor {
  uint32_t rpi() const { return read_ptr_index_; }
  uint32_t wpi() const { return write_ptr_index_.load(std::memory_order_acquire); }
};
// rexglue CP swap counter sampled at the last guest present (sub_821996F8).
// "GPU finished the just-submitted frame" == counter advanced past this.
std::atomic<uint32_t> g_present_cpcnt{0};
// Guest tick at the last present, for a bounded completion wait.
std::atomic<uint32_t> g_present_tb{0};
inline rex::graphics::CommandProcessor* ge_cp() {
  auto* ks = rex::system::kernel_state();
  if (!ks) return nullptr;
  auto* rt = ks->emulator();
  if (!rt) return nullptr;
  auto* igs = rt->graphics_system();
  if (!igs) return nullptr;
  return static_cast<rex::graphics::GraphicsSystem*>(igs)->command_processor();
}
inline rex::graphics::GraphicsSystem* ge_gs() {
  auto* ks = rex::system::kernel_state();
  if (!ks) return nullptr;
  auto* rt = ks->emulator();
  if (!rt) return nullptr;
  auto* igs = rt->graphics_system();
  if (!igs) return nullptr;
  return static_cast<rex::graphics::GraphicsSystem*>(igs);
}
}  // namespace

namespace {
inline void getcb(PPCContext*& ctx, uint8_t*& base) {
  ctx = rex::runtime::ThreadState::Get()->context();
  base = rex::system::kernel_state()->memory()->virtual_membase();
}
inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); return __builtin_bswap32(v);
}
inline uint64_t LD64(uint8_t* b, uint32_t ga) {
  uint64_t v; std::memcpy(&v, b + ga, 8); return __builtin_bswap64(v);
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  uint32_t v = __builtin_bswap32(val); std::memcpy(b + ga, &v, 4);
}
inline void STF32(uint8_t* b, uint32_t ga, float f) {
  uint32_t v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v);
  std::memcpy(b + ga, &v, 4);
}
inline float LDF32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); v = __builtin_bswap32(v);
  float f; std::memcpy(&f, &v, 4); return f;
}
inline uint16_t LD16(uint8_t* b, uint32_t ga) {
  uint16_t v; std::memcpy(&v, b + ga, 2); return __builtin_bswap16(v);
}
inline void ST16(uint8_t* b, uint32_t ga, uint16_t val) {
  uint16_t v = __builtin_bswap16(val); std::memcpy(b + ga, &v, 2);
}
}  // namespace

// ===========================================================================
// Freeze watchdog. Auto-detects the visual freeze (the guest keeps presenting
// -- present# advancing -- but the GPU command ring stops advancing) and logs
// the exact pipeline state ONCE per stall episode, so we can read the mechanism
// off the log instead of capturing a live process. Zero gameplay effect.
// ===========================================================================
namespace {
std::atomic<uint32_t> g_ge_device{0};   // device struct (dev) seen by ge_dbg_now
std::atomic<uint32_t> g_ge_idblk{0};    // id-block (idblk) seen by ge_dbg_now
std::atomic<uint32_t> g_dbgnow_calls{0};  // increments each ge_dbg_now (guest polling sub_82198C28)

void ge_watchdog_thread() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t last_wpi = 0xFFFFFFFFu, last_rpi = 0, last_present = 0, last_submit = 0;
  uint32_t present_at_stall_start = 0, dbg_at_stall_start = 0, submit_at_stall_start = 0;
  uint32_t stall = 0;
  bool logged = false;
  bool recover_fired = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto* cp = ge_cp();
    if (!cp) continue;
    uint32_t wpi = static_cast<CPProbe*>(cp)->wpi();
    uint32_t rpi = static_cast<CPProbe*>(cp)->rpi();
    uint32_t present = g_present_cpcnt.load(std::memory_order_relaxed);
    uint32_t dbg = g_dbgnow_calls.load(std::memory_order_relaxed);
    uint32_t dev = g_ge_device.load(std::memory_order_relaxed);
    uint32_t idblk = g_ge_idblk.load(std::memory_order_relaxed);
    uint32_t submit = dev ? LD32(base, dev + 16544) : 0;

    bool present_alive = (present != last_present);
    bool ring_moved = (wpi != last_wpi) || (rpi != last_rpi);
    if (present_alive && !ring_moved) {
      if (stall == 0) {
        present_at_stall_start = present;
        dbg_at_stall_start = dbg;
        submit_at_stall_start = submit;
        recover_fired = false;
      }
      ++stall;
      // AUTO-RECOVERY for the CPU<->GPU semaphore deadlock. The CP parks in a
      // WAIT_REG_MEM polling idblk for ==0 (the semaphore the render writes 0 to
      // release the CP). The render is parked waiting on GPU completion ->
      // deadlock. Write 0 to release the CP: it drains the buffer, delivers the
      // completion, the render resumes. Memory-only, no interrupt (safe).
      if (stall >= 2 && dev && idblk && idblk < 0xFFFFFFFEu) {
        ST32(base, idblk, 0u);  // release the CP's WAIT_REG_MEM semaphore
        if (!recover_fired) {
          recover_fired = true;
          REXKRNL_INFO("GEWATCHDOG RECOVERY: released CP semaphore (idblk={:#x} := 0)", idblk);
        }
      }
      if (stall >= 6 && !logged) {  // ~1.5s of present-but-no-ring
        logged = true;
        uint32_t presented = dev ? LD32(base, dev + 16552) : 0;
        uint32_t target = dev ? LD32(base, dev + 10908) : 0;
        uint32_t completed = idblk ? LD32(base, idblk + 0) : 0;
        uint32_t skip = dev ? (base[dev + 10941] & 2) : 0;
        REXKRNL_INFO(
            "GEWATCHDOG STALL: ring rpi={:#x} wpi={:#x} [{}] | present#={} (+{}/stall) | "
            "dbgnow_polls={} (+{}/stall) | submit={} completed={} target={} presented={} skipbit={} "
            "| dev={:#x} idblk={:#x}",
            rpi, wpi, (rpi == wpi ? "DRAINED" : "PENDING"), present, present - present_at_stall_start,
            dbg, dbg - dbg_at_stall_start, submit, completed, target, presented, skip, dev, idblk);
        REXKRNL_INFO(
            "GEWATCHDOG -> completion={} | presenting={} | producer={} | polling={}",
            (submit > completed ? "GPU BEHIND (completion not delivered)" : "caught up"),
            (submit > presented ? "frames NOT presenting" : "caught up"),
            (submit != submit_at_stall_start ? "ALIVE (submitting)" : "STALLED (not submitting)"),
            (dbg != dbg_at_stall_start ? "guest spinning in sub_82198C28" : "guest NOT polling"));
        // Render gate: frame loop runs render+present only when dword_8242043C&2
        // (sub_8209E1C0). Set by sub_8209E1D0(mode): mode 3 at init (enabled),
        // mode 1 = bit clear = render skipped every frame = freeze.
        uint32_t rg = LD32(base, 0x8242043Cu);
        REXKRNL_INFO("GEWATCHDOG -> render-gate dword_8242043C={} -> render+present {}", rg,
                     (rg & 2u) ? "ENABLED" : "DISABLED (frame loop skips render = FREEZE)");
        // Device flags gating the present/submit (a1 = dev). +21516 != 0 => the
        // present SKIPS VdSwap (no screen update) and sub_821A4D50 takes its alt
        // path; +22280&4 gates the GPU-completion wait; +10941/+10943 = skip bits.
        if (dev) {
          uint32_t f21516 = LD32(base, dev + 21516u);
          uint32_t f22280 = LD32(base, dev + 22280u);
          uint32_t f22276 = LD32(base, dev + 22276u);
          uint32_t f21604 = LD32(base, dev + 21604u);
          uint32_t f21600 = LD32(base, dev + 21600u);
          uint32_t b10941 = base[dev + 10941u];
          uint32_t b10943 = base[dev + 10943u];
          REXKRNL_INFO(
              "GEWATCHDOG -> devflags +21516(VdSwap-skip if !=0)={:#x} | +22280&4(gpu-wait)={} | "
              "+22276={:#x} | +21604={} +21600={} (ring) | +10941={:#x} +10943={:#x}",
              f21516, (f22280 & 4u), f22276, f21604, f21600, b10941, b10943);
          uint32_t vbl = LD32(base, dev + 16532u);   // ctx[4133] vblank count
          uint32_t fr = LD32(base, dev + 16684u);    // ctx[4171] fence read idx
          uint32_t fw = LD32(base, dev + 16688u);    // ctx[4172] fence write idx
          REXKRNL_INFO("GEWATCHDOG -> vblank ctx[4133]={} | GPU fences read={} write={} [{}]", vbl,
                       fr, fw, (fr != fw ? "PENDING -- fences NOT retiring" : "drained"));
        }
        // Frame counter dword_8308851C is updated each frame AFTER the frame-
        // limiter (0x82189e64). Sample it twice: if FROZEN, the main thread never
        // exits the frame-limiter (clock/timebase not advancing for it); if it
        // ADVANCES, the main thread cycles and the render is skipped after.
        uint32_t fc1 = LD32(base, 0x8308851Cu);
        uint32_t tb1 = (uint32_t)REX_QUERY_TIMEBASE();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        uint32_t fc2 = LD32(base, 0x8308851Cu);
        uint32_t tb2 = (uint32_t)REX_QUERY_TIMEBASE();
        REXKRNL_INFO(
            "GEWATCHDOG -> frameCounter 0x8308851C {}->{} [{}] | guestTimebase {}->{} [{}]", fc1, fc2,
            (fc1 != fc2 ? "ADVANCING (main thread cycles; render skipped after limiter)"
                        : "FROZEN (main thread STUCK in frame-limiter)"),
            tb1, tb2, (tb1 != tb2 ? "advancing" : "FROZEN"));
        // Dump every guest thread's jump state -- find WHERE the render workers
        // (guest entry 0x821A4A68) are wedged inside sub_821A4750. lr = return
        // addr, ctr = next indirect target, lastIndTgt = last REX_CALL_INDIRECT
        // target, msr bit 0x8000 = interrupts enabled.
        auto* ks2 = rex::system::kernel_state();
        if (ks2) {
          auto threads = ks2->object_table()->GetObjectsByType<rex::system::XThread>();
          for (auto& th : threads) {
            if (!th) continue;
            auto* ts = th->thread_state();
            if (!ts) continue;
            auto* c = ts->context();
            if (!c) continue;
            uint32_t sa = th->creation_params()->start_address;
            bool rw = (sa == 0x821A4A68u);
            REXKRNL_INFO(
                "GEWATCHDOG THREAD start={:#x}{} lr={:#x} ctr={:#x} lastIndTgt={:#x} msr={:#x} | "
                "r3={:#x} r11={:#x} r28={:#x} r29={:#x} r30={:#x} r31={:#x}",
                sa, rw ? " [RENDER-WORKER]" : "", (uint32_t)c->lr, c->ctr.u32,
                c->last_indirect_target, c->msr, c->r3.u32, c->r11.u32, c->r28.u32, c->r29.u32,
                c->r30.u32, c->r31.u32);
            // Guest stack walk: scan [r1, r1+0x2400) for guest code addresses
            // (0x82xxxxxx return addresses) -> the call chain, directly readable.
            {
              uint32_t sp = c->r1.u32;
              if (sp >= 0x10000u && sp < 0xC0000000u) {
                uint8_t* hsp = base + sp;
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery(hsp, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0) {
                  uint8_t* rend = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
                  uint8_t* send = hsp + 0x2400u;
                  if (send > rend) send = rend;  // never read past the committed page
                  char sbuf[500];
                  int soff = 0;
                  sbuf[0] = 0;
                  for (uint8_t* pp = hsp; pp + 4 <= send && soff < 460; pp += 4) {
                    uint32_t val;
                    std::memcpy(&val, pp, 4);
                    val = __builtin_bswap32(val);
                    if (val >= 0x82000000u && val < 0x84000000u) {
                      int n = std::snprintf(sbuf + soff, sizeof(sbuf) - soff, "%x ", val);
                      if (n > 0) soff += n;
                    }
                  }
                  REXKRNL_INFO("GEWATCHDOG   STACK start={:#x} sp={:#x}: {}", sa, sp, sbuf);
                }
              }
            }
            if (rw) {
              // a1 (worker struct) = r28; event = a1[2] = a1+0x20 (= r29);
              // queue = a1[3]: Flink/submit = a1+0x38, Blink/processed = a1+0x3C.
              // wait is INFINITE when a1->SignalState(a1+4) != *(v3+368), v3 = *a1.
              uint32_t bw = c->r28.u32;
              auto sLD = [&](uint32_t ga) -> uint32_t {
                return (ga >= 0x1000u && ga < 0x50000000u) ? LD32(base, ga) : 0xDEADBEEFu;
              };
              uint32_t v3 = sLD(bw);
              uint32_t sig = sLD(bw + 4);
              uint32_t v3f = sLD(v3 + 368);
              uint32_t subq = sLD(bw + 0x38);
              uint32_t procq = sLD(bw + 0x3C);
              REXKRNL_INFO(
                  "GEWATCHDOG   WORKER a1={:#x} queue Flink/submit={} Blink/proc={} [{}] | "
                  "SignalState={} v3={:#x} *(v3+368)={} -> wait={}",
                  bw, subq, procq,
                  (subq == procq ? "EMPTY (producer stopped feeding)" : "PENDING (LOST WAKEUP!)"),
                  sig, v3, v3f, (sig != v3f ? "INFINITE" : "30ms-timeout"));
            }
          }
          // Rapid-sample the main game thread (start 0x8235e4a8): it spends most
          // time in the frame-limiter, so one snapshot misses the render path.
          // Sample lr many times (yielding so it keeps running) -> the set of
          // unique guest PCs = its per-frame code path, revealing which render
          // subsystem call it reaches/skips.
          for (auto& th : threads) {
            if (!th) continue;
            if (th->creation_params()->start_address != 0x8235E4A8u) continue;
            auto* ts = th->thread_state();
            if (!ts) continue;
            auto* mc = ts->context();
            if (!mc) continue;
            uint32_t seen[96];
            int ns = 0;
            for (int it = 0; it < 8000 && ns < 94; it++) {
              uint32_t pc = static_cast<uint32_t>(mc->lr);
              if (pc >= 0x82000000u && pc < 0x84000000u) {
                bool dup = false;
                for (int j = 0; j < ns; j++)
                  if (seen[j] == pc) { dup = true; break; }
                if (!dup) seen[ns++] = pc;
              }
              std::this_thread::yield();
            }
            char mb[760];
            int mo = 0;
            mb[0] = 0;
            for (int j = 0; j < ns && mo < 720; j++) {
              int n = std::snprintf(mb + mo, sizeof(mb) - mo, "%x ", seen[j]);
              if (n > 0) mo += n;
            }
            REXKRNL_INFO("GEWATCHDOG MAINPATH (unique lr x{}): {}", ns, mb);
            // Snapshot the main thread's full stack repeatedly with SLEEPS (no
            // spinning -> doesn't starve it, it keeps cycling). Log only snapshots
            // where it is OUTSIDE the frame-limiter -> in the per-frame render
            // path -> the render call chain + the skipped 3D-submit branch.
            {
              int logged = 0;
              for (int snap = 0; snap < 160 && logged < 12; snap++) {
                uint32_t pc = static_cast<uint32_t>(mc->lr);
                bool in_lim = (pc >= 0x823B3040u && pc <= 0x823B3540u) ||
                              (pc >= 0x82189DC0u && pc <= 0x82189E14u);
                if (!in_lim && pc >= 0x82000000u && pc < 0x84000000u) {
                  uint32_t sp = mc->r1.u32;
                  char fb[620];
                  int fo = std::snprintf(fb, sizeof(fb), "lr=%x | ", pc);
                  if (sp >= 0x10000u && sp < 0xC0000000u) {
                    uint8_t* hsp = base + sp;
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery(hsp, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                        mbi.State == MEM_COMMIT) {
                      uint8_t* rend = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
                      uint8_t* send = hsp + 0x2800u;
                      if (send > rend) send = rend;
                      for (uint8_t* pp = hsp; pp + 4 <= send && fo < 580; pp += 4) {
                        uint32_t v;
                        std::memcpy(&v, pp, 4);
                        v = __builtin_bswap32(v);
                        if (v >= 0x82000000u && v < 0x84000000u) {
                          int n = std::snprintf(fb + fo, sizeof(fb) - fo, "%x ", v);
                          if (n > 0) fo += n;
                        }
                      }
                    }
                  }
                  REXKRNL_INFO("GEWATCHDOG FRAMEWORK[{}] {}", logged, fb);
                  logged++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
            }
            break;
          }
        }
      }
    } else {
      stall = 0;
      logged = false;
      recover_fired = false;
    }
    last_wpi = wpi; last_rpi = rpi; last_present = present; last_submit = submit;
  }
}

inline void ge_start_watchdog_once() {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    std::thread(ge_watchdog_thread).detach();
  }
}
}  // namespace

// NOTE: no frame-limiter / intro-wait hook. The post-intro freeze is a
// SYMPTOM of the rexglue GPU command-processor not consuming the ring (GPU
// hung -> game stops presenting -> guest time stops -> wait never clears).
// Any hook writing that shared time counter corrupts per-frame timing and
// slows the intros without fixing the GPU hang -> net-harmful, removed.
// Intros run at full speed; post-intro hits the rexglue GPU ceiling.

// sub_82198C28 frame-wait reads now = *(r9+0x58) (r9 = *(r13+0x100)), waits
// while (now - last) < 0x1388. That field is a hardware/kernel time the game
// only READS (no guest writer); rexglue never ticks it -> frozen at 0 ->
// infinite spin. Feed it the real guest tick clock (REX_QUERY_TIMEBASE, the
// same ~49.875MHz source mftb uses): write the live value to both the loaded
// register (so this iteration's compare sees it) and the memory field (so
// other readers/sub_8235EAA8 see a consistent advancing clock). (now-last)
// then measures real elapsed ticks exactly like console -> correct pacing.
void ge_dbg_now(PPCRegister& r9, PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  uint32_t t = (uint32_t)REX_QUERY_TIMEBASE();
  if (r9.u32) ST32(base, r9.u32 + 0x58, t);
  r30.u32 = t;

  uint32_t dev = ctx->r29.u32;
  uint32_t idblk = ctx->r11.u32;
  uint32_t ws = ctx->r31.u32;

  // Feed the freeze watchdog (stash device pointers, count polls, start thread).
  g_ge_device.store(dev, std::memory_order_relaxed);
  g_ge_idblk.store(idblk, std::memory_order_relaxed);
  g_dbgnow_calls.fetch_add(1, std::memory_order_relaxed);
  ge_start_watchdog_once();
  auto* cpp = ge_cp();
  uint32_t cpc = cpp ? cpp->counter() : 0;
  uint32_t rpi = cpp ? static_cast<CPProbe*>(cpp)->rpi() : 0;
  uint32_t wpi = cpp ? static_cast<CPProbe*>(cpp)->wpi() : 0;

  if (ws) ST32(base, ws + 12, t);

  // Clear the GPU-completion fence once the just-submitted frame is drawn.
  // Two race-free conditions:
  //  (a) CP swap counter advanced past the value sampled at the matching
  //      present -> the frame's swap executed; OR
  //  (b) the CP read pointer caught up to the write pointer -> the CP consumed
  //      every packet the game submitted (this frame's draws+resolve+swap), so
  //      it is drawn. (b) eliminates the cpc-sampling race (CP finishing before
  //      ge_diag_vdswap samples g_present_cpcnt) that intermittently left the
  //      wait spinning forever -> the random menu freeze. The CP advances rptr
  //      on its own worker thread and always catches up once the game stops
  //      feeding the ring, so (b) cannot deadlock.
  bool drawn = (cpc != g_present_cpcnt.load(std::memory_order_relaxed)) ||
               (wpi != 0u && rpi == wpi);

  //  (c) WATCHDOG. If neither (a) nor (b) has happened for a long stretch
  //      (~80ms of real wall time), the CP is genuinely stuck on this frame
  //      (e.g. blocked in a D3D12 op the backend can't complete for this
  //      title's scene). Force-complete so the guest's GPU-completion spin
  //      clears instead of deadlocking forever -- and, critically, releases the
  //      device spinlock other guest threads (sub_821A3A40) are blocked on.
  //      This turns a permanent freeze into a recoverable hitch. 80ms is far
  //      above any real frame time, so normal frames still clear via (a)/(b).
  // sub_82198C28 checks the skip bit *(device+10941)&2 at its very top and
  // returns 0 (proceed) before any fence/timeout logic -- the only GUARANTEED
  // way out of the spin. Writing the completion fence does not always release
  // it (it resets the routine's own timeout anchor, and only helps when
  // submit>=target). So: when the wait has stalled for a real wall-clock
  // stretch (~80ms, far above any frame), SET the skip bit so the guest stops
  // blocking on a GPU completion the CP cannot deliver -- and, crucially,
  // releases the device spinlock the rest of the guest threads are stuck on.
  // When the CP is keeping up (drawn via (a)/(b)) CLEAR it again so waits are
  // honored and frames stay visible. Net: visible when the GPU keeps up, a
  // brief skipped (black) frame during a stall instead of a permanent freeze.
  // Do NOT touch the skip bit on the normal/keeping-up path -- the game manages
  // *(device+10941)&2 itself (sets it when it intends NOT to block, clears it
  // when it wants to wait), and clearing it during early init hangs the boot.
  // Only SET it when the wait has genuinely stalled (~80ms, far above any real
  // frame): that forces sub_82198C28 to return 0 next iteration so the guest
  // stops blocking on a GPU completion the CP cannot deliver -- breaking the
  // spinlock cascade / freeze. The game re-clears it on its own next frame, so
  // this stays a one-shot "proceed past this stall", not a permanent skip.
  static thread_local uint32_t s_wait_start = 0;
  static thread_local bool s_waiting = false;
  if (!drawn) {
    if (!s_waiting) { s_waiting = true; s_wait_start = t; }
    else if ((uint32_t)(t - s_wait_start) > 4000000u) {  // ~80ms @49.875MHz
      drawn = true;
      if (dev) base[dev + 10941u] |= 0x02u;   // stalled: skip this GPU wait
    }
    // The six GPU-completion waits poll this routine in a TIGHT busy spin. With
    // dozens of guest threads that oversubscribes the cores and starves the
    // rexglue CP worker thread -- which is the very thread that must advance the
    // ring read pointer / swap counter to satisfy (a)/(b). Result: the fence
    // never advances, the spin never exits = freeze (visual stops, audio thread
    // keeps running on its own core). Yield here while still waiting so the CP
    // worker reliably gets CPU and can finish the frame.
    if (!drawn) std::this_thread::yield();
  } else {
    s_waiting = false;
  }

  if (dev && idblk && idblk < 0xFFFFFFFEu && drawn) {
    // DO NOT write idblk+0 here. idblk+0 is the CPU<->GPU semaphore the CP polls
    // in WAIT_REG_MEM (waits for ==0; the render writes 0 to release the CP).
    // Writing a non-zero "completed" value here HELD the semaphore -> the CP
    // stalled in WAIT_REG_MEM -> CPU<->GPU deadlock -> the visual freeze. THIS
    // self-inflicted write was the freeze. (Confirmed via the WAIT_REG_MEM
    // >60ms deadlock-breaker log polling exactly this address.)
    ST32(base, dev + 16552, LD32(base, dev + 16544));   // presented := submit
    ST32(base, idblk + 60, rpi);                        // ring RPTR write-back
  }
}

// ---------------------------------------------------------------------------
// GPU-completion fence (the real fix). Wired at the present path
// sub_821996F8 @ 0x82199948 (right after the kernel VdSwap), r31 = a1 (D3D
// device struct), r30 = v21 (cmd-buffer swap slot).
//
// At each guest present, sample rexglue's CP swap counter. The poll hook
// (ge_dbg_now) then treats the frame as GPU-complete only once the CP's
// counter has moved past this -- i.e. the just-submitted frame was really
// drawn -- so the game blocks for the real render (visible) but no longer.
void ge_diag_vdswap(PPCRegister& r31, PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx; (void)base;
  (void)r30;
  uint32_t a1 = r31.u32;
  auto* cpp = ge_cp();
  uint32_t cpc = cpp ? cpp->counter() : 0;
  g_present_cpcnt.store(cpc, std::memory_order_relaxed);
  g_present_tb.store((uint32_t)REX_QUERY_TIMEBASE(), std::memory_order_relaxed);

  static uint32_t n = 0;                       // throttled fps heartbeat
  if ((n++ & 0x3F) == 0)
    REXKRNL_INFO("GEGPU present#{} dev={:#x} cpcnt={}", n, a1, cpc);
}

// F3  0x830E0670 (site 0x8209F5F0 sub_8209F5D8 -> ge_cont_8209F5F4)
void ge_hook_830E0670(PPCRegister& r3, PPCRegister& r11, PPCRegister& r28) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  r11.u32 = r3.u32 ^ 0x2Bu;
  r28.u32 = 0x82420000u;
  base[0x82420239u] = static_cast<uint8_t>(r11.u32 & 0xFFu);
  r28.u32 = 0x82420239u;
  r11.u32 = 0x82000000u;
  ge_cont_8209F5F4(*ctx, base);
}

// F1  0x830E0630: the r30++ (with 3/6 skip) loop-increment fragment. Hooked at
// the branch site 0x820F774C; the config jump_address sends control back to
// 0x820F7750 (cmpwi r30,8 / blt loc_820F768C) IN THE PARENT sub_820F73F8, so the
// whole loop -- including the loop-back to 0x820F768C -- stays in one function
// and resolves. (Routing through a separate ge_cont_820F7750 left that loop-back
// branch cross-function -> REX_FATAL when the loop ran, e.g. at the main menu.)
bool ge_hook_830E0630(PPCRegister& r30) {
  r30.u32 = r30.u32 + 1;
  if (r30.s32 == 3 || r30.s32 == 6) r30.u32 = r30.u32 + 1;
  // 0x820F7750: cmpwi r30,8 ; 0x820F7754: blt loc_820F768C (loop) else exit.
  return r30.s32 < 8;
}

// --- Hidden debug menu (TCRF: set byte 0x82189F2B 0x0F->0x00) ----------------
// In sub_821898D0: r30 = button mask, r11 = r30 & 0x100 (LB / LSHOULDER), then
//   0x82189F28  cmplwi cr6, r11, 0xF      ; r11 is only ever 0 or 0x100
//   0x82189F2C  beq    cr6, 0x82189F4C    ; so EQ never true -> dead branch
//   0x82189F30  ...LB-> debug-menu toggle handler (runs every frame in retail)
// Setting the 0xF immediate to 0 makes the compare "r11 == 0", so the beq is
// taken when LB is UP and the handler runs only when LB is PRESSED == LB toggles
// the debug menu. The recomp runs generated code (can't patch the byte), so we
// replace the branch decision here and read it live from a cvar (no restart).
//   debug ON : take branch (skip handler) iff LB up  -> handler fires on LB
//   debug OFF: never take branch                     -> identical to retail
// Hooked at 0x82189F2C with jump_on_true=0x82189F4C, jump_on_false=0x82189F30.
REXCVAR_DEFINE_BOOL(ge_debug_menu, false, "Input",
                    "Enable the hidden debug menu (press LB in-game to toggle it)");
bool ge_debug_gate(PPCRegister& r30) {
  return REXCVAR_GET(ge_debug_menu) && ((r30.u32 & 0x100u) == 0u);
}

// F2  sub_820F7968: r26 0..8 loop. sub_820F7968 = prologue only (codegen sets
// constants + r26=0). ge_f2_driver fires after the last prologue instruction
// (0x820F79EC, after, return) and drives the loop:
//   do { body; r26++; if(r26==3||r26==6) r26++; } while (r26 < 8); epilogue
// ge_body_820F79F0 = 0x820F79F0..0x820F7CFC (one iteration). Its skip branch
// (0x820F7A2C: clrlwi r11,r3,24; cmplwi r11,0; beq loc_820F7D00) becomes a
// return from the body via ge_f2_skip (return_on_true).
bool ge_f2_skip(PPCRegister& r3) {
  return (r3.u32 & 0xFFu) == 0u;  // beq loc_820F7D00 taken when (r3&0xFF)==0
}
void ge_f2_driver(PPCRegister& r26) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  for (;;) {
    ge_body_820F79F0(*ctx, base);                 // 0x820F79F0 one iteration
    r26.u32 = r26.u32 + 1;                         // 0x830E06B0 increment
    if (r26.s32 == 3 || r26.s32 == 6) r26.u32 = r26.u32 + 1;
    if (r26.s32 >= 8) break;                       // 0x820F7D08 blt not taken
  }
  ge_epi_820F7D0C(*ctx, base);                     // 0x820F7D0C epilogue (ret)
}

// F4  0x830E0200: loop-increment fragment (same shape as F1). Hooked at the
// branch site 0x820C4914; the config jump_address sends control back to
// 0x820C4918 IN THE PARENT sub_820C4630 so the loop-back to 0x820C4858 resolves
// in-function instead of crossing into a ge_cont_820C4918 (-> REX_FATAL).
bool ge_hook_830E0200(PPCRegister& r31, PPCRegister& r29, PPCRegister& r28,
                      PPCRegister& r11, PPCRegister& r23, PPCRegister& r21) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (r31.s32 == 3) { r31.u32 = r31.u32 + 1; r29.u32 = r29.u32 + 4; }
  r28.u32 = r11.u32 + r28.u32;
  // 0x820C4918: cmpw r31,r23 ; 0x820C491C: ble loc_820C4858 (loop) else exit.
  // The loop top 0x820C4858 (lwz r11,-0x684(r21)) is skipped on first entry
  // (0x820C4854 b loc_820C485C) -> only the loop-back reaches it, so it is not a
  // standalone block. Do its r11 reload here and jump to 0x820C485C (which IS
  // reachable / labeled) instead.
  if (r31.s32 <= r23.s32) {
    r11.u32 = LD32(base, r21.u32 - 0x684u);   // 0x820C4858: lwz r11,-0x684(r21)
    return true;                              // -> loc_820C485C (loop body)
  }
  return false;                               // -> loc_820C4920 (exit)
}

// F5  0x830E04D0 (site 0x820C7450 sub_820C7390 -> ge_cont_820C7454)
void ge_hook_830E04D0(PPCRegister& r11, PPCRegister& r10, PPCRegister& r9) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (r11.s32 == 3) r11.u32 = r11.u32 - 1;
  ST32(base, r9.u32 - 0x644u, r10.u32);
  ge_cont_820C7454(*ctx, base);
}

// F6  0x830E0560 (site 0x820C742C sub_820C7390 -> ge_cont_820C7430)
void ge_hook_830E0560(PPCRegister& r11, PPCRegister& r10, PPCRegister& r9) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (r11.s32 == 3) r11.u32 = r11.u32 + 1;
  ST32(base, r9.u32 - 0x644u, r10.u32);
  ge_cont_820C7430(*ctx, base);
}

// F7  0x830E0460 (site 0x820A3E50 sub_820A3C20 -> ge_cont_820A3E9C)
void ge_hook_830E0460(PPCRegister& r11, PPCRegister& r4, PPCRegister& r29,
                      PPCRegister& r7, PPCRegister& r28, PPCRegister& r6,
                      PPCRegister& r31, PPCRegister& r5, PPCRegister& r27,
                      PPCRegister& r3) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  r4.s64 = static_cast<int64_t>(static_cast<int16_t>(r11.u32 & 0xFFFFu));
  r7.u64 = r29.u64;
  r6.u32 = LD32(base, r28.u32 + 0x4DE8u);
  r5.u64 = r31.u64;
  r3.u32 = LD32(base, r27.u32 + 0x4DE0u);
  sub_82144920(*ctx, base);
  r3.u32 = 0;
  ge_cont_820A3E9C(*ctx, base);
}

// F8  0x830E0750 (site 0x820B40E4 sub_820B40C0, returns; code ends in blr)
void ge_hook_830E0750(PPCRegister& r7, PPCRegister& r8, PPCRegister& r11,
                      PPCRegister& f1) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t v32 = LD32(base, r8.u32 - 0x564u);
  r7.u32 = v32;
  if (v32 != 0) return;
  uint64_t v64 = LD64(base, r8.u32 - 0x560u);
  r7.u64 = v64;
  if (v64 != 0) return;
  STF32(base, r11.u32 + 0x1F0u, f1.f32);
  uint32_t t = LD32(base, r11.u32 + 0x1E4u);
  r8.u32 = t;
  ST32(base, r11.u32 + 0x1ECu, t);
  r8.u32 = 0;
  ST32(base, r11.u32 + 0x200u, 0);
}

// ===========================================================================
// Mouse-look (real 1:1 keyboard/mouse looking, replacing the stick-emulation
// path). We inject raw mouse deltas straight into the guest's per-frame look
// state inside ge_bondview_control (sub_820B99E8), so the mouse drives the same
// heading/pitch the right stick does -- but without the analog deadzone/accel/
// turn-rate cap that makes the built-in MnK stick emulation feel terrible.
//
// YAW   @0x820bab6c : flt_82F1F914 (per-frame yaw delta, RADIANS) += mouse_dx.
// PITCH @0x820bb79c : bondview(+612) (pitch angle) += mouse_dy, +center suppress.
//
// Mouse and controller both work AT ONCE (additive) -- the mouse just adds to
// whatever the right stick contributes, so you can pick up or put down the pad
// freely, or play pure mouse+keyboard with no pad plugged in. Movement / buttons
// stay on rexglue's built-in keybinds -- only the *look* is custom here.
//
// While mouse-look is on we also capture the OS cursor (hidden + confined to the
// window) during play, and release it whenever the pause menu is open or the
// window loses focus.
// ===========================================================================

// Mouse-look tunables, ported from the xenia-canary mousehook cvars. The
// user-facing sensitivity multiplier is ge_mouse_sens (defined below).
REXCVAR_DEFINE_BOOL(ge_invert_x, false, "Input", "Invert mouse X (horizontal) look");
REXCVAR_DEFINE_BOOL(ge_invert_y, false, "Input", "Invert mouse Y (vertical) look");
REXCVAR_DEFINE_BOOL(ge_disable_autoaim, true, "Input",
                    "Disable auto-aim and look-ahead while mouse-look is on");
REXCVAR_DEFINE_DOUBLE(ge_menu_sensitivity, 1.0, "Input",
                      "Mouse sensitivity in menus").range(0.05, 20.0);
REXCVAR_DEFINE_DOUBLE(ge_aim_turn_distance, 0.4, "Input",
                      "Crosshair travel in aim-mode before the camera turns [0-1]").range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(ge_gun_sway, true, "Input", "Gun sway as the camera turns");

REXCVAR_DEFINE_DOUBLE(ge_mouse_sens, 1.0, "Input", "Mouse look sensitivity").range(0.05, 20.0);
// Mouse-look on/off. ON: the mouse looks (added on top of the pad -- both work
// at once, so you can put the controller down) and the cursor is captured during
// play. OFF: no mouse-look, cursor free, controller only.
REXCVAR_DEFINE_BOOL(ge_mouselook_enable, true, "Input",
                    "Mouse look (works alongside the controller; captures the cursor in-game)");

namespace {
std::atomic<int> g_mouse_dx{0};
std::atomic<int> g_mouse_dy{0};
std::atomic<bool> g_mouselook_suppressed{false};  // set true while the pause menu is open
// True while the rebind menu is waiting for a key. We must swallow ALL game input
// (keyboard injection AND the real controller) so the key being bound doesn't also
// act on the game/menu -- the menu only listens for the capture.
std::atomic<bool> g_rebind_capturing{false};

// Cursor-capture state (touched from the mouse thread + SetMouselookSuppressed).
HWND g_game_hwnd = nullptr;
HCURSOR g_arrow_cursor = nullptr;
HCURSOR g_blank_cursor = nullptr;
bool g_captured = false;

bool ge_mouselook_on() { return REXCVAR_GET(ge_mouselook_enable); }

bool ge_game_has_focus() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

// The visible game window (the foreground window while it's ours). Cached so we
// can still restore the cursor after focus has moved elsewhere.
HWND ge_game_window() {
  HWND fg = GetForegroundWindow();
  if (fg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) g_game_hwnd = fg;
  }
  return g_game_hwnd;
}

// Active = mouse-look on, no menu up, and we own focus. Drives both delta
// collection and cursor capture.
bool ge_mouse_active() {
  return ge_mouselook_on() && !g_mouselook_suppressed.load(std::memory_order_relaxed) &&
         ge_game_has_focus();
}

HCURSOR ge_make_blank_cursor() {
  // 32x32 fully transparent cursor (AND=1 / XOR=0 == transparent everywhere).
  BYTE and_mask[32 * 32 / 8];
  BYTE xor_mask[32 * 32 / 8];
  std::memset(and_mask, 0xFF, sizeof(and_mask));
  std::memset(xor_mask, 0x00, sizeof(xor_mask));
  return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask, xor_mask);
}

// Capture (hide + confine) the cursor during play; release it in menus, when
// unfocused, or when mouse-look is off. Hiding is done by swapping the game
// window's class cursor (works cross-thread, unlike ShowCursor). Safe to call
// from any thread.
void ge_update_mouse_capture() {
  HWND hwnd = ge_game_window();
  const bool want = ge_mouse_active() && hwnd != nullptr;
  if (want) {
    if (!g_captured) {
      g_captured = true;
      if (g_blank_cursor) SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)g_blank_cursor);
      REXKRNL_INFO("GEMOUSE capture ON  hwnd={}", (void*)hwnd);
    }
    RECT rc;
    if (GetClientRect(hwnd, &rc)) {
      POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
      ClientToScreen(hwnd, &tl);
      ClientToScreen(hwnd, &br);
      RECT screen{tl.x, tl.y, br.x, br.y};
      ClipCursor(&screen);  // re-confine each tick (window may move/resize)
    }
  } else if (g_captured) {
    g_captured = false;
    if (hwnd && g_arrow_cursor) SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)g_arrow_cursor);
    ClipCursor(nullptr);
  }
}

LRESULT CALLBACK GeRawWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INPUT) {
    RAWINPUT ri;
    UINT sz = sizeof(ri);
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
        ri.header.dwType == RIM_TYPEMOUSE &&
        (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
      static std::atomic<bool> logged{false};
      bool exp = false;
      if (logged.compare_exchange_strong(exp, true))
        REXKRNL_INFO("GEMOUSE first WM_INPUT dx={} dy={} active={}", ri.data.mouse.lLastX,
                     ri.data.mouse.lLastY, ge_mouse_active());
      // Only collect while active, so deltas don't queue while alt-tabbed/paused
      // and snap the view on return.
      if (ge_mouse_active()) {
        g_mouse_dx.fetch_add(ri.data.mouse.lLastX, std::memory_order_relaxed);
        g_mouse_dy.fetch_add(ri.data.mouse.lLastY, std::memory_order_relaxed);
      }
    }
    return 0;
  }
  if (msg == WM_TIMER) {
    ge_update_mouse_capture();
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// Dedicated thread: own a message-only window + RIDEV_INPUTSINK raw mouse so we
// get true mouse deltas without touching the SDK's window/message loop. A timer
// drives the cursor capture/release each ~15ms.
void ge_mouse_thread() {
  g_arrow_cursor = LoadCursorA(nullptr, IDC_ARROW);  // IDC_ARROW is an ANSI int-resource
  g_blank_cursor = ge_make_blank_cursor();
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = GeRawWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GeRawMouseWnd";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              wc.hInstance, nullptr);
  if (!hwnd) return;
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;  // generic desktop
  rid.usUsage = 0x02;      // mouse
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = hwnd;
  BOOL reg = RegisterRawInputDevices(&rid, 1, sizeof(rid));
  REXKRNL_INFO("GEMOUSE thread up: hwnd={} rawinput_register={} (err={})",
               (void*)hwnd, reg ? "OK" : "FAIL", reg ? 0u : GetLastError());
  if (!reg) return;
  SetTimer(hwnd, 1, 15, nullptr);  // ~60Hz capture-state poll
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

void ge_start_mouse_once() {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    std::thread(ge_mouse_thread).detach();
  }
}

int ge_take_mouse_dx() { return g_mouse_dx.exchange(0, std::memory_order_relaxed); }
int ge_take_mouse_dy() { return g_mouse_dy.exchange(0, std::memory_order_relaxed); }
}  // namespace

namespace ge {
// Start the raw-mouse + cursor-capture thread once, at app startup, so capture
// works regardless of whether the guest look hooks have fired yet.
void InitMouseLook() {
  REXKRNL_INFO("GEMOUSE InitMouseLook (enable={})", REXCVAR_GET(ge_mouselook_enable));
  ge_start_mouse_once();
}

// Called by the app when the pause menu opens/closes so mouse motion isn't
// turned into look while the player is in menus (the cursor is needed there).
void SetMouselookSuppressed(bool v) {
  g_mouselook_suppressed.store(v, std::memory_order_relaxed);
  if (v) {  // drop any queued motion so closing the menu doesn't snap the view
    g_mouse_dx.store(0, std::memory_order_relaxed);
    g_mouse_dy.store(0, std::memory_order_relaxed);
  } else {
    g_rebind_capturing.store(false, std::memory_order_relaxed);  // never stick on close
  }
  ge_update_mouse_capture();  // release the cursor immediately when the menu opens
}

// Called by the rebind menu while it is waiting for a key. While true, all slot-0
// controller input is swallowed (ge_inject_keyboard) so the bound key can't act.
void SetRebindCapturing(bool v) {
  g_rebind_capturing.store(v, std::memory_order_relaxed);
}
}  // namespace ge

// ===========================================================================
// Mouse-look: faithful port of the xenia-canary mousehook
// (src/xenia/hid/winkey/hookables/goldeneye.cc, GoldeneyeGame::DoHooks) for the
// GoldenEye_Nov2007_Release build. Runs once per frame from ge_inject_keyboard,
// operating on the local player struct in guest RAM. Writes the game's own
// camera / crosshair / gun fields incrementally so it coexists with the
// controller, recoil, and the tank turret.
// ===========================================================================
namespace {
// RareGameBuildAddrs for GoldenEye_Nov2007_Release (from supported_builds[]).
constexpr uint32_t GE_MENU_XY       = 0x8272B37Cu;  // menu cursor X (Y at +4)
constexpr uint32_t GE_PAUSE_FLAG    = 0x82F1E70Cu;  // non-zero ~= game paused
constexpr uint32_t GE_SETTINGS_PTR  = 0x83088228u;  // -> settings struct pointer
constexpr uint32_t GE_SETTINGS_BITS = 0x298u;       // bitflags offset in struct
constexpr uint32_t GE_PLAYER_PTR    = 0x82F1FA98u;  // -> players[0] (host's Bond)
constexpr uint32_t GE_BONDVIEW_CUR  = 0x82F1FAACu;  // -> currently-controlled view's player
constexpr uint32_t GE_MUSIC_STATE   = 0x83066750u;  // XBLA mission-music state (0..6)
constexpr uint32_t GE_STAGE_MUSIC_ID = 0x8306674Cu; // current stage music-table key
constexpr uint32_t GE_MUSIC_CUES    = 0x83064DF4u;  // three active native music-cue slots
constexpr uint32_t GE_PENDING_STAGE = 0x82423DFCu;  // boss g_MainStageNum
constexpr uint32_t GE_CURRENT_STAGE = 0x82423E00u;  // boss g_StageNum
constexpr uint32_t GE_TITLE_STAGE   = 90u;
constexpr uint32_t GE_OFF_WATCH     = 0x2E8u;       // watch status (!=0 -> input disabled)
constexpr uint32_t GE_OFF_DISABLED  = 0x80u;        // control-disabled flag (cutscene)
constexpr uint32_t GE_OFF_CAM_X     = 0x254u;       // camera yaw
constexpr uint32_t GE_OFF_CAM_Y     = 0x264u;       // camera pitch
constexpr uint32_t GE_OFF_CH_X      = 0x10A8u;      // crosshair X
constexpr uint32_t GE_OFF_CH_Y      = 0x10ACu;      // crosshair Y
constexpr uint32_t GE_OFF_GUN_X     = 0x10BCu;      // gun X
constexpr uint32_t GE_OFF_GUN_Y     = 0x10C0u;      // gun Y
constexpr uint32_t GE_OFF_AIM_MODE  = 0x22Cu;       // aim-mode (1 = aiming)
constexpr uint32_t GE_OFF_AIM_MULT  = 0x11ACu;      // aim-turn multiplier (slows when zoomed)
constexpr uint32_t GE_IN_TANK_FLAG  = 0x82F1F8D4u;  // authoritative mounted-state flag
constexpr uint32_t GE_PLAYER_TANK_PROP = 0x82F1F8DCu;
constexpr uint32_t GE_TANK_TURRET_RAD  = 0x82F1F900u;
constexpr uint32_t GE_TANK_TURRET_ACCUM = 0x82F1F904u;
constexpr uint32_t GE_TANK_TURRET_TARGET = 0x82F1F910u;
constexpr uint32_t GE_TANK_TURRET_RECIP = 0x82003C64u; // 1 / (1 - smoothing)
enum GESettingFlag {
  GE_SET_AutoAim   = 0x10,
  GE_SET_LookAhead = 0x80,
};

// The XBLA AI interpreter recognizes opcodes F4/F5, but its handlers only call
// printf. They are the original music_xtrack_play/music_xtrack_stop commands.
// Keep their four-slot timing semantics here, then drive the XBLA mission-music
// state machine (sub_82144C60), which already performs the native XACT lookup,
// Track 2 playback, and Track 1 cross-fades from music.xsb/music.xwb.
struct GEXTrackSlot {
  bool active = false;
  double minimum_seconds = 0.0;
  double total_seconds = 0.0;
};

std::array<GEXTrackSlot, 4> g_xtrack_slots{};
std::chrono::steady_clock::time_point g_xtrack_last_tick =
    std::chrono::steady_clock::now();
bool g_xtrack_had_player = false;
uint32_t g_xtrack_last_music_state = UINT32_MAX;

bool ge_any_xtrack_slot_active() {
  for (const GEXTrackSlot& slot : g_xtrack_slots) {
    if (slot.total_seconds > 0.0 &&
        (slot.active || slot.minimum_seconds > 0.0)) {
      return true;
    }
  }
  return false;
}

void ge_stop_native_music_cue(PPCContext& ctx, uint8_t* base,
                              uint32_t slot) {
  ctx.r3.u32 = slot;
  sub_82144A80(ctx, base);
}

void ge_start_stage_primary_cue(PPCContext& ctx, uint8_t* base) {
  // This is the same lookup/play sequence used by sub_82144C60 for a normal
  // 0 -> 1 mission-music transition, kept here because its 2 -> 1 transition
  // assumes the (missing) fade code merely left Track 1 running silently.
  ctx.r3.u32 = LD32(base, GE_STAGE_MUSIC_ID);
  sub_821453C0(ctx, base);
  const uint32_t cue_id = ctx.r3.u32;
  if (static_cast<int32_t>(cue_id) < 0) return;
  ctx.r3.u32 = 0u;
  ctx.r4.u32 = cue_id;
  sub_82144BA8(ctx, base);
}

void ge_start_stage_secondary_cue(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u32 = LD32(base, GE_STAGE_MUSIC_ID);
  // sub_82145518 is the stage-specific X-track lookup used by the native
  // 1 -> 2 and 4 -> 5 transitions. sub_821454C0 is ambience (slot 2), not the
  // elevator cue.
  sub_82145518(ctx, base);
  const uint32_t cue_id = ctx.r3.u32;
  if (static_cast<int32_t>(cue_id) < 0) return;
  ctx.r3.u32 = 1u;
  ctx.r4.u32 = cue_id;
  sub_82144BA8(ctx, base);
}

void ge_set_mission_music_state(PPCContext& source_ctx, uint8_t* base,
                                uint32_t target_state) {
  const uint32_t old_state = LD32(base, GE_MUSIC_STATE);
  if (old_state == target_state) return;

  const bool entering_xtrack =
      (old_state == 1u && target_state == 2u) ||
      (old_state == 4u && target_state == 5u);
  const bool leaving_xtrack =
      (old_state == 2u && target_state == 1u) ||
      (old_state == 5u && target_state == 4u);

  // Preserve the AI/input caller's registers. The native PPC function uses the
  // copied guest context and the real guest stack/memory, just like an ordinary
  // PPC call, and ultimately reaches the game's existing XACT music manager.
  PPCContext call_ctx = source_ctx;
  call_ctx.r3.u32 = target_state;
  sub_82144C60(call_ctx, base);

  if (entering_xtrack) {
    // The transition has created Track 2, but its original Track-1 fade call
    // is a no-op in XBLA. Use the game's well-tested cue-stop wrapper instead.
    ge_stop_native_music_cue(call_ctx, base, 0u);
  } else if (leaving_xtrack) {
    // Its Track-2 fade-out is the same no-op. Stop Track 2, then recreate the
    // stage's primary cue through the same native lookup/play path used at
    // mission start. This restarts rather than cross-fades, but stays entirely
    // within already-exercised game wrappers and avoids unsafe vtable guesses.
    ge_stop_native_music_cue(call_ctx, base, 1u);
    ge_start_stage_primary_cue(call_ctx, base);
  }

  g_xtrack_last_music_state = LD32(base, GE_MUSIC_STATE);
  REXKRNL_INFO("GEXTRACK state {} -> {} (native cue slot1={:08X})",
               old_state, LD32(base, GE_MUSIC_STATE),
               LD32(base, GE_MUSIC_CUES + 4u));
}

void ge_refresh_xtrack_state(PPCContext& ctx, uint8_t* base) {
  const uint32_t state = LD32(base, GE_MUSIC_STATE);

  const bool supported_mission_state =
      state == 1u || state == 2u || state == 4u || state == 5u;
  const bool prior_state_was_xtrack =
      g_xtrack_last_music_state == 2u || g_xtrack_last_music_state == 5u;
  const bool state_was_reset_externally =
      prior_state_was_xtrack && (state == 1u || state == 4u);

  // Mission abort/restart can retain a valid player pointer while the native
  // music manager tears down and recreates its cues.  Treat an externally
  // observed X -> normal transition (or any non-mission music state) as the
  // lifecycle boundary it is. Otherwise Control's 255-second slot survives a
  // restart and immediately suppresses the next run's primary level theme.
  if (!supported_mission_state || state_was_reset_externally) {
    if (ge_any_xtrack_slot_active()) {
      g_xtrack_slots = {};
      REXKRNL_INFO("GEXTRACK cleared stale slots at native state {}", state);
    }
    g_xtrack_last_music_state = state;
    return;
  }

  g_xtrack_last_music_state = state;
  const bool wants_xtrack = ge_any_xtrack_slot_active();

  // XBLA states mirror the shipped transition table: 1/4 are normal mission
  // music without/with ambience; 2/5 are their corresponding X-track states.
  if (wants_xtrack) {
    if (state == 1u) ge_set_mission_music_state(ctx, base, 2u);
    else if (state == 4u) ge_set_mission_music_state(ctx, base, 5u);
  } else {
    if (state == 2u) ge_set_mission_music_state(ctx, base, 1u);
    else if (state == 5u) ge_set_mission_music_state(ctx, base, 4u);
  }
}

void ge_xtrack_tick(PPCContext& ctx, uint8_t* base, bool player_active) {
  const auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - g_xtrack_last_tick).count();
  g_xtrack_last_tick = now;

  if (!player_active) {
    if (g_xtrack_had_player) {
      g_xtrack_slots = {};
      g_xtrack_had_player = false;
    }
    g_xtrack_last_music_state = LD32(base, GE_MUSIC_STATE);
    return;
  }
  g_xtrack_had_player = true;

  // The original uses its simulation ClockTimer, so timers do not advance while
  // paused. Clamp a long host stall as well; loading/alt-tab must not consume an
  // entire minimum-duration window in one input poll.
  if (LD32(base, GE_PAUSE_FLAG) != 0u) elapsed = 0.0;
  if (elapsed > 0.25) elapsed = 0.25;

  if (elapsed > 0.0) {
    for (GEXTrackSlot& slot : g_xtrack_slots) {
      if (!slot.active && slot.minimum_seconds <= 0.0) continue;
      if (slot.minimum_seconds > 0.0) {
        slot.minimum_seconds -= elapsed;
        if (slot.minimum_seconds < 0.0) slot.minimum_seconds = 0.0;
      }
      if (slot.total_seconds > 0.0) {
        slot.total_seconds -= elapsed;
        if (slot.total_seconds <= 0.0) {
          slot.total_seconds = 0.0;
          slot.active = false;
        }
      }
    }
  }

  ge_refresh_xtrack_state(ctx, base);
}

// The leaked XBLA build contains the authentic watch and Mission Select cues in
// music.xwb, but several N64 music-script transitions were never wired into
// this version. Supply those transitions through the game's native XACT music
// manager from the existing once-per-frame hook; no extracted audio is needed.
enum class GENativeRestoredMusic {
  None,
  MissionSelect,
  Watch,
};

GENativeRestoredMusic g_native_restored_music =
    GENativeRestoredMusic::None;

// Values accepted by sub_82144BA8 are the game's logical music indices, not
// raw XACT cue indices. The runtime translation table maps 23 -> XACT 11
// (Mission Select) and 24 -> XACT 32 (watch theme).
constexpr uint32_t GE_LOGICAL_CUE_MISSION_SELECT = 23u;
constexpr uint32_t GE_LOGICAL_CUE_WATCH = 24u;

bool ge_start_native_restored_music(PPCContext& source_ctx, uint8_t* base,
                                    GENativeRestoredMusic kind,
                                    uint32_t slot, uint32_t logical_cue) {
  PPCContext call_ctx = source_ctx;
  call_ctx.r3.u32 = slot;
  call_ctx.r4.u32 = logical_cue;
  sub_82144BA8(call_ctx, base);
  if (LD32(base, GE_MUSIC_CUES + slot * 4u) == 0u) return false;
  g_native_restored_music = kind;
  return true;
}

uint32_t ge_find_active_viewport_player(uint8_t* base) {
  uint32_t player = 0;
  for (int i = 0; i < 4; ++i) {
    uint32_t candidate = LD32(base, GE_PLAYER_PTR + i * 4u);
    if (candidate && LD32(base, candidate + 0x904u) == 0u) {
      player = candidate;
      break;
    }
  }
  return player;
}

uint32_t ge_find_active_player(uint8_t* base) {
  uint32_t player = ge_find_active_viewport_player(base);
  if (!player) player = LD32(base, GE_BONDVIEW_CUR);
  if (!player) player = LD32(base, GE_PLAYER_PTR);
  return player;
}

void ge_missing_music_tick(PPCContext& ctx, uint8_t* base) {
  static bool previous_watch_open = false;
  static bool saw_active_non_title_stage = false;
  static bool title_music_pending = false;
  static uint32_t title_settle_frames = 0;
  static uint32_t watch_saved_music_state = 0;
  static bool watch_stopped_native_tracks = false;
  const uint32_t active_viewport_player =
      ge_find_active_viewport_player(base);
  const uint32_t player = ge_find_active_player(base);
  const bool player_active = active_viewport_player != 0;
  const uint32_t music_state = LD32(base, GE_MUSIC_STATE);
  const uint32_t pending_stage = LD32(base, GE_PENDING_STAGE);
  const uint32_t current_stage = LD32(base, GE_CURRENT_STAGE);
  const bool title_requested = current_stage == GE_TITLE_STAGE ||
                               pending_stage == GE_TITLE_STAGE;

  // The outgoing mission's viewport/player pointers can remain valid while
  // stage 90 is loading.  Do not let that stale player keep an elevator
  // X-track timer alive behind Mission Select, where its eventual expiry would
  // restart the previous stage's primary music.
  if (title_requested && ge_any_xtrack_slot_active()) {
    REXKRNL_INFO("GEXTRACK clearing slots for Mission Select transition");
  }
  ge_xtrack_tick(ctx, base, player_active && !title_requested);

  // Some XBLA exits write the boss stage globals directly and never call the
  // N64-style setter. Arm only after an actual player is active in a non-title
  // stage, then observe either current or pending stage becoming title (90).
  // This covers abort, failure, and completion without playing on initial boot.
  if (player_active && current_stage != GE_TITLE_STAGE && !title_requested) {
    saw_active_non_title_stage = true;
  }

  if (saw_active_non_title_stage && title_requested) {
    saw_active_non_title_stage = false;
    title_music_pending = true;
    title_settle_frames = 0;
    REXKRNL_INFO("GEMISSINGMUSIC armed Mission Select cue at stage transition current={} pending={}",
                 current_stage, pending_stage);
  }

  // pending=90 is written while the outgoing level is still current. Starting
  // a cue at that point succeeds, but the subsequent title-stage audio reset
  // destroys it. Wait until stage 90 has been current for a few input ticks so
  // the title audio system has finished resetting its native cue slots.
  if (title_music_pending && current_stage == GE_TITLE_STAGE &&
      ++title_settle_frames >= 3u) {
    title_music_pending = false;
    ge_stop_native_music_cue(ctx, base, 0u);
    ge_stop_native_music_cue(ctx, base, 1u);
    g_native_restored_music = GENativeRestoredMusic::None;
    watch_stopped_native_tracks = false;
    if (ge_start_native_restored_music(
            ctx, base, GENativeRestoredMusic::MissionSelect, 0u,
            GE_LOGICAL_CUE_MISSION_SELECT)) {
      REXKRNL_INFO("GEMISSINGMUSIC started native XACT Mission Select cue at stage transition current={} pending={}",
                   current_stage, pending_stage);
    } else {
      REXKRNL_ERROR("GEMISSINGMUSIC could not play native Mission Select current={} pending={}",
                    current_stage, pending_stage);
    }
  } else if (!title_requested &&
             (player_active || music_state != 0u) &&
             g_native_restored_music == GENativeRestoredMusic::MissionSelect) {
    // A level's own startup replaces slot 0, so relinquish ownership without
    // stopping the new stage cue.
    g_native_restored_music = GENativeRestoredMusic::None;
  }

  const bool watch_open = player && LD32(base, GE_PAUSE_FLAG) != 0u &&
                          LD32(base, player + GE_OFF_WATCH) != 0u;
  if (watch_open == previous_watch_open) return;
  previous_watch_open = watch_open;

  if (watch_open) {
    watch_saved_music_state = music_state;
    watch_stopped_native_tracks = true;
    ge_stop_native_music_cue(ctx, base, 0u);
    ge_stop_native_music_cue(ctx, base, 1u);
    g_native_restored_music = GENativeRestoredMusic::None;
    if (ge_start_native_restored_music(
            ctx, base, GENativeRestoredMusic::Watch, 0u,
            GE_LOGICAL_CUE_WATCH)) {
      REXKRNL_INFO("GEWATCHMUSIC started native XACT watch cue");
    } else {
      REXKRNL_ERROR("GEWATCHMUSIC could not play native watch cue");
    }
  } else {
    const bool had_native_watch =
        g_native_restored_music == GENativeRestoredMusic::Watch;
    if (had_native_watch) {
      ge_stop_native_music_cue(ctx, base, 0u);
      g_native_restored_music = GENativeRestoredMusic::None;
    }
    if (watch_stopped_native_tracks && !title_requested) {
      PPCContext restore_ctx = ctx;
      if (watch_saved_music_state == 2u || watch_saved_music_state == 5u ||
          ge_any_xtrack_slot_active()) {
        ge_start_stage_secondary_cue(restore_ctx, base);
        REXKRNL_INFO("GEWATCHMUSIC restored elevator/X-track cue");
      } else {
        ge_start_stage_primary_cue(restore_ctx, base);
        REXKRNL_INFO("GEWATCHMUSIC restored primary level cue");
      }
    }
    watch_stopped_native_tracks = false;
  }
}
}  // namespace

// AI opcode F4: music_xtrack_play(slot, minimum_seconds, total_seconds).
// The original XBLA block at 0x82135BF0 advances r30 by four bytes and then
// prints an unimplemented-command message. ge_config.toml skips that whole
// block, so this hook performs the cursor update as well as the missing action.
void ge_music_xtrack_play() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);

  const uint32_t command = ctx->r31.u32;
  const int32_t slot_index = static_cast<int8_t>(base[command + 1u]);
  const uint8_t minimum_seconds = base[command + 2u];
  const uint8_t total_seconds = base[command + 3u];
  ctx->r30.u32 += 4u;

  if (slot_index < 0 || slot_index >= static_cast<int32_t>(g_xtrack_slots.size())) {
    REXKRNL_ERROR("GEXTRACK ignored F4 with invalid slot {}", slot_index);
    return;
  }

  GEXTrackSlot& slot = g_xtrack_slots[static_cast<size_t>(slot_index)];
  if (!slot.active) {
    slot.active = true;
    slot.minimum_seconds = static_cast<double>(minimum_seconds);
    slot.total_seconds = static_cast<double>(total_seconds);
    REXKRNL_INFO("GEXTRACK F4 play slot={} minimum={} total={}",
                 slot_index, minimum_seconds, total_seconds);
  }

  ge_refresh_xtrack_state(*ctx, base);
}

// AI opcode F5: music_xtrack_stop(slot). A negative slot means all four slots.
void ge_music_xtrack_stop() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);

  const uint32_t command = ctx->r31.u32;
  const int32_t slot_index = static_cast<int8_t>(base[command + 1u]);
  ctx->r30.u32 += 2u;

  if (slot_index < 0) {
    g_xtrack_slots = {};
    REXKRNL_INFO("GEXTRACK F5 stopped all slots");
  } else if (slot_index < static_cast<int32_t>(g_xtrack_slots.size())) {
    // Match the original semantics: stopping clears the active flag but leaves
    // the minimum-duration timer running before the main track may return.
    g_xtrack_slots[static_cast<size_t>(slot_index)].active = false;
    REXKRNL_INFO("GEXTRACK F5 stop slot={}", slot_index);
  } else {
    REXKRNL_ERROR("GEXTRACK ignored F5 with invalid slot {}", slot_index);
  }

  ge_refresh_xtrack_state(*ctx, base);
}

void ge_mouse_camera(uint8_t* base) {
  // Persistent state (= GoldeneyeGame member vars in xenia).
  static uint32_t prev_pause = 0, prev_disabled = 0, prev_aim_mode = 0;
  static bool start_centering = false, disable_sway = false;
  static float centering_speed = 0.0125f;

  const float sensitivity = static_cast<float>(REXCVAR_GET(ge_mouse_sens));
  const float menu_sensitivity = static_cast<float>(REXCVAR_GET(ge_menu_sensitivity));
  const bool invert_x = REXCVAR_GET(ge_invert_x);
  const bool invert_y = REXCVAR_GET(ge_invert_y);
  const bool disable_autoaim = REXCVAR_GET(ge_disable_autoaim);
  const bool gun_sway = REXCVAR_GET(ge_gun_sway);

  // Consume this frame's raw mouse delta once; used for both menu and camera.
  const float mdx = static_cast<float>(ge_take_mouse_dx());
  const float mdy = static_cast<float>(ge_take_mouse_dy());

  // Move the menu selection crosshair (the game's own menus read these).
  {
    float menuX = LDF32(base, GE_MENU_XY);
    float menuY = LDF32(base, GE_MENU_XY + 4);
    menuX += (mdx / 5.f) * menu_sensitivity;
    menuY += (mdy / 5.f) * menu_sensitivity;
    STF32(base, GE_MENU_XY, menuX);
    STF32(base, GE_MENU_XY + 4, menuY);
  }

  // Target the LOCAL player. Online uses Xbox System Link, where each console
  // controls its OWN Bond at a session-global index (host = players[0], clients =
  // players[1..3]). players[0] (GE_PLAYER_PTR) is therefore only the local player
  // on the host -- using it made mouse-look host-only. GE_BONDVIEW_CUR points at
  // the view the local console is actually driving, so it resolves to this
  // console's player in online play and to players[0] in single-player/the host.
  // Target the LOCAL player = the active-viewport player. player+0x904 (viewport
  // size/offset) is 0 only for the view the local console actually renders -- the
  // same signal GoldenEye's own code uses ("current player is the active
  // viewport", per the CE 3D-SFX hack). This is stable every frame and resolves
  // to players[0] on the host, players[1] on a joiner, etc., automatically.
  // (The old GE_BONDVIEW_CUR target flipped between local/remote each frame
  // because the bondview CONTROL loop cycles it across all players -- that was
  // the online jitter.)
  uint32_t player = 0;
  for (int i = 0; i < 4; ++i) {
    uint32_t p = LD32(base, 0x82F1FA98u + i * 4u);
    if (p && LD32(base, p + 0x904u) == 0u) { player = p; break; }
  }
  if (!player) player = LD32(base, GE_BONDVIEW_CUR);  // fallback (menus/boot)
  if (!player) player = LD32(base, GE_PLAYER_PTR);
  if (!player) return;

  const uint32_t game_pause_flag = LD32(base, GE_PAUSE_FLAG);

  // control-disabled (cutscene); fall back to watch-status (watch up/down).
  uint32_t game_control_disabled = LD32(base, player + GE_OFF_DISABLED);
  if (game_control_disabled == 0)
    game_control_disabled = LD32(base, player + GE_OFF_WATCH);

  // Disable auto-aim & look-ahead, only when the pause/control state changes --
  // xenia's exact behaviour. (Doing it every frame oscillated against the game's
  // per-frame auto-aim in multiplayer and caused the camera jitter.)
  if (game_pause_flag != prev_pause || game_control_disabled != prev_disabled) {
    const uint32_t sp = LD32(base, GE_SETTINGS_PTR);
    if (sp) {
      const uint32_t sva = sp + GE_SETTINGS_BITS;
      uint32_t settings = LD32(base, sva);
      if (settings & GE_SET_LookAhead) settings &= ~(uint32_t)GE_SET_LookAhead;
      if (disable_autoaim && (settings & GE_SET_AutoAim))
        settings &= ~(uint32_t)GE_SET_AutoAim;
      ST32(base, sva, settings);
    }
    prev_pause = game_pause_flag;
    prev_disabled = game_control_disabled;
  }

  if (game_control_disabled) return;

  // The ordinary camera yaw is rebuilt from the tank body + turret every
  // simulation tick, so writing player->vv_theta (the on-foot mouse path)
  // cannot turn the tank horizontally. Shift both the native turret target and
  // its smoothed current value by the same mouse delta instead. This preserves
  // the game's steering, collision rollback, controller input, and interpolation
  // while making mouse-X act on the same turret state that native tank controls
  // ultimately drive. The accumulator uses the reciprocal constant from this
  // exact XBLA build rather than assuming the N64 smoothing rate.
  const bool in_tank = LD32(base, GE_IN_TANK_FLAG) == 1u &&
                       LD32(base, GE_PLAYER_TANK_PROP) != 0u;
  float look_mdx = mdx;
  if (in_tank && mdx != 0.f) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTau = 2.f * kPi;
    const float delta_rad =
        (invert_x ? -1.f : 1.f) * (mdx / 10.f) * sensitivity *
        (kPi / 180.f);
    auto wrap_tau = [kTau](float angle) {
      angle = std::fmod(angle, kTau);
      if (angle < 0.f) angle += kTau;
      return angle;
    };

    const float target =
        wrap_tau(LDF32(base, GE_TANK_TURRET_TARGET) + delta_rad);
    const float current =
        wrap_tau(LDF32(base, GE_TANK_TURRET_RAD) + delta_rad);
    STF32(base, GE_TANK_TURRET_TARGET, target);
    STF32(base, GE_TANK_TURRET_RAD, current);
    STF32(base, GE_TANK_TURRET_ACCUM,
          current * LDF32(base, GE_TANK_TURRET_RECIP));
    look_mdx = 0.f;  // do not also write the transient on-foot camera yaw
  }

  const uint32_t aim_mode = LD32(base, player + GE_OFF_AIM_MODE);
  if (aim_mode != prev_aim_mode) {
    if (aim_mode != 0) {  // entering aim mode -> reset gun position
      STF32(base, player + GE_OFF_GUN_X, 0.f);
      STF32(base, player + GE_OFF_GUN_Y, 0.f);
    }
    // Always reset crosshair on enter/exit (else non-aim fires toward it).
    STF32(base, player + GE_OFF_CH_X, 0.f);
    STF32(base, player + GE_OFF_CH_Y, 0.f);
    prev_aim_mode = aim_mode;
  }

  const float bounds = 1.f;
  const float crosshair_multiplier = 1.f, centering_multiplier = 1.f;

  if (aim_mode == 1) {
    // #61: DIRECT 1:1 mouse aim. The old Xenia crosshair-travel mechanic moved a
    // free crosshair and only turned the camera past a threshold; the game's
    // native aim-mode auto-centering then sprang the crosshair/view back to the
    // screen centre the instant the mouse stopped (the "snaps to middle" bug).
    // Instead drive the camera straight from the mouse (same feel as hip-fire /
    // v1.2.2) and hold the crosshair + gun centred, so there is nothing for the
    // game to spring back to.
    if (look_mdx != 0.f || mdy != 0.f) {
      float camX = LDF32(base, player + GE_OFF_CAM_X);
      float camY = LDF32(base, player + GE_OFF_CAM_Y);
      camX += (invert_x ? -1.f : 1.f) * (look_mdx / 10.f) * sensitivity;
      camY -= (invert_y ? -1.f : 1.f) * (mdy / 10.f) * sensitivity;
      STF32(base, player + GE_OFF_CAM_X, camX);
      STF32(base, player + GE_OFF_CAM_Y, camY);
    }
    STF32(base, player + GE_OFF_CH_X, 0.f);
    STF32(base, player + GE_OFF_CH_Y, 0.f);
    STF32(base, player + GE_OFF_GUN_X, 0.f);
    STF32(base, player + GE_OFF_GUN_Y, 0.f);
    start_centering = false;   // nothing to centre -> no spring-back
    disable_sway = false;
  } else {
    float gX = LDF32(base, player + GE_OFF_GUN_X);
    float gY = LDF32(base, player + GE_OFF_GUN_Y);

    // Gun-centering back to the middle after aim-mode / when idle.
    if (start_centering) {
      if (gX != 0 || gY != 0) {
        if (gX > 0) gX -= std::min(centering_speed * centering_multiplier, gX);
        if (gX < 0) gX += std::min(centering_speed * centering_multiplier, -gX);
        if (gY > 0) gY -= std::min(centering_speed * centering_multiplier, gY);
        if (gY < 0) gY += std::min(centering_speed * centering_multiplier, -gY);
      }
      if (gX == 0 && gY == 0) {
        centering_speed = 0.0125f;
        start_centering = false;
        disable_sway = false;
      }
    }

    if (look_mdx != 0.f || mdy != 0.f) {
      float camX = LDF32(base, player + GE_OFF_CAM_X);
      float camY = LDF32(base, player + GE_OFF_CAM_Y);

      camX += (invert_x ? -1.f : 1.f) * (look_mdx / 10.f) * sensitivity;

      // Add 'sway' to the gun as the camera turns.
      const float gun_sway_x = ((look_mdx / 16000.f) * sensitivity) * bounds;
      const float gun_sway_y = ((mdy / 16000.f) * sensitivity) * bounds;
      float gun_sway_x_changed = gX + gun_sway_x;
      float gun_sway_y_changed = gY + gun_sway_y;

      if (!invert_y) {
        camY -= (mdy / 10.f) * sensitivity;
      } else {
        camY += (mdy / 10.f) * sensitivity;
        gun_sway_y_changed = gY - gun_sway_y;
      }

      STF32(base, player + GE_OFF_CAM_X, camX);
      STF32(base, player + GE_OFF_CAM_Y, camY);

      if (gun_sway && !disable_sway) {
        // Bound the sway to [0.2:-0.2] (only if it would push further OOB).
        if (gun_sway_x_changed > (0.2f * bounds) && gun_sway_x > 0) gun_sway_x_changed = gX;
        if (gun_sway_x_changed < -(0.2f * bounds) && gun_sway_x < 0) gun_sway_x_changed = gX;
        if (gun_sway_y_changed > (0.2f * bounds) && gun_sway_y > 0) gun_sway_y_changed = gY;
        if (gun_sway_y_changed < -(0.2f * bounds) && gun_sway_y < 0) gun_sway_y_changed = gY;
        gX = gun_sway_x_changed;
        gY = gun_sway_y_changed;
      }
    } else {
      if (!start_centering) {
        start_centering = true;
        centering_speed = 0.0125f;
      }
    }

    gX = std::min(gX, bounds); gX = std::max(gX, -bounds);
    gY = std::min(gY, bounds); gY = std::max(gY, -bounds);

    STF32(base, player + GE_OFF_CH_X, gX * crosshair_multiplier);
    STF32(base, player + GE_OFF_CH_Y, gY * crosshair_multiplier);
    STF32(base, player + GE_OFF_GUN_X, gX);
    STF32(base, player + GE_OFF_GUN_Y, gY);
  }
}

// ===========================================================================
// Keyboard buttons -> guest gamepad. The right stick (look) is the mouse; every
// other controller input is mapped to a rebindable keyboard key here, injected
// into the polled gamepad buffer so it works alongside a real pad. We do this
// ourselves (not via the SDK's MnK driver) so it can't fight the mouse capture.
//
// Slot-0 gamepad buffer (filled by XamInputGetState in ge_input_poll_controllers,
// Xbox360 big-endian): +0 buttons(u16), +2 LT, +3 RT, +4 LX(s16), +6 LY(s16).
// ===========================================================================
namespace {
constexpr uint32_t GE_PAD0 = 0x830C8B9Cu;  // unk_830C8B9C, slot-0 gamepad

// XInput button bits (match the masks the guest unpacks).
constexpr uint16_t BTN_DPAD_UP = 0x0001, BTN_DPAD_DOWN = 0x0002, BTN_DPAD_LEFT = 0x0004,
                   BTN_DPAD_RIGHT = 0x0008, BTN_START = 0x0010, BTN_BACK = 0x0020,
                   BTN_LTHUMB = 0x0040, BTN_RTHUMB = 0x0080, BTN_LSHOULDER = 0x0100,
                   BTN_RSHOULDER = 0x0200, BTN_A = 0x1000, BTN_B = 0x2000, BTN_X = 0x4000,
                   BTN_Y = 0x8000;

bool ge_input_active() {  // keyboard counts only when focused + not in the menu
  return !g_mouselook_suppressed.load(std::memory_order_relaxed) && ge_game_has_focus();
}

// Is any key bound to cvar `name` held down? The bind may list SEVERAL keys
// separated by commas (#63 "multiple keys per function", e.g. "W,Up") -- held if
// ANY of them is down. Each key name parses to a virtual key (== Windows VK code).
bool ge_key_down(const char* name) {
  std::string binds = rex::cvar::GetFlagByName(name);
  if (binds.empty()) return false;
  size_t start = 0;
  while (start <= binds.size()) {
    size_t comma = binds.find(',', start);
    std::string one = binds.substr(start, comma == std::string::npos
                                              ? std::string::npos : comma - start);
    while (!one.empty() && (one.front() == ' ' || one.front() == '\t')) one.erase(one.begin());
    while (!one.empty() && (one.back() == ' ' || one.back() == '\t')) one.pop_back();
    if (!one.empty()) {
      rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(one);
      if (vk != rex::ui::VirtualKey::kNone &&
          (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0)
        return true;
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}
}  // namespace

// Keyboard binds. Defaults are a placeholder layout; the user's preferred layout
// is hard-set at boot (ge_app OnConfigurePaths) and rebindable in the menu.
REXCVAR_DEFINE_BOOL(ge_keyboard_enable, true, "Input", "Map keyboard keys to controller buttons");
REXCVAR_DEFINE_STRING(ge_key_mv_up, "W", "Input/Keybinds", "Move forward (left stick up)");
REXCVAR_DEFINE_STRING(ge_key_mv_down, "S", "Input/Keybinds", "Move back (left stick down)");
REXCVAR_DEFINE_STRING(ge_key_mv_left, "A", "Input/Keybinds", "Move left (left stick left)");
REXCVAR_DEFINE_STRING(ge_key_mv_right, "D", "Input/Keybinds", "Move right (left stick right)");
REXCVAR_DEFINE_STRING(ge_key_a, "Space", "Input/Keybinds", "A button");
REXCVAR_DEFINE_STRING(ge_key_b, "Control", "Input/Keybinds", "B button");
REXCVAR_DEFINE_STRING(ge_key_x, "R", "Input/Keybinds", "X button");
REXCVAR_DEFINE_STRING(ge_key_y, "E", "Input/Keybinds", "Y button");
REXCVAR_DEFINE_STRING(ge_key_lt, "RMB", "Input/Keybinds", "Left trigger");
REXCVAR_DEFINE_STRING(ge_key_rt, "LMB", "Input/Keybinds", "Right trigger");
REXCVAR_DEFINE_STRING(ge_key_lb, "Q", "Input/Keybinds", "Left shoulder");
REXCVAR_DEFINE_STRING(ge_key_rb, "F", "Input/Keybinds", "Right shoulder");
REXCVAR_DEFINE_STRING(ge_key_l3, "C", "Input/Keybinds", "Left stick press");
REXCVAR_DEFINE_STRING(ge_key_r3, "V", "Input/Keybinds", "Right stick press");
REXCVAR_DEFINE_STRING(ge_key_dup, "Up", "Input/Keybinds", "D-pad up");
REXCVAR_DEFINE_STRING(ge_key_ddown, "Down", "Input/Keybinds", "D-pad down");
REXCVAR_DEFINE_STRING(ge_key_dleft, "Left", "Input/Keybinds", "D-pad left");
REXCVAR_DEFINE_STRING(ge_key_dright, "Right", "Input/Keybinds", "D-pad right");
REXCVAR_DEFINE_STRING(ge_key_start, "Return", "Input/Keybinds", "Start button");
REXCVAR_DEFINE_STRING(ge_key_back, "Tab", "Input/Keybinds", "Back button");
// Right analog stick (look/aim) as keyboard binds (#63). Unbound by default so
// they never fight mouse-look; bind them (e.g. arrow keys) for keyboard-only look.
// They feed the guest's native right-stick, so they work alongside the mouse.
REXCVAR_DEFINE_STRING(ge_key_look_up, "", "Input/Keybinds", "Look up (right stick up)");
REXCVAR_DEFINE_STRING(ge_key_look_down, "", "Input/Keybinds", "Look down (right stick down)");
REXCVAR_DEFINE_STRING(ge_key_look_left, "", "Input/Keybinds", "Look left (right stick left)");
REXCVAR_DEFINE_STRING(ge_key_look_right, "", "Input/Keybinds", "Look right (right stick right)");

// Runs once per controller poll, after XamInputGetState fills the slot-0 buffer
// and before the guest dispatches it. OR our keyboard buttons in, and set the
// left stick / triggers when their keys are held (pad input is preserved).
void ge_mouse_camera(uint8_t* base);  // defined above
void ge_apply_ce_data_patches(uint8_t* base);  // ge_ce_patches.cpp

void ge_inject_keyboard(PPCRegister& /*r11*/) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);

  // Apply BeanTools community DATA bug-fixes once, before any level loads its
  // setup/fog/BG data. The data segment is live in guest RAM by the first input
  // poll (menu), which precedes any level load.
  static bool ce_patched = false;
  if (!ce_patched) {
    ce_patched = true;
    ge_apply_ce_data_patches(base);
    REXKRNL_INFO("GECE community data bug-fixes applied");
  }

  // Restore missing XBLA music transitions regardless of whether keyboard or
  // mouse-look support is enabled.
  ge_missing_music_tick(*ctx, base);

  // Rebind capture: the menu is listening for a key to bind. Swallow ALL slot-0
  // controller input (buttons, triggers, both sticks) so the key/button being
  // bound doesn't also drive the game, and skip keyboard injection + mouse-look.
  if (g_rebind_capturing.load(std::memory_order_relaxed)) {
    ST16(base, GE_PAD0 + 0, 0);   // buttons
    base[GE_PAD0 + 2] = 0;        // LT
    base[GE_PAD0 + 3] = 0;        // RT
    ST16(base, GE_PAD0 + 4, 0);   // LX
    ST16(base, GE_PAD0 + 6, 0);   // LY
    ST16(base, GE_PAD0 + 8, 0);   // RX
    ST16(base, GE_PAD0 + 10, 0);  // RY
    return;
  }

  // Mouse look runs every frame here, independent of the keyboard toggle. The
  // raw-mouse thread only accumulates deltas while the game is focused and the
  // cursor is captured, so this is a no-op in menus / when unfocused.
  ge_start_mouse_once();
  if (REXCVAR_GET(ge_mouselook_enable)) ge_mouse_camera(base);

  if (!REXCVAR_GET(ge_keyboard_enable) || !ge_input_active()) return;

  uint16_t add = 0;
  if (ge_key_down("ge_key_a")) add |= BTN_A;
  if (ge_key_down("ge_key_b")) add |= BTN_B;
  if (ge_key_down("ge_key_x")) add |= BTN_X;
  if (ge_key_down("ge_key_y")) add |= BTN_Y;
  if (ge_key_down("ge_key_lb")) add |= BTN_LSHOULDER;
  if (ge_key_down("ge_key_rb")) add |= BTN_RSHOULDER;
  if (ge_key_down("ge_key_l3")) add |= BTN_LTHUMB;
  if (ge_key_down("ge_key_r3")) add |= BTN_RTHUMB;
  if (ge_key_down("ge_key_dup")) add |= BTN_DPAD_UP;
  if (ge_key_down("ge_key_ddown")) add |= BTN_DPAD_DOWN;
  if (ge_key_down("ge_key_dleft")) add |= BTN_DPAD_LEFT;
  if (ge_key_down("ge_key_dright")) add |= BTN_DPAD_RIGHT;
  if (ge_key_down("ge_key_start")) add |= BTN_START;
  if (ge_key_down("ge_key_back")) add |= BTN_BACK;
  if (add) ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | add);

  if (ge_key_down("ge_key_lt")) base[GE_PAD0 + 2] = 0xFF;
  if (ge_key_down("ge_key_rt")) base[GE_PAD0 + 3] = 0xFF;

  int16_t lx = 0, ly = 0;
  if (ge_key_down("ge_key_mv_left")) lx = -32767;
  if (ge_key_down("ge_key_mv_right")) lx = 32767;
  if (ge_key_down("ge_key_mv_up")) ly = 32767;
  if (ge_key_down("ge_key_mv_down")) ly = -32767;
  if (lx) ST16(base, GE_PAD0 + 4, static_cast<uint16_t>(lx));
  if (ly) ST16(base, GE_PAD0 + 6, static_cast<uint16_t>(ly));

  // Right stick (look/aim) -> slot-0 gamepad RX(+8)/RY(+10), s16 BE (#63). Feeds
  // the guest's native right-stick look, so it coexists with mouse-look.
  int16_t rx = 0, ry = 0;
  if (ge_key_down("ge_key_look_left")) rx = -32767;
  if (ge_key_down("ge_key_look_right")) rx = 32767;
  if (ge_key_down("ge_key_look_up")) ry = 32767;
  if (ge_key_down("ge_key_look_down")) ry = -32767;
  if (rx) ST16(base, GE_PAD0 + 8, static_cast<uint16_t>(rx));
  if (ry) ST16(base, GE_PAD0 + 10, static_cast<uint16_t>(ry));
}

// ===========================================================================
// BeanTools Community Edition CODE fixes (instruction patches replicated as
// midasm hooks; the recomp runs generated C++ so the xex bytes can't be patched
// directly). Addresses/values 1:1 with finalizer.c. Data-only CE fixes live in
// ge_ce_patches.cpp.
// ===========================================================================

// fix_door_volume_clamp @0x820DD814: `li r3,0` -> `li r3,1` (min volume for
// distant doors; 0 overflows). After-hook forces r3 = 1.
void ge_ce_door_vol(PPCRegister& r3) { r3.u32 = 1; }

// remove_beta_string_at_logo @0x820ED678: `ori r3,r3,0x9D97` -> `...0x9CE3`
// (point the GoldenEye-logo string id at the empty string). Replace low half.
void ge_ce_beta_str(PPCRegister& r3) {
  r3.u32 = (r3.u32 & 0xFFFF0000u) | 0x9CE3u;
}

// extend_audio_distance, store site 0x8214438C: original `stfs f0,0x5C(r31)`
// stored a small default scaler; CE makes X3DEmitter->CurveDistanceScaler =
// 6500.0f. Re-store 6500.0f (0x45CB2000) to r31+0x5C after the original store.
void ge_ce_audio_dist(PPCRegister& r31) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  ST32(base, r31.u32 + 0x5Cu, 0x45CB2000u);  // 6500.0f
}

// hardcode_near_clip_to_2, per-fog store site 0x82117B44: original
// `stfs f0,0x14(r11)` writes the fog entry's near-clip into the global. CE NOOPs
// it and pins the global to 2.0f. Can't NOOP a store in the recomp, so re-write
// the just-stored slot (r11+0x14 == the near-clip global) back to 2.0f each load.
void ge_ce_near_clip(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  ST32(base, r11.u32 + 0x14u, 0x40000000u);  // 2.0f
}

// remove_original_graphics_mode_blur @0x82188E70: CE NOOPs `bne cr6,+0x19C` so
// the blur path is never taken. Branch-replace -> always fall through.
//   jump_on_true  = 0x8218900C (original target, never taken)
//   jump_on_false = 0x82188E74 (fall through)
bool ge_ce_blur(PPCRegister& /*r3*/) { return false; }

// remove_original_graphics_mode_from_intro @0x8209972C: CE turns
// `bne cr6,0x82099750` into an unconditional `b 0x82099750` so the intro reads
// the current graphics-mode flag. Branch-replace -> always take.
//   jump_on_true  = 0x82099750 (always)
//   jump_on_false = 0x82099730 (unused)
bool ge_ce_intro_gfx(PPCRegister& /*r3*/) { return true; }

// ===========================================================================
// BeanTools Community Edition MP / network hack-functions, re-implemented as
// midasm hooks (the recomp can't add the new 0x830E guest code, so each hack's
// logic is replicated in C++ -- the same pattern as ge_hook_830E0xxx). Game
// functions are called directly via their generated sub_ symbols. 1:1 with
// finalizer.c.
// ===========================================================================
namespace { constexpr uint32_t GE_NET_FLAG = 0x830CAEA0u; }  // byte: !=0 = network MP session

// disable_doors_autoclosing_on_mp @0x820E4F1C (after `lwz r11,0xE8(r30)` loads
// the door open-tick): in a network session, force it to 0 so doors never
// auto-close. Outside a session, keep the loaded value.
void ge_ce_mp_door(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  if (base[GE_NET_FLAG] != 0) r11.u32 = 0;
}

// disable_player_collisions_for_network_mp @0x820CDFA4 (replaces `bl sub_820B3E90`,
// the player-collision-radius calc): run it normally outside a network session;
// in one, skip it so players pass through each other. The CE hack tail-returns
// from the enclosing function in both cases, so return=true.
void ge_ce_mp_collision() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (base[GE_NET_FLAG] == 0) sub_820B3E90(*ctx, base);
}

// fix_golden_gun_respawn_visiblity_flag @0x820CF940 (replaces cmpwi/bne): keep
// the respawning weapon's invisible flag only for the golden gun in the MWTGG
// scenario (so it stays hidden until grabbed); otherwise clear it so weapons
// reappear. MP scenario id @0x82F61084; weapon id in r11 (golden gun = 0x13).
//   jump_on_true  = 0x820CF948 (keep invisible: GG path)
//   jump_on_false = 0x820CF94C (clear flag: normal path)
bool ge_ce_golden_gun(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  return LD32(base, 0x82F61084u) == 3u && r11.u32 == 0x13u;
}

// make_mp_always_use_p2_fog @0x82117CB0 (after `cmpwi cr6,r3,1` @0x82117CAC; r3 =
// active player count): with 2+ players, force the fog index to 2 (P2 fog) for a
// consistent look; 1 player keeps the original path.
//   jump_on_true  = 0x82117CB8 (2+ players: continue with r3=2)
//   jump_on_false = 0x82117CB4 (1 player: original `li r3,0`)
bool ge_ce_p2_fog(PPCRegister& r3) {
  if (r3.s32 != 1) { r3.u32 = 2; return true; }
  return false;
}

// fix_network_armor_bug @0x8216BC1C (after `stw r12,0x64(r30)`): re-implements
// the CE `cal_dam` armor hack (the patch ships its C source). When an armor prop
// is processed, award it to the NEAREST player within 10m -- fixes armor not
// being granted to remote players in network MP. r30 = armor prop pointer.
// Offsets from armor_fix_code.h: prop.type@+3, prop.pos@+0x58, prop.armorval@+0x84;
// player coords ptr@+0x1AC, coord.pos@+0xC, player.armor@+0x1E8. Float consts:
// 50.0@0x82000B90, 1000.0@0x8200371C (=10m), 1e6@0x82003F0C.
void ge_ce_armor_fix(PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  const uint32_t prop = r30.u32;
  if (base[prop + 3] != 0x15) return;  // not an armor prop
  const float f50 = LDF32(base, 0x82000B90u);
  const float f1000 = LDF32(base, 0x8200371Cu);
  float nearest = LDF32(base, 0x82003F0Cu);  // 1,000,000
  const float px = LDF32(base, prop + 0x58u);
  const float py = LDF32(base, prop + 0x5Cu);
  const float pz = LDF32(base, prop + 0x60u);
  int pick = -1;
  for (int i = 0; i < 4; ++i) {
    const uint32_t pl = LD32(base, 0x82F1FA98u + i * 4u);
    if (!pl) continue;
    const uint32_t coords = LD32(base, pl + 0x1ACu);
    const float dx = LDF32(base, coords + 0x0Cu) - px;
    const float dy = (LDF32(base, coords + 0x10u) - f50) - py;
    const float dz = LDF32(base, coords + 0x14u) - pz;
    const float test = sqrtf(dx * dx + dy * dy + dz * dz);
    if (test < nearest) { nearest = test; pick = i; }
  }
  if (pick < 0 || nearest > f1000) return;  // nearest player >10m away (or none)
  const uint32_t winner = LD32(base, 0x82F1FA98u + pick * 4u);
  STF32(base, winner + 0x1E8u, LDF32(base, prop + 0x84u));  // grant armorval
}

// increase_mp_characters: bump the unlocked MP character count from 0x21 to 0x32
// (`li r11,0x21` -> `li r11,0x32`) at the two unlock sites (0x820EF350 SP-clear,
// 0x82106C54 system-link). The new character struct data is written in
// ge_ce_patches.cpp (mpchars_altsandbonus -> 0x8272BA80).
void ge_ce_mp_charcount(PPCRegister& r11) { r11.u32 = 0x32u; }

// add_sfx_to_remote_player_weapons @0x8216E25C (runs BEFORE the original
// `add r11,r10,r11`): play the firing SFX for a REMOTE player's weapon so you
// hear other players shoot online. The CE hack saved/restored every register
// around the SFX calls; we snapshot/restore the whole PPC context so the
// remote-fire (tracer-spawn) function continues undisturbed -- the SFX is a pure
// side effect. r11 = remote player struct pointer.
//   paused flag @0x830633EC; remote-fire gate player+0x2044; old sound-buffer
//   slots player+0xAFC / +0xB00; current weapon player+0x928; weapon-stats array
//   @0x82421968 stride 0x38 (model flag +0x08, stats ptr +0x0C, sound id +0x26);
//   play=sub_82144920, free=sub_82144970/sub_82144A08, set-loc=sub_821448F8;
//   solo-fullscreen screen flag @0x8272B424; player coords player+0x1AC (+0xC).
void ge_ce_remote_weapon_sfx(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  const uint32_t player = r11.u32;
  if (!player) return;
  if (LD32(base, 0x830633ECu) != 0) return;        // game paused
  if (LD32(base, player + 0x2044u) != 0) return;   // remote-fire gate

  PPCContext saved = *ctx;  // the SFX calls clobber volatile regs; restore after

  auto deactivate = [&](uint32_t slot) {
    uint32_t buf = LD32(base, slot);
    if (!buf) return;
    ctx->r3.u32 = buf; sub_82144970(*ctx, base);
    if (ctx->r3.u32 == 0) return;
    ctx->r3.u32 = LD32(base, slot); sub_82144A08(*ctx, base);
  };
  deactivate(player + 0x0AFCu);   // free old sound buffer 1
  deactivate(player + 0x0B00u);   // free old sound buffer 2

  const uint32_t snd_slot = player + 0x0B00u;  // play into buffer-2 slot
  const uint32_t channel  = 0x1461u;

  const uint32_t weapon = LD32(base, player + 0x928u);
  const uint32_t entry  = 0x82421968u + weapon * 0x38u;  // weapon stats entry
  if (LD32(base, entry + 0x08u) != 0) { *ctx = saved; return; }  // no model -> no sfx
  const uint32_t stats = LD32(base, entry + 0x0Cu);
  if (stats == 0) { *ctx = saved; return; }                     // null stats
  const uint32_t sound_id = LD16(base, stats + 0x26u);          // weapon sound id
  if ((int32_t)sound_id > 0x105) { *ctx = saved; return; }      // illegal range

  ctx->r3.u32 = LD32(base, 0x83064DE0u);
  ctx->r4.u32 = sound_id;
  ctx->r5.u32 = snd_slot;
  ctx->r6.u32 = LD32(base, 0x83064DE8u);
  ctx->r7.u32 = 0x820036A8u;
  ctx->r8.u32 = channel;
  sub_82144920(*ctx, base);          // play sfx -> r3 = sound buffer
  const uint32_t buf = ctx->r3.u32;
  if (buf != 0 && LD32(base, 0x8272B424u) == 3u) {  // solo full-screen -> 3D pos
    const uint32_t coord = LD32(base, player + 0x01ACu);
    ctx->r3.u32 = buf;
    ctx->r4.u32 = coord + 0x0Cu;
    sub_821448F8(*ctx, base);        // set 3D location
  }

  *ctx = saved;  // restore -> remote-fire function continues unaffected
}

// set_mp_sfx_to_use_player_location: the 4 SFX call sites (gasp 0x820BF264,
// slapper 0x820CDC5C, knife 0x820ACF54, item-equip 0x820AC4D0) all originally
// `bl sub_82144920` (play sfx). CE redirects each through a helper that plays the
// sound AND positions it at the emitting player's 3D location, so in
// split-screen-solo/online you hear other players' actions directionally. This
// hook IS that helper: it plays the sfx (args already in ctx from the caller),
// then sets the 3D location -- but only when another player is the source (not
// the local/active-viewport player, whose own sounds stay centered). Registered
// at all 4 sites with jump_address = site+4 to replace the original bl. No reg
// save needed: the original was itself a bl, so volatiles are already clobbered.
// Shared helper: play the sfx (args already in ctx) and 3D-position it at the
// emitting player -- but only when the source is a NON-local player (the local/
// active-viewport player's own sounds stay centered).
static void ge_ce_play_at_location(PPCContext* ctx, uint8_t* base) {
  sub_82144920(*ctx, base);                    // play sfx (caller's args in ctx)
  const uint32_t buf = ctx->r3.u32;            // sound buffer handle
  if (buf == 0) return;                        // null buffer -> done
  if (LD32(base, 0x8272B424u) != 3u) return;   // not solo full-screen view
  if (LD32(base, 0x82F1FA9Cu) == 0 && LD64(base, 0x82F1FAA0u) == 0)
    return;                                    // single-player -> no positioning
  const uint32_t cur = LD32(base, 0x82F1FAACu);  // current player
  if (LD32(base, cur + 0x904u) == 0) return;   // local active viewport -> centered
  const uint32_t coord = LD32(base, cur + 0x1ACu);
  ctx->r3.u32 = buf;
  ctx->r4.u32 = coord + 0x0Cu;                 // -> player world location
  sub_821448F8(*ctx, base);                    // set 3D location
  ctx->r3.u32 = buf;                           // leave buffer in r3 for downstream
}

void ge_ce_sfx_3d() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  ST32(base, 0x824203ACu, 0);  // your own sound -> your death -> death tune plays
  ge_ce_play_at_location(ctx, base);
}

// Trigger Gasps If Local Player Damaged, Else Argh (set_mp_sfx, gasp half) @
// 0x820BF408 (replaces `bl sub_82144920`). When the damaged player is a REMOTE
// player (solo-fullscreen + MP + not the local viewport + has a model), play a
// gender-appropriate "argh" at their 3D location and flag this death as NOT
// yours (0x824203AC=1) so the death tune is suppressed for it; otherwise it's
// your own gasp (flag=0 -> death tune plays). jump_address skips the original bl.
//   chr = player+0x1AC, model = chr+0x08, bodynum = model+0x0F; body-info array
//   0x82729020 stride 0x24, gender +0x18; argh index female 0x83062BF4 (0..2,
//   +0x0D) / male 0x83062BF8 (0..0x18, +0x86).
void ge_ce_gasp() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  uint32_t model = 0;
  if (LD32(base, 0x8272B424u) == 3u) {                       // solo full-screen
    const bool mp = (LD32(base, 0x82F1FA9Cu) != 0) || (LD64(base, 0x82F1FAA0u) != 0);
    if (mp) {
      const uint32_t cur = LD32(base, 0x82F1FAACu);
      if (LD32(base, cur + 0x904u) != 0) {                   // not the local viewport
        const uint32_t chr = LD32(base, cur + 0x1ACu);
        if (chr) model = LD32(base, chr + 0x08u);
      }
    }
  }
  if (model != 0) {                                          // remote player damaged
    ST32(base, 0x824203ACu, 1u);                             // not your death
    const uint32_t bodynum = base[model + 0x0Fu];
    const uint32_t gender = base[0x82729020u + bodynum * 0x24u + 0x18u];
    ctx->r5.u32 = 0;
    uint32_t arghid;
    if (gender == 0u) {                                      // female
      int32_t i = (int32_t)LD32(base, 0x83062BF4u) + 1;
      if (i > 2) i = 0;
      ST32(base, 0x83062BF4u, (uint32_t)i);
      arghid = (uint32_t)i + 0x0Du;
    } else {                                                 // male
      int32_t i = (int32_t)LD32(base, 0x83062BF8u) + 1;
      if (i > 0x18) i = 0;
      ST32(base, 0x83062BF8u, (uint32_t)i);
      arghid = (uint32_t)i + 0x86u;
    }
    ctx->r4.u32 = arghid;
    ge_ce_play_at_location(ctx, base);                       // argh at their location
  } else {                                                   // your own gasp
    ST32(base, 0x824203ACu, 0u);                             // your death -> death tune plays
    sub_82144920(*ctx, base);                                // play gasp (caller's args)
  }
}

// only_trigger_mp_death_tune_for_your_kills_and_yourself @0x820BFB04 (the
// `bl <play death tune>`; r3 already = 6 from the vanilla `li r3,6` at 0x820BFB00).
// Skip the death tune when 0x824203AC is set (the gasp hook flagged this death as
// another player's). jump_on_true skips the bl; no jump_on_false -> falls through
// and plays it for your own deaths.
bool ge_ce_death_tune() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  return LD32(base, 0x824203ACu) != 0u;
}

// reset_internal_cheat_state float relocation @0x8209D88C: the death-tune logic
// reads a 5.0f that originally lived at the address CE now repurposes as the
// bypass flag (0x824203AC). After the original `lfs f1,0x3AC(r11)`, reload f1
// from +0x3A8 instead (where ge_ce_patches stashes the 5.0f). r11 = 0x82420000.
void ge_ce_killtune_float(PPCRegister& r11, PPCRegister& f1) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  f1.f64 = (double)LDF32(base, r11.u32 + 0x3A8u);
}

// fix_watch_volume_sliders_range: the watch volume sliders only spanned half the
// real 0-100 range. CE reads the stored byte and halves it for display, and
// doubles the slider value before storing (entering the save routine past its
// clamp). READ hooks replace `bl <vol read>` (r3 = settings ptr -> vol byte >> 1);
// SAVE hooks replace `bl <vol save>` (double r4, then run the save routine from
// its mid-point continuation so the doubled value isn't clamped back). Music vol
// byte = settings+0x295, fx vol = settings+0x294. All use jump_address = site+4.
void ge_ce_watch_music_read(PPCRegister& r3) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  r3.u32 = (uint32_t)(base[r3.u32 + 0x295u] >> 1);
}
void ge_ce_watch_sfx_read(PPCRegister& r3) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  r3.u32 = (uint32_t)(base[r3.u32 + 0x294u] >> 1);
}
void ge_ce_watch_music_save() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  ctx->r4.u32 = ctx->r4.u32 + ctx->r4.u32;   // double the slider value
  ge_cont_82184E18(*ctx, base);              // save routine past its clamp
}
void ge_ce_watch_sfx_save() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  ctx->r4.u32 = ctx->r4.u32 + ctx->r4.u32;
  ge_cont_82184E48(*ctx, base);
}
