# Native Linux run from a Live USB: operator manual

**Goal:** boot the same laptop (Ryzen 7 7840HS) into native Ubuntu from a USB stick, run every
measurement that WSL2 cannot do, and save all output automatically to a USB drive, without
installing anything on the laptop's disk. The comparison baseline is "WSL2 reference data" in
`last-task-completed.md`. The measurements are listed in that file's "Native Linux Checklist".

**Time:** day 1 takes ~60 min (≈20 min setup + ≈25 min unattended run). Days 2 and 3 take ~25 min each.
The Phase 4 variance check needs runs on **three separate days**.

**What gets saved automatically:** `scripts/native_run.sh` tees every line printed in the terminal
into `transcript.log`, writes one log per step, and generates `summary.md` plus TSV tables. You
don't need to copy anything from the terminal by hand.

---
## 1. What you need
| Item | Notes |
|---|---|
| **USB stick A**, ≥ 8 GB | Becomes the Ubuntu live USB. **It will be erased.** |
| **USB drive B**, ≥ 1 GB, formatted **exFAT**, label `LOBDATA` | Carries the repo in and the results out. exFAT works on both Windows and Linux. Avoid NTFS drives that Windows has hibernated (Linux mounts them read-only). |
| Ubuntu **24.04 LTS Desktop** ISO | from ubuntu.com/download/desktop |
| Rufus (Windows) or balenaEtcher | writes the ISO to stick A |
| AC charger | must stay plugged in for every run |
| Wi-Fi (or phone USB tethering) | the live session installs packages each boot |
| BitLocker recovery key (just in case) | booting from USB normally doesn't trigger it. Changing BIOS settings (e.g. Secure Boot) can, so **leave Secure Boot on**; Ubuntu boots with it. |

A single stick can hold both, via Rufus's persistence option (§3), but two devices is simpler and safer.

## 2. Before leaving Windows (in WSL)
1. Make sure everything is committed. Results record the commit hash and flag a dirty tree.
   ```bash
   cd ~/projects/lob-engine
   git status --short          # should print nothing
   git log --oneline -1
   ```
2. Plug in drive B and note its Windows drive letter (e.g. `D:`). Then write the repo as a single bundle file:
   ```bash
   git bundle create /mnt/d/lob-engine.bundle --all
   git bundle verify /mnt/d/lob-engine.bundle      # "... is okay"
   ```
   Event data (`data/*.bin`) isn't needed. It's regenerated on the live system and verified by checksum.
3. Optional: copy this manual (`docs/NATIVE-LIVE-USB.md`) to drive B so you can read it while booted into Linux.
4. Eject drive B from Windows ("Safely remove").

## 3. Make the live USB (stick A)
1. Open Rufus, select stick A, select the Ubuntu ISO. Use partition scheme **GPT** and target system **UEFI**.
2. *Optional but recommended for the 3-day protocol:* set **Persistent partition size** to 4–8 GB. The live session then keeps installed packages and settings between boots, so days 2–3 skip most of §5. Without it, redo §5 each day (~10 min).
3. Start the write and wait for it to finish, then eject.

## 4. Boot into the live session
1. Plug in **AC power**. Insert stick A (and drive B).
2. Reboot, choosing the USB in one of these ways:
   - **Windows:** Settings → System → Recovery → Advanced startup → **Restart now** → *Use a device* → the USB stick. Or:
   - **Boot menu key** during power-on: usually **F12**, sometimes F7, F11, Esc or F2, depending on the laptop vendor.
3. In the GRUB menu, choose **Try or Install Ubuntu**. When the installer window appears, choose **Try Ubuntu** or close the installer. **Do not install.**
4. Connect to Wi-Fi from the top-right menu.
5. **Stop the screen from sleeping.** A suspend in the middle of a run ruins it. Go to Settings → Power and set:
   - Screen Blank = **Never**;
   - Automatic Suspend = **Off**;
   - Power Mode = **Performance** (if offered).

