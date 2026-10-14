# Building peggle2-nx

Everything below runs on Windows. You need your own copies of the Xbox Live Arcade game and of Peggle 2 Magical
Masters Edition for PS4; nothing made from them is committed (`assets/`, `app/generated/` and `out/` are
git-ignored).

## Tools

- VS 2022 Build Tools (MSVC libraries + Windows SDK), LLVM clang 20 (portable is fine; by default `..\_tools\llvm`),
  Python 3 with `numpy`, `Pillow`, `lz4` and `xxhash`, Git. CMake and Ninja come from the VS Build Tools. Paths can be
  overridden, see `tools/win/env.ps1`.
- For the Switch: devkitPro (devkitA64, libnx) and the Mesa Horizon SDK with the static NVK driver (by default the
  `mesa-switch-main-nfsmw-source` install under `C:/devkitPro`; `-NvkSdk` overrides it).

## 1. The game and the PC build

```powershell
# Extract your package (verifies the STFS hash tables, writes nothing to it; ~880 MB)
python tools\stfs_extract.py "<...>\58411436\000D0000\2452FB99DD4F3E052C919B780FEAE8F8E8281CF058" assets\game_root

# Third-party sources of the SDK (only the files this fork changed are kept in sdk/thirdparty)
python tools\fetch_thirdparty.py

# The code generator (-march=x86-64-v2 is required: the SDK uses SSSE3 intrinsics)
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"

# Translate the game to C++ (and the post-codegen steps: python -P tools/post_codegen.py)
powershell -File tools\codegen.ps1

# The PC build
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app"
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out/pc"

# A 45 s run (muted, minimized) and the end of its log
powershell -File tools\win\boot_test.ps1
```

The native renderer needs the shader library made from your own files: run once with `peggle2_dump_shaders = true`
(or `REX_PEGGLE2_DUMP_SHADERS=true`), then `bash tools/build_shader_library.sh out/pc/shaders ...` (see
`docs/peggle2-notes.md`, "Stage 4", for the variant and wrapper folders), and copy `out/shaders/peggle2_shaders.mesp*`
next to the executable.

`out\pc\peggle2.toml` holds the PC build's settings. It starts **muted and minimized** by default (`audio_mute`,
`window_start_minimized`), so test runs stay out of the way; set both to `false` to play. Test runs drive a scripted
pad (`-InputScript`, see `app/src/peggle2_input_script.h`) and can record video (`-VideoAt`, `-VideoFrames`).

## 2. The PS4 content

Extract the PS4 base package's `game_data/main.rsb` (with the port's PFS reader, `out/ps4_dlc/scripts/ps4pfs.py`, or
any tool of your choice), then build the Xbox 360 archive with the PS4 content:

```powershell
python -P -E tools\ps4_dlc\build_mod.py --x360 assets\game_root\main.rsb --ps4 <PS4 main.rsb> `
  --out out\ps4_dlc\game_root_one\main.rsb `
  --content costumes,jimmy,windy,levels,strings,multiplayer,select,videos,sound,music
```

The other game files are copied next to it by `tools\win\mod_test.ps1` at its first run (or copy them from
`assets\game_root`). What each content step takes from the PS4 archive and why is in `docs/peggle2-notes.md`, "PS4
content in the Xbox 360 build" and the sections after it.

## 3. The 1080p art

```powershell
python -P -E tools\hd\build_hd_pack.py --x360 out\ps4_dlc\game_root_one\main.rsb --ps4 <PS4 main.rsb> --out out\hd\pack --jobs 5
```

About 70 minutes on 16 threads. The pack is keyed by the content of each 720p atlas: after a change to the archive,
`python -P -E tools\hd\merge_packs.py --x360 <main.rsb> <old pack folder>` lists the groups whose atlases changed;
build only those (`--groups A,B --out <partial>`) and merge (`merge_packs.py --x360 <main.rsb> --out <new> <old> <partial>`).

## 4. The NRO

```powershell
powershell -File tools\win\build_switch.ps1 -Jobs 8                                    # out\nx\peggle2.nro
powershell -File tools\win\build_switch.ps1 -Jobs 8 -Define "PEGGLE2_SWITCH_LTO=OFF"   # a quicker development build
```

The Switch build's settings are compiled in: `app/peggle2_switch_defaults.toml` (every enhancement on, the PS4
content on, the 1080p art always, no log) is applied before the SD card's `peggle2.toml`, and
`app/peggle2_switch.toml` (frame rate, language, controllers) is written as `peggle2.toml` at the first start when
there is none. Both are written by `python tools\make_enhancement_configs.py` from the enhancement table; the test
configs it writes to `app/configs/` (logging, the profiler, A/B switches) still work as `peggle2.toml`.

`python tools\check_switch_build.py` checks a build before it goes to the console (start-up code, NVK driver, the
compiler options the console needs).

### Profile-guided optimisation

1. `build_switch.ps1 -Pgo generate` builds an instrumented NRO. Play it on the console: it writes
   `pgo/peggle2_pgo_<N>.stream` next to the NRO (one file per session, saved every 2 minutes and on the HOME button).
2. Copy the console's `pgo` folder to the PC and run `powershell -File tools\win\pgo_merge.ps1 <folder>`. It merges
   every session into `pgo\` and fills in each file's summary. Merge on the PC that built the instrumented NRO: the
   streams name its profile folder.
3. `build_switch.ps1 -Pgo use` builds the optimised NRO. `-Pgo none` goes back to a build without PGO.

Do not change the game or app code between steps 1 and 3: a changed function loses its profile.

## 5. The SD folder

```powershell
python tools\build_asset_pack.py out\ps4_dlc\game_root_one --out-dir out\ps4_dlc\pack_one --verify
powershell -File tools\win\package_switch.ps1 -Pack out\ps4_dlc\pack_one\game_root_one.nxpack -MovePack -HdPack out\hd\pack\peggle2_hd.pack
```

`out\dist\switch\peggle2-nx\` then holds the NRO, the game pack (`game_root.nxpack` + `.nxidx`, one file the console
keeps open instead of the game folder), the 1080p art pack, the shader library and the test configs. Copy it to
`sdmc:/switch/`.
