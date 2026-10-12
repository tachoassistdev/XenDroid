# Magna Carta 2 on the AYN Thor Pro — optimisation notes

**Title:** Magna Carta 2 — `4E4D080B` (Unreal Engine 3)
**Device:** AYN Thor Pro — Snapdragon 8 Gen 2 (1× X3, 2× A715, 2× A710, 3× A510), Adreno 740, 12 GB
**Driver in the reference config:** `Turnip_Gen8_V34`, `turnip_debug = "sysmem"`
**Starting point (player-reported):** 16–30 fps, correct rendering with the
settings listed in §3.

> **No FPS change is claimed here.** Nothing in this document was measured
> on the device. Every change below is either accounting/diagnostics or a
> removal of host overhead identified by reading the code, and it needs the
> on-device procedure in §6 to say whether it moved the frame rate.

---

## 1. What XenDroid uses from Xenia

| Part | Where | Used on Android |
|---|---|---|
| Emulator core, kernel HLE (xboxkrnl/xam), VFS | `xenia/src/xenia/{kernel,vfs}`, `emulator.cc` | yes |
| PowerPC frontend + HIR passes | `cpu/ppc`, `cpu/compiler/passes` | yes |
| ARM64 JIT backend | `cpu/backend/a64` | yes (x64 backend not built) |
| GPU: PM4 command processor, shader translator (SPIR-V), texture/render-target/shared-memory caches | `gpu/*.cc` | yes |
| Vulkan backend | `gpu/vulkan`, `ui/vulkan` | yes (D3D12/Metal not built) |
| Audio | `apu` + `xe_aaudio_*` / `xe_opensles_*` in `emulator-core/src/main/cpp` | yes |
| Android glue (JNI, input, surfaces, driver loading via libadrenotools) | `emulator-core/src/main/cpp/*.cpp`, `emulator-core/src/main/java` | yes |
| Frontend (Kotlin/Compose) | `app/` | yes |
| wxWidgets/ImGui desktop UI, D3D12, Metal, x64 JIT | | no |

The linked libraries are listed in `emulator-core/src/main/cpp/CMakeLists.txt`
(`target_link_libraries(emu …)`).

**Per-title mechanisms:**
- `game_quirks.cc`: built-in cvar overrides per title ID, applied at per-game-config
  priority.
- `config/<TITLEID>.config.toml`: the user's own per-game config, which wins over quirks.
- `patches/<TITLEID>.patch.toml`: guest code patches.

---

## 2. Building

### What worked in the cloud environment
- Android SDK/NDK 29.0.14206865 and CMake 3.30.3 are available. The native
  tree (`libe.so`, 752 build steps) **compiles and links cleanly** with the same
  flags Gradle passes (`-O2 -DNDEBUG -g -fno-strict-aliasing`, arm64-v8a,
  android-29).
- The `xenia-base-tests` suite was built as a static arm64 binary and **run
  under `qemu-aarch64`**. The new `guest_clock_test` passes. Three
  `memory_test` cases fail only because the qemu run has no Android
  `ASharedMemory`; a stub was linked in for the run and is not committed.

### What did not work in the cloud environment, and the workaround
- `dl.google.com` and every Google Maven mirror were blocked by the cloud
  environment's network policy. Gradle could therefore not resolve the Android
  Gradle Plugin or AndroidX, and **no APK could be produced in that sandbox**.
- To fix this, `.github/workflows/android-debug-apk.yml` builds `:app:assembleDebug`
  on GitHub's runners on every push to a non-`main` branch (and on manual
  dispatch). It uploads `XenDroid_Debug_<hash>.apk` as an artifact.
  - The APK installs as `xendroid.compose.debug`, alongside a release install,
    with its own data directory.
  - Native code is compiled `-O2`, the same as release.
  - If the fork has Actions disabled, enable them in *Settings → Actions*.
  - **Copy your per-game config and driver into the debug app's data directory**
    (paths in §5).

### Local build
See `BUILD.md`; nothing about it changed.

---

## 3. The reference configuration (what keeps rendering correct)

These are the settings in the player's working config that differ from the
bundled defaults and matter for this title:

| Setting | Bundled default | Working value | Status now |
|---|---|---|---|
| `[GPU] clear_memory_page_state` | false | **true** | **applied automatically for 4E4D080B** (game quirk) |
| `[GPU] readback_resolve` | `uma` | **`none`** | **applied automatically** |
| `[GPU] occlusion_query` | `fast` | **`fake`** | **applied automatically** |
| `[Vulkan] turnip_debug` | `sysmem` | `sysmem` | unchanged (see experiment C) |
| `[Vulkan] adrenotools_force_max_clocks` | false | true | user setting (custom driver only) |
| `[Storage] mount_cache` / `mount_scratch` | false | true | user setting |