## 5. Set up the session (Terminal: Ctrl+Alt+T)
Drive B mounts automatically at `/media/ubuntu/LOBDATA` (check with `ls /media/ubuntu`).
The live image has no `git`, so install it first, then clone the repo into home. Home lives in RAM,
which is fine: results go to drive B.
```bash
sudo add-apt-repository -y universe && sudo apt update && sudo apt install -y git
git clone /media/ubuntu/LOBDATA/lob-engine.bundle ~/lob-engine
cd ~/lob-engine
git log --oneline -1                     # same commit as in §2
scripts/native_setup.sh                  # toolchain, perf, governor/EPP = performance; prints a verify block
```
**Check setup's "Verify" block** before moving on:
| Line | Must show |
|---|---|
| `perf: OK` with cycles/instructions counts | if it says `perf counters unavailable`, see §10 |
| `clocksource:` | normally `tsc` (recorded either way) |
| `power:` | `AC online` |

Then run a 2-minute smoke test. It uses small event counts and saves to a throwaway folder:
```bash
QUICK=1 scripts/native_run.sh full /tmp/smoke
cat /tmp/smoke/*/summary.md | head -60    # Warnings should NOT list WSL2 or perf-unavailable
```

## 6. Run the measurements
Close every other window (browser, Files), and let the laptop idle for ~5 minutes after setup so
it cools down. Don't touch it while the run is going.

**Day 1: full run (~25 min):**
```bash
cd ~/lob-engine
scripts/native_run.sh full /media/ubuntu/LOBDATA/lob-results
```
It performs, in order:
1. release build;
2. environment capture and pre-flight warnings;
3. `check.sh` (ASan/UBSan + release);
4. TSan build + tests;
5. regenerate the 1M/5M logs and verify the golden checksums;
6. `perf stat` ×5 repeats for all 7 storage variants on 1M and 5M;
7. THP `never` vs `always` with dTLB counters (restored afterwards);
8. both latency timers;
9. `bench_match` on all 7 standard/controlled-depth workloads;
10. `scripts/bench.sh 5000000`;
11. a second environment capture (temperatures and frequency at the end).

**Days 2 and 3: variance run (~8 min):** boot the live USB again (§4), redo §5 if you didn't use persistence, then:
```bash
cd ~/lob-engine
scripts/native_run.sh variance /media/ubuntu/LOBDATA/lob-results
scripts/native_compare.sh /media/ubuntu/LOBDATA/lob-results
```
`native_compare.sh` prints each workload/variant's medians across runs and flags spreads above
the Phase 4 target of ~5% with `!`. Try to keep conditions alike each day: same core (`CORE=2`,
the default), similar room temperature, on AC, and an idle cool-down first.

