# Debugging on the device

Read `TESTING.md` first. Almost everything reproduces in `scripts/test.sh`
in a second; this file is for what does not.

Device defaults to `move.local`, user `ableton`. Override with `DEVICE=ableton@move5 ./scripts/install.sh`.

## The unified log

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'
ssh ableton@move.local 'tail -f /data/UserData/schwung/debug.log'
```

Arm it **before** loading the module — the interesting part is the load.

Disarm it when you are done. `debug_log_on` adds file I/O on the realtime path
and has itself caused the audio dropouts it was being used to hunt.

### What a healthy load looks like

```
[chain-v2] Loading synth: .../setBfree-organ/dsp.so
Parsed ui_hierarchy params: count=0
No inline params in ui_hierarchy, falling through to chain_params
```

`count=0` is **expected and correct**. The hierarchy's `params` entries are bare
`{"key": ...}` references, which the parser skips as navigation items; it then
reads the `chain_params` array. A non-zero count here would mean the hierarchy
is shaped differently from what this module serves.

No `create_instance failed`. No `param-slow`. No `dlopen` error.

The organ renders silence for the first moment after load while the
deferred-init worker builds the tone generator. The `RDY` readout on the Output
page flips 0 → 1 when it is live. If it stays 0, init failed — see below.

## param-slow

```
param-slow: set slot 2 synth:module took 4294967.295 ms on the SPI callback
            — the module is doing blocking work in its entry point
```

**This is always on. There is no flag to arm**, because module entry points *are*
the SPI callback and most of the ecosystem does not know it, so a module
blocking in `set_param` is the steady state rather than an anomaly. It fires at
1000 µs.

`4294967.295 ms` is not a duration, it is `0xFFFFFFFF` µs — the overflow
sentinel. It means the callback was held long enough to stall the whole device,
not just this slot. That exact line is what this port was rebuilt to fix: the
previous version called `initToneGenerator()` inline in `create_instance`.

If it reappears, something has moved back onto the callback. Check that
`init_worker` is still doing the allocation and that nothing new was added to
`set_param` or `get_param`.

## Symptoms

### No pages in the Move menu

The chain host reads a **sound generator's** `ui_hierarchy` only from
`get_param` (`chain_host.c:2036`) — there is no fallback. A hierarchy declared
in `module.json` is ignored without comment. Confirm the plugin serves it:

```bash
./scripts/test.sh   # asserts ui_hierarchy > 0 with a levels/root object
```

### Module does not appear at all

Nothing loaded it. Check, in order:

```bash
ssh ableton@move.local 'ls -l /data/UserData/schwung/modules/sound_generators/setBfree-organ/'
```

- all three files present (`dsp.so`, `module.json`, `help.json`)
- `module.json`'s `"dsp"` field matches the actual filename. If it does not, the
  host reports only "dlopen failed", on the audio thread, with no hint that the
  name is the problem. `scripts/validate.sh` checks this.
- `"api_version": 2`. The chain host rejects v1 synths.
- `aarch64-linux-gnu-nm -D build/modules/dsp.so | grep move_plugin_init_v2`
  shows a `T`. Everything else is hidden by `-fvisibility=hidden`.

### Loads, but silent

`RDY` tells you which half it is.

- **RDY = 0**: the init worker never finished. Either an allocation returned
  NULL or it aborted. `plugin_get_error` carries the message; the chain host
  surfaces it on the slot.
- **RDY = 1**: the organ built and something downstream is wrong. Check the
  drawbars are not all 0 (one at 8 is enough), the swell pedal is not at 0, and
  that MIDI is arriving. Note that the module takes **raw 3-byte MIDI** with
  status in `msg[0]` — the earlier version expected USB-MIDI framing and
  required `len >= 4`, so it never received a single message and this looked
  exactly like a dead synth.

### Crash on load

```bash
ssh ableton@move.local 'journalctl -n 100 -xe'
```

`[CRASH] [shim]` at init usually means the entry point was not found or has the
wrong signature. Anything inside setBfree's own code is more likely a pointer
type: `setDrawBars()` and the MIDI dispatch take a `b_instance*`, and passing a
`b_tonegen*` instead reinterprets tonewheel state as the `->synth` and
`->midicfg` pointers. That was a real bug here, and it is also invisible to the
compiler, which is why `schwung_bfree.h` exists.

## Realtime instruments

Both off by default, both need `debug_log_on` as well.

### Stray realtime threads

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/rt_thread_audit_on'
```

Reports **arrival** (a realtime thread that was not there before, attributed to
the module whose `<prefix>:module` write was most recent) and **burn** (CPU
consumed at realtime priority since the last sample).

This module starts one worker per instance. It must appear as SCHED_OTHER, not
FIFO. Threads inherit SCHED_FIFO 70, so a worker that forgets to demote itself
outranks Move's own `Link Main` at FIFO 35 and starves it. `init_worker`'s first
action is `sched_setscheduler(0, SCHED_OTHER, ...)`, followed by pinning to
cores 0–2 so core 3 stays free for SPI.

A name check will not find a stray worker: a child inherits the parent's `comm`,
so an inherited thread reports as `Audio Main/SPI` and is indistinguishable in
`top`. The audit diffs the set of realtime threads by tid for that reason.

### SPI frame tally

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/spi_tally_on'
```

```
spi-tally: 345 frames / 345 irq  tx avg 389us (max 447us)  headroom 2513us  backlog 8
```

345/s is correct — the block rate is 344.5 Hz (128 frames at 44.1 kHz, 2.902 ms
per block), not 44.

`backlog` is the thing to watch. ablspi's IRQ is a counting semaphore, so
overrunning the budget does **not** drop a frame: it queues, and the following
waits return immediately and replay back-to-back. A late frame appears as a
burst, not a gap, which is why it so often gets blamed on somebody else's
producer.

The tally stays silent for ~20 s after arming. That looks like a broken build
and is not.

**A single blown frame is invisible here** — `backlog` and `frames / irq` are
1 Hz aggregates, and a 20 ms serve has produced zero `LATE` lines across a whole
session. For a one-frame stall read `spi_timing`'s `Pre(us): ... param=avg/max`
instead.

## Budget

~2370 µs of real slack per block, not the ~900 µs older notes claim. The ioctl
takes 2569 µs but only 389 µs of that is the transfer; the rest is
`wait_event_interruptible` blocking for the next XMOS IRQ — frame pacing, not
work.

Corollary: **`total_us` is not a load signal.** Our work growing shrinks the
driver's wait by the same amount, so the loop total sits near the period
whatever we do.
