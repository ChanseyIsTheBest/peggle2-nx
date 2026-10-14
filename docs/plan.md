# Plan

The order is the one that worked for nfsmw-nx, masseffect-nx and sa2-nx: every problem of the translation is far
easier to find on a PC than on the console, and ReXGlue's emulated GPU is the right way to see the first frames, but
far too slow on the Switch. One stage at a time, each verified before the next.

## The game

| | |
|---|---|
| Package | XBLA LIVE (STFS, read-only), content type `0x000D0000`, title `58411436` ("Peggle 2"), 225,176 blocks |
| `default.xex` | 3,969,024 bytes, XEX2, original PE name `PeggleNext360.exe`, version 4 (base 4) |
| Image | base `0x82000000`, size `0x00DF0000` (14,614,528 bytes), entry `0x823186D0`, default stack 256 KB |
| Imports | `xam.xex`, `xboxkrnl.exe` only |
| Static libraries | XDK 2.0.21256 (D3D9, D3DX9, XGRAPHC, XAUDIO2, XAPOBA, XMCORE, XMP, XHV2, XNET, XONLINE, XPARTY, XAPILIB, XBOXKRNL), MSVC 16.0.11886 (C1/C2/LIBCPMT), LINK 10.0.11886 |
| Data | `main.rsb` (917,962,752 bytes, PopCap's resource bundle), `ArcadeInfo.xml` (28 achievements, one leaderboard), 31 PNGs (achievement and marketplace images) |

Found with `tools/stfs_extract.py` (34 entries, all blocks hash-checked) and the XEX optional headers (2026-10-05).
The package holds no `*.xex` other than `default.xex` and no overlay files, so there is no `[[modules]]` entry.
`main.rsb` is data; whether the game's code mentions `XexLoadImage` is checked in the generated code
(see [peggle2-notes.md](peggle2-notes.md)).

Peggle 2 on the 360 is the last XDK (21256, 2014): the D3D device layout is newer than the 2008 (Banjo-Kazooie N&B)
and 2012 (SA2) ones the renderer knows, so the device's bound VS/PS offsets must be measured again in stage 4.

## Stage 2: code generation

1. `rexglue codegen app/peggle2_manifest.toml` -> `app/generated/default/`.
2. Each "target not in any function" -> `[functions]` entry in `app/overrides.toml` with its caller(s).
3. Missed-code sweep before booting, re-run after every codegen:
   - functions reached only by pointer: `tools/pointer_fixpoint.py`, `tools/pointer_audit.py`
   - truncated jump tables: `tools/switch_check.py`
   - `setjmp`/`longjmp`: by shape, `setjmp_address` / `longjmp_address`

## Stage 3: boot on PC with the emulated GPU

`masseffect_renderer_native` off (the renderer is not even built: `PEGGLE2_NATIVE_RENDERER=OFF`). Boot with
`tools/win/boot_test.ps1`, triage crashes (unregistered call -> pointer/gap class; guest access violation ->
symbolize `peggle2.exe+0xRVA`), `license_mask = 1`. Goal: title, menus, a level of the adventure, a trial, a duel.

## Stage 4: native renderer on PC

Same design as sa2-nx (`app/src/me`): find the game's D3D functions with `tools/d3d_match.py`, hook the draw-time
shader load, Draw*, Create*Shader and the flip, build the shader library until 0 unidentified loads, then compare
native against emulated frame by frame until nothing differs.

## Stage 5: GPU optimizations, then the Switch

devkitA64 + libnx, NVK from mesa-switch, the SDK's Horizon layer; read nfsmw-nx `docs/platform-notes.md`,
`toolchain.md`, `measuring.md` first. Every optimization pixel-identical (`compare_video.py`) and measured on the
console.

## After the console crash: the CPU work on the console

Order agreed with the other ports (Mushihimesama session, 2026-10-06), each item with its own runtime key (build-time ones go into the one optimised NRO: no A/B NRO pairs) and
checked with `app/configs/peggle2_cpu_verify.toml` before it is measured on the console (CPU at 1020 MHz, game-thread
ms per frame, `peggle2_cpu_on` against `peggle2_cpu_off`):

1. Console profile first: `logs/rex/stacks_profile.flag`, `tools/switch_hot_functions.py` with `out/nx/peggle2.map`.
2. Codegen options in the optimised build (masseffect-nx's options, futari-nx's implementation): `cr/xer/ctr/reserved_as_local`, `skip_msr`,
   `elide_lr_stores` (+ `lr_keep_returns` for every return address a hook compares), `dform_disp_split`,
   `fctiwz_inline`, `share_registers` on funclets/split fragments (futari-nx `tools/scan_caller_register_reads.py`
   finds them; ours so far: `0x8231CEE4`), then direct calls + guest memory without volatile (futari-nx
   `tools/perf_codegen_post.py`, `codegen.ps1 -Perf`). Not before the heap diagnosis is finished: `elide_lr_stores`
   leaves `ctx.lr` unset at calls, and the heap tracker's caller addresses come from it.
3. Handheld GPU boost, Mesa disk cache / pipeline cache on misses.
4. Kernel sync, TLS, FPCR, relaxed `stwcx.` last, gated by an audio-heavy console test.
5. Native versions of hot game functions, batch by batch (sa2-nx's method, relayed 2026-10-06): rank ~30 functions
   by busy self time (console profile once it exists: `switch_profile_stacks`; until then the PC profile), write
   natives (C runtime and leaf routines by hand; the rest as "localized copies" of the recompiled C++ with the guest
   registers in locals, synced back to ctx before every call and return), each with its own key, a code hash of its
   guest bytes and use counters; never write a register the original leaves alone (MSVC's interprocedural register
   allocation: callers read r4-r10 after calls known not to touch them); verify on the PC (guarded comparison for
   leaf functions, every call checked in verify mode, a lockstep capture compared frame by frame), then A/B on the
   console with the profiler on both sides. First PC profile (notes, "Enhancements"): the main thread is flat (no
   function above 4 %), Wwise's `sub_825BE6A0` 8.6 % of its thread.
6. The direct renderer (the user's orders relayed by the SA2 session, 2026-10-07; notes, "The direct renderer"):
   step 1 (`peggle2_cpu_direct_state`) and step 1b with sa2-nx's ring rounds 2-5 are in and PC-verified; deferred
   recording stays explicit-only (not pixel-identical in futari-nx's lockstep). On the console: the ring timer's CPU
   figure (`[native] ring thread: ... CPU`) with `peggle2_cpu_on` against `peggle2_cpu_off`, and the profile, decide
   whether steps 2/3 (native flush from the device shadow, identity from bound objects, coherency from the API, EDRAM
   ownership from SetRenderTarget / Resolve) are worth it for Peggle 2's ~33 draws a frame.
7. The 50-60 % CPU goal (the user's, 2026-10-07; judged on ARM, notes "Less CPU everywhere"). Done on the PC and in the
   Switch ELF: post-codegen passes (direct calls, plain guest memory, loop barriers), diet codegen (reserved_as_local,
   skip_msr, dform_disp_split, fctiwz_inline), smart sync in the copies, Switch LTO + -fipa-pta: recompiled code -12.7 %
   instructions before LTO. Waiting for the console: `elide_lr_stores` (+ lr_keep for the heap tracker's call sites, or
   after the heap diagnosis), `non_argument_as_local` (needs a share_registers scan), args_in_registers, hot-function
   ordering from a console profile, `-fno-math-errno -fno-trapping-math` (needs a quiet-PC lockstep), and the console
   profile itself to pick the next copies (main thread ~62 % and Wwise ~23 % of the PC CPU).
8. Log writer thread (futari-nx's finding, 2026-10-07): on the console the 10 s report bursts the ring thread writes
   through spdlog's rotating file sink became 250-1,022 ms frames (newlib's 1 KB FILE buffer: one SD write per long
   line, under the sink mutex, competing with BGM streaming). Peggle's SDK has the same sink (sdk/src/core/logging.cpp).
   Port futari-nx's WriterFileSink (log_file_writer, log_file_writer_buffer_kb; RexLogUnwrittenTail in
   exception_handler_switch.cpp) once its console run (futari-nx build N_logwriter) confirms it. The tail-loss worry
   is mostly covered (futari-nx, 2026-10-07): abort() and unexpected exit() go through RexSwitchCrashLog
   (sdk/src/ui/switch_crash_hooks.c there), which appends the unwritten tail to rex_crash.log as for CPU exceptions;
   the writer thread writes and flushes at least once a second (a hang loses <= ~1 s, as the old 1 KB buffer did);
   and `log_file_writer = false` in the toml gives back the old synchronous sink for crash hunting. Check that
   Peggle's SDK has the same crash hooks before porting.