**How to read the end of a run:**
- `DONE. Results in: …`: finished; the files are on drive B.
- `!!! CHECKSUM MISMATCH`: the numbers from this run are **not valid**. Bring the logs back and investigate before trusting anything.
- `note: some steps exited non-zero`: check the `Steps` table in `summary.md`. On native Linux every step should be `0`, including `tsan`.
- **Useful environment variables:** `CORE=4 scripts/native_run.sh …` pins another core (avoid core 0 and your core's SMT sibling, shown in `env.txt`). `QUICK=1` is for smoke tests only.

## 7. Save and bring the results back
1. The script writes directly to drive B and calls `sync` at the end. Before unplugging, still **eject** drive B in the Files app (or run `sync`).
2. Anything left in `~/lob-engine` disappears at shutdown. That's expected; drive B has everything.
3. Back in Windows/WSL, copy the results into the repo's git-ignored `results/` folder:
   ```bash
   mkdir -p ~/projects/lob-engine/results
   cp -r /mnt/d/lob-results/* ~/projects/lob-engine/results/
   scripts/native_compare.sh results
   ```
4. Ask Claude to **import the native results**. That turns them into benchmark-log rows, updates the Phase 2/3/4 experiment notes, and ticks the checklist in `last-task-completed.md`. The docs are the permanent record; `results/` holds the raw evidence.

## 8. What each run folder contains
`lob-results/<YYYY-MM-DD_HHMMSS>_<full|variance>/`

| File | Contents |
|---|---|
| `summary.md` | start here: environment, warnings, step exit codes, golden checksums, throughput, latency, derived perf counters (IPC, instructions/event, branch-miss %, L1d/cache/dTLB misses per event) |
| `transcript.log` | every line printed in the terminal during the run |
| `NN-<step>.log` | raw output of each step (e.g. `12-perf_1000000_dense.log`, `27-thp_always.log`) |
| `env.txt` | full environment: CPU, kernel, live/WSL flags, governor, EPP, boost, frequencies, AC/battery, clocksource, THP, temperatures, compilers, flags, `lscpu`, top processes |
| `warnings.txt` | pre-flight warnings (empty is best) |
| `steps.tsv` | step, exit code, seconds |
| `golden.tsv` | workload, expected checksum, actual, OK/MISMATCH |
| `results.tsv` | `bench_match` rates: stamp, mode, workload, variant, median, min, max (input to `native_compare.sh`) |
| `latency.tsv` | per workload and timer: overhead, p50, p90, p99, p99.9 |
| `perf.tsv` | raw `perf stat` counts with ± variance per events/variant/counter |

## 9. Known limits of this setup
- **`perf stat` counts include setup work.** They cover the whole `replay_cli` process, including reading the 32/160 MB log and building the book, so per-event values are upper bounds. Comparisons between variants are still fair, since every variant has the same overhead.
- **The THP comparison is system-wide.** It flips `transparent_hugepage/enabled` for everything. A prefault-only baseline isn't available, because no CLI flag triggers the pool prefault yet (`prefault_pool()` is only called from a unit test).
- **Laptop limits:** boost stays whatever the firmware allows and is recorded in `env.txt`. Laptop thermals and boost make a < 5% cross-day spread hard to reach. If you miss it, that is itself a result: record it with the temperatures.

## 10. Troubleshooting
| Problem | Fix |
|---|---|
| USB stick doesn't appear in the boot menu | Rewrite it with Rufus using GPT/UEFI; try another port; use Windows Advanced startup (§4). Keep Secure Boot **on**. |
| No Wi-Fi in the live session | Use phone USB tethering, or Ethernet |
| `linux-tools-<kernel>` not found / `perf not found for kernel` | The live kernel is newer or older than the archive's tools. Install `linux-tools-generic`, then `for p in /usr/lib/linux-tools/*/perf; do sudo ln -sf "$p" /usr/local/bin/perf; done` and rerun `native_setup.sh`. |
| `perf` shows `<not supported>` for some events | Some generic events don't map on this CPU. They're recorded as `n/a`; other counters still work. |
| `Permission denied` / paranoid errors from perf | Rerun `scripts/native_setup.sh` (it sets `perf_event_paranoid=1`, which resets on every boot) |
| `cannot create …/lob-results` or read-only drive | Drive B isn't mounted, or is NTFS with Windows hibernation. Reformat it as exFAT on Windows. |
| Warning "governor is not 'performance'" | Rerun `scripts/native_setup.sh` (resets each boot unless persistence is on) |
| THP steps skipped: "passwordless sudo unavailable" | Only happens outside the live session; run `sudo -v` first, or accept the skip |
| Screen locked or laptop suspended mid-run | That run is invalid (see the step durations in `steps.tsv`). Fix the §4 power settings and rerun. |
| Terminal closed mid-run | Partial logs stay on drive B; start a new run (new folder) |
| `!!! CHECKSUM MISMATCH` | Stop. Don't use the numbers; bring the folder back for investigation. |
