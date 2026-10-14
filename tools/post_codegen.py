#!/usr/bin/env python3
"""The post-codegen steps of the optimised build, in one place (from sa2-nx's tool of the same name, 2026-10-13;
docs/peggle2-notes.md, "sa2-nx's three CPU rounds"). It replaces tools/perf_codegen_post.py (futari-nx's three passes
in one file): sa2-nx's later steps are written against the text its own passes produce.

Run after EVERY codegen, by tools/codegen.ps1 and by the build itself: app/CMakeLists.txt appends this script to the
generated peggle2_codegen command, because the PC build re-runs the generator whenever an input changes (the
manifest, a toml, the xex, the generator binary) and that would otherwise drop the steps silently.

  base (as before):
      tools/pch_no_volatile.py  guest loads and stores without volatile            } required
      tools/loop_barriers.py    a compiler barrier at the loop headers that need it } together
      tools/direct_calls.py     calls between unhooked functions become direct (inlinable); --redo, so a function
                                hooked since the last run is called through its hook again
  round 1 (sasrt-nx's, through sa2-nx):
      tools/guest_cas.py        compare-and-swap without the legacy builtin's trailing `dmb ish` on AArch64
      tools/flush_mode_flow.py  the flush-mode checks the control flow and the callees' summaries fix (nothing on the
                                Switch; still tested in the PC build), with the entry modes from the image
      tools/vec_byteswap.py     the 16-byte reversal of guest vector loads and stores without a table lookup
      tools/pch_neon_shuffles.py constant word shuffles as one permutation on AArch64
  round 2:
      tools/indirect_calls.py   every `bctrl` tagged with its site (PC census), and the sites of
                                app/indirect_promotions.txt as a guarded direct call. Before loop_barriers.py (a
                                promoted call is no longer an indirect call) and before direct_calls.py.
      tools/pch_cold_traps.py   the pch's trap helper out of line and cold
      tools/ro_constants.py     loads from the image's read-only data as constants. Before float_loads.py.
      tools/float_loads.py      a single-precision load straight into a float register
      tools/leaf_locals.py      the ctx registers of leaf functions with loops as locals. Last: it renames registers.
  round 3:
      tools/loop_locals.py      the ctx registers a call-free loop uses as locals for the length of the loop. After
                                loop_barriers.py, before the localized copies and leaf_locals.py.
  not run: sa2-nx's tools/frame_traffic.py (its three stack-frame elisions are off there: not exact).

ro_constants.py and the entry modes of flush_mode_flow.py need the generator's image dump (out/default_image.bin,
written by tools/codegen.ps1 through REXGLUE_DUMP_IMAGE).

Then the localized native copies (tools/localize_native.py --list app/src/cpu/hot/localized.txt), made from this
text, and last leaf_locals.py.

Every step is idempotent and the whole sequence is too (sa2-nx checked it by hash on a copy of the tree).

  python tools/post_codegen.py [--gen <generated folder>]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
IMAGE = os.path.join(ROOT, 'out', 'default_image.bin')
PROMOTIONS = os.path.join(APP, 'indirect_promotions.txt')
PCH = 'peggle2_pch.h'


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen: {" ".join(os.path.basename(a) for a in args[:1])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    scratch = gen != GEN  # a copy for measurements: no localized copies
    tree = os.path.join(gen, 'default')
    pch = os.path.join(tree, PCH)
    if not os.path.isfile(IMAGE):
        sys.exit('post_codegen: out/default_image.bin is missing (the image dump of the code generator): run '
                 'tools\\codegen.ps1 once, it writes it (REXGLUE_DUMP_IMAGE)')
    xex = os.path.join(ROOT, 'assets', 'game_root', 'default.xex')
    if os.path.isfile(xex) and os.path.getmtime(xex) > os.path.getmtime(IMAGE):
        sys.exit('post_codegen: out/default_image.bin is older than assets/game_root/default.xex: run '
                 'tools\\codegen.ps1 (it writes the dump again)')
    os.environ.setdefault('REXSDK_DIR', os.path.join(ROOT, 'sdk'))
    run(tool('pch_no_volatile.py'), pch)
    # Before direct_calls.py (a promoted call is a direct call like any other) and BEFORE loop_barriers.py: a loop
    # needs no barrier when every way round it passes an indirect call, and a promoted call is no longer one.
    run(tool('indirect_calls.py'), '--gen', tree, '--promote', PROMOTIONS)
    # REQUIRED with pch_no_volatile.py: without barriers GCC turns guest polling loops into infinite loops.
    run(tool('loop_barriers.py'), tree)
    run(tool('pch_cold_traps.py'), pch)
    run(tool('direct_calls.py'), '--redo', '--gen', tree)
    # After direct_calls.py: the flush-mode proofs depend on which callees are called directly.
    run(tool('guest_cas.py'), '--gen', tree)
    run(tool('flush_mode_flow.py'), '--redo', '--gen', tree, '--image', IMAGE)
    run(tool('vec_byteswap.py'), '--gen', tree)
    run(tool('pch_neon_shuffles.py'), pch)
    # ro_constants before float_loads (it folds the integer form of a float load).
    # --xex: the words of the XEX's import records are written by the loader (kernel variables' addresses): loads
    # of them stay loads.
    run(tool('ro_constants.py'), '--gen', tree, '--image', IMAGE, *(['--xex', xex] if os.path.isfile(xex) else []))
    run(tool('float_loads.py'), '--gen', tree)
    # Before the localized copies, which are made from this text and rename its ctx registers once more.
    run(tool('loop_locals.py'), '--gen', tree)
    if not scratch:
        # Before leaf_locals.py in a fresh tree; after it when this script runs again (the tool reads both forms).
        run(tool('localize_native.py'), '--list', os.path.join(APP, 'src', 'cpu', 'hot', 'localized.txt'))
    run(tool('leaf_locals.py'), '--gen', tree)
    # Not an optimisation: the multiplayer lobby's master loop asks the port whether to go past five masters
    # (app/src/dlc/peggle2_dlc_lobby.cpp). Last: it matches the text the steps above leave.
    run(tool('mp_masters.py'), '--gen', tree)
    # Not an optimisation either: the game's frame step asks the port how many logic updates the frame gets
    # (peggle2_fps, app/src/peggle2_frame_hooks.cpp).
    run(tool('fps_updates.py'), '--gen', tree)
    # Nor this: the Select a Master picture gets states for the PS4's six costumes (peggle2_dlc_master_select,
    # app/src/dlc/peggle2_dlc_master_select.cpp).
    run(tool('costume_states.py'), '--gen', tree)
    print(f'post_codegen: optimised codegen steps done{" on " + gen if scratch else ""}')


if __name__ == '__main__':
    main()
