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

#include <atomic>
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

// Tunables (sens is the user-facing multiplier; these set the base feel).
namespace {
// Direct 1:1 look: mouse delta -> absolute yaw/pitch, written straight into the
// camera state every frame (tune via sens).
constexpr float kYawDegPerCount = 0.10f;       // guest yaw DEGREES per mouse count @ sens 1
constexpr float kPitchPerCount = 0.10f;      // guest pitch (+612) units per mouse count @ sens 1
constexpr float kPitchMin = -60.0f;
constexpr float kPitchMax = 60.0f;
constexpr uint32_t GE_BONDVIEW_CUR = 0x82F1FAACu; // dword_82F1FAAC -> current bondview struct
constexpr uint32_t GE_BV_YAW = 596u;              // +596 facing yaw (degrees, HUD/radar copy)
constexpr uint32_t GE_BV_PITCH = 612u;            // +612 pitch angle
constexpr uint32_t GE_BV_CENTER_FLAG = 524u;      // +524 auto-center-this-frame flag
// Camera yaw is built from the heading -- drive it + its smoother steady-state.
constexpr uint32_t GE_YAW_HEADING = 0x82F1F900u;  // flt_82F1F900 heading (rad)
constexpr uint32_t GE_YAW_SMOOTH = 0x82F1F904u;   // flt_82F1F904 smoothed (= heading*12.5)
constexpr uint32_t GE_YAW_TARGET = 0x82F1F910u;   // flt_82F1F910 target heading (rad)
constexpr uint32_t GE_YAW_OFFSET = 0x82F1F8F0u;   // flt_82F1F8F0 yaw offset added to heading
constexpr float GE_TWO_PI = 6.2831855f;
constexpr float GE_RAD2DEG = 57.29578f;
}  // namespace

namespace {
float g_look_yaw_deg = 0.0f;  // our absolute yaw (deg, [0,360))
float g_look_pitch = 0.0f;    // our absolute pitch (+612 units)
uint32_t g_look_bv = 0;       // bondview struct we're tracking
}  // namespace

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
  }
  ge_update_mouse_capture();  // release the cursor immediately when the menu opens
}
}  // namespace ge

// YAW: runs right after the guest stores the per-frame yaw delta (radians) to
// flt_82F1F914 in ge_bondview_control. Add the mouse's horizontal delta so it
// flows through the same heading integrator the stick uses -> 1:1 mouse yaw,
// alongside the pad. Only reached in look-mode, so it self-gates to gameplay.
// This site (the stick-yaw write) only runs in control mode 2, which normal
// gameplay does NOT use -- so mouse YAW is applied in ge_mouselook_pitch instead
// (that site runs every frame). Kept only to start the thread / confirm firing;
// it must NOT consume the mouse dx, or it would steal it from the pitch hook in
// the rare mode-2 scenes.
void ge_mouselook_yaw(PPCRegister& /*r11*/) {
  ge_start_mouse_once();
}