The first three now come from `game_quirks.cc`. You can return them to the
defaults in the **global** config so other games stop paying for
`clear_memory_page_state`, and Magna Carta 2 still gets them. A per-game
config file still overrides the quirks key by key.

`xe.log` confirms they applied with lines like
`Game quirk for 4E4D080B: clear_memory_page_state (...)`.

---

## 4. Investigation: what is verified, and what is hypothesis

**Verified** means read in code with a concrete mechanism. **Hypothesis**
means plausible but not shown to cost frame time on this game.

### Verified (code facts)
1. **Kernel-call logging cost.** Every non-`kHighFrequency` kernel export
   formatted its whole argument list into a string on each call. At the default
   `log_level = 2` that string was discarded (`kernel/util/shim_utils.h`).
   Affected exports include KeResetEvent, KeReleaseSemaphore,
   ObReferenceObjectByHandle, ObDereferenceObject, KeQuerySystemTime and NtClose.
   **Fixed.**
2. **`mftb` (guest time base) took a process-wide mutex on every read.** It
   also accumulated a truncated delta per read, dropping up to (den−1)/den of a
   tick each time (`base/clock.cc`). Guest time therefore ran slow in proportion
   to how often the title read it. UE3 reads the time base for its frame timing
   and stats. **Fixed** (lock-free, exact).
3. **Critical-section spinning.** `RtlEnterCriticalSection` spun with a CAS on
   every iteration, up to `spin_count × 256` times. That took the cache line
   exclusive and stole it from the owner and from the releasing thread
   (`xboxkrnl_rtl.cc`). **Fixed** (test, then CAS, plus a spin hint).
4. **`clear_memory_page_state` re-uploads.** At every frame close it
   invalidates all valid, non-GPU-written pages in every 16 MB chunk that saw a
   CPU write fault or a GPU write. Every vertex/index/fetch range the next frame
   reads from those chunks is copied CPU→GPU again: a memcpy on the GPU command
   thread, plus a buffer copy and barriers.
   - The **size** of that cost for this game was unknown. It can now be measured
     (`log_shared_memory_uploads`, §5).
5. **The bundled template sets `framerate_limit = 0`** (no host present
   throttle). Present mode prefers IMMEDIATE, then MAILBOX. The guest vblank is
   60 Hz. Nothing paces presentation to the 30 fps the game targets, so frame
   *pacing* is left to the guest's own vblank waits.
6. **No core-type awareness.** The 6 "Guest CPU" dispatch threads and the GPU
   command thread have no affinity or priority set. Placement is left to the
   Android scheduler. Only the audio and XMA threads are reniced (−12).
7. **`turnip_debug = "sysmem"` disables GMEM tiling on Turnip.** It is the
   default because 2× MSAA in GMEM hard-faults Adreno **830** (gen8)
   (`docs/a830-gmem-msaa-plan.md`). The Thor Pro's Adreno 740 is gen7, so that
   fault was never observed on it.

### Hypotheses (need the device)
- **H1:** Magna Carta 2 is bound by the guest CPU thread(s), not by the GPU
  (typical for UE3 on Xenia).
  - Test: the `stall` column of `GpuFrame` (GPU thread idle waiting for the
    guest). A large stall at under 30 fps means CPU/guest bound.
- **H2:** `clear_memory_page_state` re-uploads are a large part of the GPU
  thread's `draw` time. Test: `SharedMemUploads` KB/frame next to `GpuFrame draw=`.
- **H3:** GMEM rendering (`turnip_debug = ""`) is faster on Adreno 740 than
  sysmem for this title (fewer DRAM round-trips) and is stable on gen7.
- **H4:** The ADPF hint session, which reports only the short audio callback
  against a 5.33 ms target, does not help hold big-core clocks for the Guest
  CPU threads.
  - Not changed, because it needs device measurement (`dumpsys` / `simpleperf`)
    first.
- **H5:** Guest dispatch threads occasionally land on A510 cores during heavy
  scenes. Not changed: pinning without data can easily make things worse.

---

## 5. Implemented changes

