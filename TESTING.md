# Testing

Three layers, cheapest first. The point of the order is that almost nothing
should reach the device untested — the previous iteration of this project had
only the third layer, and every defect cost an `scp` and a `tail -f` to find.

| Layer | Command | Catches |
|---|---|---|
| Structural | `./scripts/validate.sh` | wrong arch, missing export, manifest/artifact name mismatch, broken help.json |
| Contract | `./scripts/test.sh` | wrong behaviour, wrong timing, allocation on the audio thread |
| Device | see `DEBUGGING.md` | anything that only happens on real hardware |

## The contract test

```bash
./scripts/test.sh
```

Builds the module **natively** and drives it through `tests/host_test.c`, which
calls it exactly the way `chain_host.c` does: same entry symbol, same call
order, same raw 3-byte MIDI, same `get_param` keys.

The native build is not shippable and is not meant to be. It exists so a logic
error fails in a second. It is also built `-DCMAKE_BUILD_TYPE=Debug` on purpose,
which leaves setBfree's own asserts live — `tonegen.c`'s `assert(setting < 9)`
in particular, so a drawbar range bug aborts here instead of being clamped away
by the release build's `-DNDEBUG` and going wrong quietly on device.

The old harness, `test_setBfree.c`, was compiled x86-64 and pointed at an
aarch64 `dsp.so`. Its `dlopen` could never succeed, so it passed by never
running anything. It also declared its own truncated four-field
`host_api_v1_t`. The replacement includes the real `host/plugin_api_v1.h`.

### What it asserts

**Timing.** These are not performance nits. Every entry point runs on the SPI
callback, which has ~2.4 ms of slack for the *whole chain* per 128-frame block.

- `create_instance` under 50 ms — a generous ceiling that still catches "the
  module built the whole organ inline", which is the original crash.
- first `render_block` under 5 ms and **silent**, while the worker builds.
- worst `render_block` under 1.0 ms for 128 frames (2.9 ms of audio).
- `destroy_instance` under 50 ms.

It also prints how long deferred init actually took. That number is how long
`create_instance` would have blocked the callback in the old design.

**Realtime cleanliness.** `tests/rt_guard.c` is an `LD_PRELOAD` interposer on
`malloc`/`calloc`/`realloc`/`free`/`open`/`fopen`/`opendir`. The harness arms it
around the calls that must be clean and disarms it around the ones that may
allocate. Arming is **per-thread**, which is the whole trick: the deferred-init
worker is allowed to allocate and runs concurrently with the audio-thread calls
being policed.

`host_api_v1_t.log` is wired to a callback that records a failure. The module
must never log — logging is forbidden from the audio thread and every entry
point is on it.

**The `get_param` contract.**

- `preset_name` returns **0 with an empty string, not -1**. A negative return
  means "the read did not complete" and the grid is right to retry it; voice-poc
  measured 29 failed reads per second from getting this wrong, on the single
  param channel every value on screen shares.
- `state` returns a payload. A module that does not answer it makes the slot
  autosave retry forever on that same channel.
- `ui_hierarchy` is served **by the plugin**. For a sound generator the chain
  host reads it only from `get_param` (`chain_host.c:2036`, no fallback) — this
  is the reason the Move menu had no pages.
- an unknown key returns negative, distinguishably from an empty answer.
- a too-small buffer returns negative rather than truncating silently.

**Manifest agreement.** `module.json` and the plugin must declare the same keys,
and every key `module.json` declares must be readable. `chain_params` falls back
to the manifest, so both copies are live; the manifest is the one nobody
recompiles, so it is the one that rots.

**Sound.** A silent organ passed every structural check the old harness had, so
the test plays a note and requires a peak above 100/32767 that is not pinned at
full scale.

**Block sizes 1..256.** `oscGenerateFragment` always produces exactly 128 mono
samples regardless of what it is asked for, so every size other than 128
exercises the carry buffer. The old code's `wbuf = fbuf + frames*2` ran off the
end of its allocation at 256.

**Malformed MIDI, full-range MIDI, multiple instances, and
create-then-immediately-destroy** (which cancels the worker mid-init — the one
path where destroy legitimately waits on a join).

## Structural validation

`scripts/validate.sh` runs automatically at the end of `scripts/build.sh`. It
inspects the binary **without loading it**, so it works on the cross-compiled
aarch64 artifact from an x86-64 host:

- ELF, `ET_DYN`, aarch64
- `move_plugin_init_v2` exported (everything else is hidden by
  `-fvisibility=hidden`)
- no unexpected undefined symbols — `-Wl,--no-undefined` should already have
  caught these at link time, so a leftover means the flag was dropped
- `module.json`'s `"dsp"` field matches the artifact filename. The old scripts
  got this wrong in both directions: the build produced `dsp.so` while the
  manifest asked for `setBFree.so`, and the validator looked for `setBFree.so`
  and exited 1 under `set -e`
- `module.json` declares `api_version: 2` and parses
- `help.json`, via `scripts/check-help.py`

## help.json

```bash
python3 scripts/check-help.py
```

Two failure modes, both silent on device:

1. **Schema.** The loader's entire test is `if (helpData.children)`. A file that
   is valid JSON but names its topics anything else is discarded without a word
   and the viewer reports "No help content available for this module", exactly
   as if the file were absent. b5 shipped in that state — several KB of help
   under a `sections` key that nobody could read.

2. **Width.** A help line is drawn, never wrapped and never truncated. Pixels
   past x=127 are dropped by `set_pixel` with no error anywhere, so an over-long
   line loses its tail with no ellipsis and nothing in the log.

   The budget is **pixels, not characters**: `load_font` trims every glyph to
   its own inked extent, so the atlas is fixed-pitch but the screen is
   proportional — `.` advances 3 px and `W` advances 6 px. The checker measures
   against the `FONT` table in schwung's `scripts/generate_font.py`, which
   `schwung_host.c` names as the single source of truth for the atlas. That half
   of the check needs schwung checked out next to b5 or `SCHWUNG_ROOT` set; the
   schema half always runs.

## Header drift

`scripts/test.sh` diffs `src/host/plugin_api_v1.h` against schwung's on every
run, ignoring line endings — both repos store the file as LF, but a Windows
checkout has CRLF in the working tree and that difference says nothing about
the ABI. Anything else must match exactly. Drift between a module's copy of
this header and the host's is what boot-looped a device via breakbeat: the
module declared one extra callback, so a guarded call read 8 bytes past the
struct and jumped into the heap. The header carries a `_Static_assert` on
`offsetof(host_api_v1_t, reserved) == 120` for the same reason.

The check needs a schwung checkout — `$SCHWUNG_ROOT`, `../schwung`, or
`/workspaces/schwung`. **Drift is always fatal. A missing checkout is fatal
only when `SCHWUNG_ROOT` is set**, i.e. when the check was asked for and could
not run; otherwise it warns and continues, so that a clone of this repo on its
own still builds and tests. Set `SCHWUNG_ROOT` in your own environment to make
it mandatory.