// PITCH: runs right after the guest integrates pitch into bondview(+612). Add
// the mouse's vertical delta (mouse-up = look up), clamp, and clear the
// auto-center flag (+524) for this frame so the view HOLDS instead of springing
// back to level when the stick isn't being pushed.
// Direct 1:1 look. Runs every frame in ge_bondview_control (unconditional pitch
// site). Keep our own absolute yaw/pitch from raw mouse deltas and write them
// straight into the camera state: +596 (HUD yaw), +612 (pitch), and the heading
// (flt_82F1F900/904/910) the view matrix is built from. Re-sync to the game's
// values when the active view changes.
void ge_mouselook_pitch(PPCRegister& /*r11*/) {
  ge_start_mouse_once();
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t bv = LD32(base, GE_BONDVIEW_CUR);
  if (!bv) return;

  if (!ge_mouselook_on()) return;

  float game_yaw = LDF32(base, bv + GE_BV_YAW);
  float game_pitch = LDF32(base, bv + GE_BV_PITCH);
  if (bv != g_look_bv) {  // new view -> adopt the game's current angles
    g_look_bv = bv;
    g_look_yaw_deg = game_yaw;
    g_look_pitch = game_pitch;
  }

  const float sens = static_cast<float>(REXCVAR_GET(ge_mouse_sens));
  int dx = ge_take_mouse_dx();
  int dy = ge_take_mouse_dy();

  g_look_yaw_deg += static_cast<float>(dx) * sens * kYawDegPerCount;   // right = +yaw
  while (g_look_yaw_deg >= 360.0f) g_look_yaw_deg -= 360.0f;
  while (g_look_yaw_deg < 0.0f) g_look_yaw_deg += 360.0f;

  g_look_pitch += static_cast<float>(-dy) * sens * kPitchPerCount;     // up = look up
  if (g_look_pitch > kPitchMax) g_look_pitch = kPitchMax;
  if (g_look_pitch < kPitchMin) g_look_pitch = kPitchMin;

  STF32(base, bv + GE_BV_YAW, g_look_yaw_deg);   // HUD/radar copy
  STF32(base, bv + GE_BV_PITCH, g_look_pitch);
  ST32(base, bv + GE_BV_CENTER_FLAG, 0u);  // suppress vertical auto-center

  // YAW: drive the heading the camera is built from (+ its smoother steady-state).
  float offset = LDF32(base, GE_YAW_OFFSET);
  float h = g_look_yaw_deg / GE_RAD2DEG - offset;
  while (h >= GE_TWO_PI) h -= GE_TWO_PI;
  while (h < 0.0f) h += GE_TWO_PI;
  STF32(base, GE_YAW_HEADING, h);
  STF32(base, GE_YAW_SMOOTH, h * 12.5f);
  STF32(base, GE_YAW_TARGET, h);
}

// Runs in ge_bondview_tick right after the camera pitch flt_82F1F8F8 is finalized
// (= v106 * 0.06, the only write). That is AFTER the game's narrow clamp and the
// walking auto-level (v102==32 swaps the pitch to a leveled value), so forcing
// our pitch here gives the full mouse range and NO auto-level.
void ge_tick_pitch(PPCRegister& /*r11*/) {
  // No-op (kept hooked, harmless). Pitch is driven via +612 in ge_mouselook_pitch.
}

// THE pitch auto-level. ge_bondview_tick @0x820bcb30 eases +612 toward the neutral
// target flt_82F1F8B0 (~-4) by blend flt_82F1F444 -> when walking, that blend
// ramps to 1 and pulls the view back to level. When mouse-look is on, restore our
// pitch right after this write so it HOLDS where the mouse put it.
void ge_pitch_hold(PPCRegister& /*r11*/) {
  if (!ge_mouselook_on() || !g_look_bv) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t bv = LD32(base, GE_BONDVIEW_CUR);
  if (bv) STF32(base, bv + GE_BV_PITCH, g_look_pitch);
}

// GE move-to-centre: ge_bondview_control @0x820bb460 sets field_104 (+524) = 1
// when you move without pushing the look stick; the very next block then eases the
// pitch back to neutral ("auto look ahead"), and it stays on until the look stick
// is touched. Clear +524 right here -- before that ease reads it -- so mouse-look
// pitch never auto-levels. Gated on mouse-look (off = native behaviour returns).
void ge_no_autolevel(PPCRegister& /*r11*/) {
  if (!ge_mouselook_on()) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t bv = LD32(base, GE_BONDVIEW_CUR);
  if (bv) ST32(base, bv + GE_BV_CENTER_FLAG, 0u);
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

// Is the key currently bound to cvar `name` held down? Reads the bind by name,
// parses it to a virtual key (== Windows VK code), and polls the async state.
bool ge_key_down(const char* name) {
  std::string keyname = rex::cvar::GetFlagByName(name);
  if (keyname.empty()) return false;
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(keyname);
  if (vk == rex::ui::VirtualKey::kNone) return false;
  return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
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

// Runs once per controller poll, after XamInputGetState fills the slot-0 buffer
// and before the guest dispatches it. OR our keyboard buttons in, and set the
// left stick / triggers when their keys are held (pad input is preserved).
void ge_inject_keyboard(PPCRegister& /*r11*/) {
  if (!REXCVAR_GET(ge_keyboard_enable) || !ge_input_active()) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;

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
}
