# schwung-setbfree — Tonewheel Organ for Schwung

A port of [setBfree](https://github.com/pantherb/setBfree) (Hammond B3 emulation
by Robin Gareus and Fredrik Kilander) to a
[Schwung](https://github.com/charlesvestal/schwung) sound-generator module for
the Ableton Move.

91 tonewheels, 9 drawbars, scanner vibrato, percussion, tube overdrive, reverb
and a Leslie cabinet.

## Controls

Five pages, in this order. The root level is pure navigation — it declares no
knobs of its own, so the knob grid emits no page for it and **opens directly on
Drawbars**; in the list menu it's just the five rows below, with the encoders
handed to the Drawbars row.

```
 Drawbars   16'  5 1/3'  8'  4'  2 2/3'  2'  1 3/5'  1 1/3'   (+ 1' in the list)
 Output     Leslie  Reverb  Swell  RDY
 Percussion Perc  Level  Decay  Harm
 Vibrato    Scanner  Upper  Lower
 Overdrive  Drive  Char  In  Out
```

### Drawbars — nine additive harmonics, 0..8

Each bar adds one harmonic to every note played. `0` is fully in (silent), `8`
fully out (loudest) — the same numbering as the hardware tabs.

| # | Footage | Interval | Default |
|---|---------|----------|---------|
| 0 | 16'     | sub-octave | 8 |
| 1 | 5 1/3'  | sub-third  | 8 |
| 2 | 8'      | fundamental | 8 |
| 3 | 4'      | octave | 0 |
| 4 | 2 2/3'  | twelfth | 0 |
| 5 | 2'      | fifteenth | 0 |
| 6 | 1 3/5'  | seventeenth | 0 |
| 7 | 1 1/3'  | nineteenth | 0 |
| 8 | 1'      | twenty-second | 0 |

The default `88 8000 000` is the standard jazz comping registration.

Nine bars do not fit eight encoders. Bars 0–7 are the knob row; bar 8 (`1'`) is
in the page's parameter list, so it's reachable in the menu and lands on a
continuation page in the knob grid.

### Output

- **Leslie** — Slow / Stop / Fast (default Slow). Rotating horn plus rotating
  bass drum. Slow is chorale, Fast is tremolo, Stop is the dry cabinet. The
  speed *ramps*, and horn and drum have different inertia, so they drift apart
  in transit and lock back together — switching mid-phrase is the effect.
- **Reverb** — 0..1 wet mix (default 0.1, setBfree's own). 0.2–0.4 sits behind the organ;
  above 0.6 it washes.
- **Swell** — 0..1 (default 0.8). The expression pedal, **not** a volume knob:
  it sits *before* the overdrive, so backing it off cleans the tone as well as
  quieting it.
- **RDY** — read-only status, see below.

#### RDY (read-only)

**This is a status readout, not a control.** Building the organ — 91 tonewheels,
the play matrix, the taper lists — takes about a second, and it cannot happen on
the audio callback (see [Deferred initialisation](#deferred-initialisation)). So
`create_instance` returns immediately and a background worker builds the engine.

`RDY` reads **0 while that worker is still building and 1 once the organ can
sound.** The module outputs silence while it reads 0; MIDI you send in the
meantime is queued and plays on the first audible block. It goes 0 → 1 exactly
once per instance and never returns to 0 — loading a new preset into the slot
creates a fresh instance, which starts at 0 again.

Turning the knob does nothing: it's declared `"access": "read"`. It earns its
place by making a silent module right after load read as "still loading" rather
than "broken".

### Percussion — a single decaying tap on attack

Fires on the attack of the *first* key pressed and will not retrigger until
every key is released, which is why it rewards detached playing.

- **Perc** — Off / On (default On)
- **Level** — Norm / Soft (default Norm). Soft lowers the tap relative to the drawbars.
- **Decay** — Slow / Fast (default Fast)
- **Harm** — 3rd / 2nd (default 2nd). 2nd is the bright bark, 3rd the hollow one.

As on real hardware, engaging percussion steals the 1' drawbar and thins the
overall output slightly.

### Vibrato — the mechanical scanner

A swept delay line driven by a rotating capacitor, not a pitch LFO.

- **Scanner** — `V1 C1 V2 C2 V3 C3` (default C3). `V` settings are vibrato
  (pitch only, shallow to deep); `C` settings are chorus, which mixes the dry
  signal back in — that's where the shimmer comes from. C3 is the famous one.
- **Upper** — Off / On (default On)
- **Lower** — Off / On (default Off)

Upper and Lower are the per-manual enables. **With both off, Scanner does
nothing.** Since this port has no lower manual, `Lower` has no audible effect —
it's exposed because the engine takes it.

### Overdrive — tube preamp, ahead of the Leslie

- **Drive** — Off / On (default **Off**; the other three do nothing until it's on)
- **Char** — 0..1 (default 0.5). Curve shape, soft to hard.
- **In** — 0..1 (default 0.357). Drives the stage.
- **Out** — 0..1 (default 0.079). Trims the level back down.

`In` and `Out` are a matched pair — raising `In` without lowering `Out` gets loud
fast. The low `Out` default is deliberate.

### MIDI

Notes play the upper manual; anything outside 36–96 folds into range by octaves.
`CC 11` is expression (same as Swell), `CC 91` reverb mix, `CC 120` all sound
off, `CC 123` all notes off.

**Velocity is ignored** — a tonewheel key is a set of switches, open or closed.
Use Swell for dynamics.

Changing any of the above in the on-device menu requires no rebuild *except* the
layout itself; see [Parameters](#parameters) for why the hierarchy is compiled in.


## Install

Requires Docker, and SSH enabled on the Move (development settings page). Note, you might get a DNS error when running the install.sh. An option to solve this is to run docker with --network host option: 
`docker run --network host `

```bash
git clone https://github.com/<you>/schwung-setbfree.git
cd schwung-setbfree
./scripts/build.sh
./scripts/install.sh
```

`build.sh` builds its own container image on first run and re-invokes itself
inside it, so there is nothing to install beyond Docker. If you already have an
aarch64 cross-toolchain on `PATH` it uses that directly and skips Docker
entirely.

`install.sh` defaults to `ableton@move.local`. Override with:

```bash
DEVICE=ableton@move5.local ./scripts/install.sh
```

To deploy to your device if it has another name like `move5`, first build and then run:

```bash
./scripts/build.sh
DEVICE=ableton@move5.local ./scripts/install.sh
```

## What gets built

```
dist/setBfree-organ/
  dsp.so        aarch64 shared object, exports move_plugin_init_v2
  module.json   manifest: api_version 2, component_type sound_generator
  help.json     on-device help topics
```

That directory is what lands in
`/data/UserData/schwung/modules/sound_generators/` on the device.

## Development

```bash
./scripts/test.sh              # native build + contract test — run this first
./scripts/build.sh             # cross-build aarch64, validate, stage dist/
./scripts/build.sh --debug     # keep asserts live, skip strip
./scripts/install.sh           # copy to the device
```

`scripts/test.sh` builds the same sources for the host architecture and drives
them through `tests/host_test.c`, which calls the module exactly the way
`chain_host.c` does. It runs in about a second and catches most of what would
otherwise cost an `scp` and a `tail -f` to find.

See [`TESTING.md`](TESTING.md) for what the three test layers cover, and
[`DEBUGGING.md`](DEBUGGING.md) for on-device diagnostics.

## Architecture

### Deferred initialisation

**Every plugin entry point runs on the SPI audio callback** — `create_instance`,
`destroy_instance`, `set_param`, `get_param`, `on_midi`, `render_block`. There
is no control thread. The contract at the top of `src/host/plugin_api_v1.h` is
the authority: SCHED_FIFO 70, pinned to core 3, roughly 2370 µs of slack per
128-frame block.

setBfree's init is nowhere near that budget. `initToneGenerator()` builds 91
tonewheels, compiles the play matrix and allocates the taper lists. Doing it
inline in `create_instance` is what produced

```
param-slow: set slot 2 synth:module took 4294967.295 ms on the SPI callback
```

— the `0xFFFFFFFF` µs overflow sentinel, i.e. the callback was held long enough
to stall the whole device rather than just this slot.

So `create_instance` does one `calloc` and starts a worker. The worker **demotes
itself first** (threads inherit FIFO 70, which would starve Move's own `Link
Main` at FIFO 35), pins to cores 0–2 to leave core 3 for SPI, builds the organ,
and publishes it with a release store. `render_block` emits silence until then,
and the read-only `ready` parameter reports 0/1 so the UI can show it.

### Signal path

```
oscGenerateFragment  ->  preamp  ->  reverb  ->  whirlProc  ->  int16 L/R
   (mono, always            (dispatches to      (Leslie, mono
    exactly 128              overdrive           in, stereo out)
    samples)                 internally)
```

`oscGenerateFragment` **ignores its `lengthSamples` argument** — every mixdown
loop in `tonegen.c` is `for (i = 0; i < BUFFER_SIZE_SAMPLES; i++)`. It always
writes exactly 128 mono floats, so the wrapper carries a 128-sample buffer and
serves whatever block size the host asks for out of it.

### Parameters

The cache in `plugin_instance_t` is authoritative and is what `get_param`
answers at all times, including before the organ exists. Changes reach the
engine through setBfree's own public control path — `setDrawBars()` and
`notifyControlChangeByName()` — not by writing component state directly.

Drawbars are **0..8**, matching both the hardware tabs and `tonegen.c`'s
`assert(setting < 9)`.

`ui_hierarchy` and `chain_params` are served from `get_param`. For a sound
generator the chain host reads the hierarchy **only** from the plugin
(`chain_host.c:2036`, no fallback) — a hierarchy declared in `module.json` is
silently ignored, which is why the Move menu had no pages.

**Changing the UI layout therefore requires a rebuild.** Both the hierarchy and
the parameter list are string literals in `src/schwung_bfree/schwung_bfree.c`;
`module.json` carries copies so the manifest and the plugin cannot describe
different modules, and the contract test fails if they disagree. Editing
`module.json` alone changes nothing. `help.json` and the manifest's `name` /
`abbrev` / `description` fields *are* read from the files and need no rebuild.

## Layout

```
src/setbfree/          upstream setBfree, unmodified
src/schwung_bfree/     the port
  schwung_bfree.c        instance, deferred init, params, render
  schwung_bfree.h        shared prototypes, so the compiler checks the wrapper
  schwung_wrapper.c      move_plugin_init_v2 and the v2 vtable
  setbfree_stubs.c       SampleRateD, mainConfig, mainDoc
  module.json / help.json
src/host/plugin_api_v1.h   verbatim copy of schwung's; test.sh diffs it
tests/                 host_test.c (contract), rt_guard.c (LD_PRELOAD)
scripts/               build.sh, install.sh, test.sh, validate.sh, check-help.py
```

`src/host/plugin_api_v1.h` must stay identical to schwung's. Header drift
between a module's copy and the host's is what boot-looped a device via
breakbeat: an extra declared callback made a guarded call read 8 bytes past the
struct. The header carries a `_Static_assert` about it, and `test.sh` diffs the
two on every run when a schwung checkout is available (set `SCHWUNG_ROOT`, or
place it next to b5).

## Licence

GPL-2.0-or-later — see [`LICENSE`](LICENSE). setBfree is GPL-2.0-or-later and
this is a derivative work of it.

Third-party components and their notices are listed in
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md). In short: the audio engine
is setBfree (GPL-2.0-or-later, © Fredrik Kilander, Robin Gareus, Ken Restivo,
Will Panther), and `src/host/plugin_api_v1.h` is from Schwung (MIT, © Charles
Vestal).