| Commit | Change | Expected benefit | Possible regressions |
|---|---|---|---|
| `[Kernel] Decide whether a kernel call is logged before formatting it` | Early-out before string formatting when the line would be filtered | Less host CPU per kernel call on every guest thread | None expected. Logging at `log_level ≥ 3` still prints the same lines |
| `[Base] Compute guest ticks from an anchor, lock-free…` | Seqlock-anchored guest clock, exact `floor(Δ·num/den)` | No mutex on `mftb` / system-time queries; no slow-clock drift with frequent reads | Guest time now advances at exactly the nominal rate. If a title had been tuned around the slightly slow clock, timing could look different (not expected). Covered by `guest_clock_test` |
| `[Kernel] Test a critical section before the CAS…` | TTAS spin + `yield` hint, same spin count and semantics | Less cache-line ping-pong between guest CPU threads on contended locks | None expected |
| `[GPU] Measure clear_memory_page_state re-uploads…` | `log_shared_memory_uploads` (off), `clear_memory_page_state_skip_unchanged` (experimental, **off**) | Lets you see the refresh cost. If you enable the skip, pages whose bytes are unchanged are not re-copied | Skip **off**: none (≈ a few bit operations per upload). Skip **on**: if this title needs the refresh to *overwrite GPU-side writes the emulator doesn't track*, the original bugs (flicker / missing model) can come back. That is why it is opt-in |
| `[GPU] Apply the verified Magna Carta 2 settings as title quirks` | Quirks for 4E4D080B | Correct rendering without global settings that slow other games | None for MC2 (same values as your config) |
| `[CI] Build an installable debug APK on branch pushes` | GitHub Actions workflow | An APK you can install | — |

---

## 6. Collecting performance logs on the Thor Pro

### 6.1 Per-game config for measuring

Create `config/4E4D080B.config.toml`.
- Debug APK: `/sdcard/Android/data/xendroid.compose.debug/files/compose/config/`.
- Release: `.../xendroid.compose/files/compose/config/`.

```toml
[GPU]
log_gpu_frame_time_breakdown = true   # "GpuFrame:" line every second
log_shared_memory_uploads = true      # "SharedMemUploads:" line every second

[Kernel]
guest_scheduler_stats = true          # optional: scheduler counters every second

[Display]
show_debug_overlay = false            # keep the overlay off while measuring
```

Section headers matter: a key in the wrong section is silently ignored (see
`GAME_COMPAT.md`).

### 6.2 Getting the log

```sh
adb shell ls /sdcard/Android/data/xendroid.compose.debug/files/compose/
adb pull /sdcard/Android/data/xendroid.compose.debug/files/compose/xe.log mc2-run1.log
# previous sessions are zipped under .../files/compose/logs/
python3 tools/perf_log_summary.py mc2-run1.log --from 20
```

**What the `GpuFrame` columns mean:**
- `interval avg` — the frame time.
- `max` — the worst frame in that second, which shows hitches.
- `exec` — PM4 command processing time on the GPU thread.
- `draw` — draw setup on the GPU thread, including shared-memory uploads.
- `swap` — the swap itself.
- `stall` — the GPU thread waiting for the guest to write commands. When this
  is large, the game logic and CPU emulation are the limit.

### 6.3 Optional deeper data
- **CPU hot spots:** the emulator runs in the `:emu` process
  (`adb shell pidof xendroid.compose.debug:emu`). `a64_perf_map = true` (the
  default, and in your config) plus
  `adb shell simpleperf record -g -p <pid> --duration 20 -o /data/local/tmp/perf.data`
  attributes JIT code to guest functions. This needs a debuggable build, which
  the debug APK is.
- **CPU placement:** `adb shell top -H -p <pid>` shows how the "Guest CPU n",
  "GPU Commands", "XMA Decoder" and "Audio Worker" threads load the cores. Use
  `adb shell cat /proc/<pid>/task/<tid>/stat` (field 39) to see which core each
  thread is on.
- **Thermals and clocks:** `adb shell cat /sys/class/kgsl/kgsl-3d0/gpuclk` and
  `/sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq` during the run.

---

## 7. Repeatable benchmark procedure

### Fixed conditions (write them down for each run)
- Same APK build, same driver (`vulkan_lib_path`), same AYN performance mode
  and fan mode.
- Device at room temperature and resting for ≥ 5 minutes before the run.
- Charger state fixed, airplane mode on.
- **Warm pipeline cache.** Play the route once after installing a build and
  throw that run away. The first run recompiles pipelines and reads several
  fps low (`docs/gw-gpu-bottleneck-investigation.md` §10).
- **Check that the config applied.** Grep `xe.log` for `Game quirk for 4E4D080B`
  and the log cvars before trusting any number.
- **Three runs per scenario**; report the median. Device noise is about ±2 fps.

### Scenarios
Use one save placed at the start of each scenario so every run is identical.
- **Exploration (120 s):** the first town (Abazet / the port area), walking a
  fixed loop: the same start point, a fixed path around the main street, and
  back. The camera is held the same way.
- **Combat (90 s):** a repeatable field encounter near a save point. Engage
  the same enemy group with the same party and the same chain-attack sequence.
  Start the timer on the transition into battle.
- **Cutscene (full length):** an in-engine (real-time) cutscene that a save
  starts deterministically, for example the first Abazet cutscene. Note that
  FMV cutscenes measure the video decoder, not 3D rendering. Benchmark one FMV
  separately if stutter occurs there.

### Recording
1. Start the run, play the scenario, quit to the menu, then pull `xe.log`.
2. Use `--from` with the second at which the scenario started (each `GpuFrame`
   line is one second). Run
   `python3 tools/perf_log_summary.py xe.log --from <start> --to <end> --csv run.csv`.
3. Record for each run: avg fps, 5th-percentile fps, % seconds below 30, the
   95th percentile of the worst frame, the GPU-thread breakdown, and
   `SharedMemUploads` KB/frame.

### Pass criteria for "stable 30"
- ≥ 95% of seconds at 30 fps.
- The 95th percentile of the per-second worst frame under 50 ms.
- No visual regressions (checklist in §9).

---

## 8. A/B experiments, in order

Change **one** thing per experiment, using the per-game config, and keep the
rest of §7 fixed.

| # | Change | What to look at | Risk |
|---|---|---|---|
| A | Previous APK vs this branch's APK, same config | Total frame time, `stall` | Low |
| B | `[GPU] clear_memory_page_state_skip_unchanged = true` | `SharedMemUploads` "skipped" KB, `draw` ms. **Check the hero model and the logo/loading flicker** | The original bugs could return. If they do, turn it off again; the result says the refresh is overwriting untracked GPU writes |
| C | `[Vulkan] turnip_debug = ""` (GMEM tiling) instead of `"sysmem"` | Frame time when `stall` is small (GPU-bound) | GPU hang / device lost (the a830 issue). Gen7 may be fine. Revert on any hang |
| D | `[Vulkan] vulkan_allow_present_mode_immediate = false` and `vulkan_allow_present_mode_mailbox = false` (FIFO), and/or `[GPU] framerate_limit = 30` | Per-second worst frame (pacing) and perceived smoothness | Can add a frame of latency, and can reduce fps if the game misses vsync |
| E | `[CPU] a64_perf_map = false` (keep it on only when profiling) | Hitches when new code is compiled (entering new areas) | None |
| F | `[CPU] clock_source_raw = true` | `stall`, overall fps | Uses the ARM generic timer (19.2 MHz) instead of `CLOCK_MONOTONIC_RAW`. Small timing-granularity change |

**Reading the results:**
- If `stall` dominates, the bottleneck is guest CPU emulation, and the next step
  is a `simpleperf` profile (§6.3) of the Guest CPU threads.
- If `draw`/`exec` dominate, look at `SharedMemUploads` and texture loads.
- If neither dominates and the interval is still long, the GPU itself is the
  limit, so try C.

---

## 9. Changes that need physical-device validation

- [ ] The game boots, and `xe.log` shows the three `Game quirk for 4E4D080B` lines.
- [ ] No flicker on the Softmax / UE3 logos and loading screens, and the main
      character model is present (the quirks are equivalent to your config).
- [ ] Guest timing is unchanged: in-game clock, cutscene audio/video sync, and
      combat timers behave as before (guest clock change).
- [ ] No new hangs at loading screens or in combat (critical-section spin change).
- [ ] `log_shared_memory_uploads` prints sensible numbers. Record the
      refresh-only KB/frame for each scenario.
- [ ] Experiment B: visuals stay correct, or the result is recorded that they
      don't.
- [ ] Experiment C: stability over at least 30 minutes, including combat
      effects and MSAA passes.
- [ ] FPS / pacing numbers for A–F with the §7 procedure. **No improvement
      claim before this.**

---

## 10. Not done, and why

- **Frame skipping, game-speed changes and accuracy cuts** were deliberately not
  implemented.
- **Thread affinity/priority changes for the guest dispatch threads (H5)**
  depend on scheduler behaviour that can only be measured on the device.
- **A frame-based ADPF session (H4)** has the same dependency: it can only be
  checked on the device.
- **`turnip_debug` default for gen7** was not changed (experiment C) because of
  the documented GPU-fault risk on a newer generation.
- **Enabling the shared-memory skip by default** was not done. It depends on
  experiment B showing that the refresh is not compensating for untracked GPU
  writes in this title.
