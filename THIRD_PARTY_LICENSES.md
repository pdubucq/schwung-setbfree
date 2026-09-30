# Third-Party Licenses

b5's own source code is licensed under the **GNU General Public License,
version 2 or later** — see [`LICENSE`](LICENSE). It has to be: the module is a
derivative work of setBfree, which is GPL-2.0-or-later.

This document lists the third-party components in this repository, their
licenses, and how each one reaches the shipped artifact.

---

## Shipped artifact

| Artifact | Licence as conveyed | Why |
|---|---|---|
| `dsp.so` | **GPL-2.0-or-later** | setBfree (GPL-2.0-or-later) compiled in, plus b5's own GPL-2.0-or-later port code |

`dsp.so` is a combined work. The MIT-licensed header below imposes no
conditions on it beyond preserving its own notice, which this file does.

---

## setBfree

- **Upstream:** https://github.com/pantherb/setBfree
- **Licence:** GPL-2.0-or-later
- **Location:** `src/setbfree/`
- **Used in:** the entire tone generator, scanner vibrato, percussion,
  overdrive, reverb and Leslie — i.e. all of the audio engine
- **Modified:** no. The files are upstream's, unchanged.

```
Copyright (C) 2003-2004 Fredrik Kilander <fk@dsv.su.se>
Copyright (C) 2008-2018 Robin Gareus <robin@gareus.org>
Copyright (C) 2010 Ken Restivo <ken@restivo.org>
Copyright (C) 2012 Will Panther <pantherb@setbfree.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
```

Upstream's `memstream.c` — the one GPL-3.0 file in that tree — is **not**
included here. It reimplements `open_memstream()` for platforms without it,
glibc provides it natively, and it was already excluded from the build. Its
9-line header `memstream.h` remains because `pgmParser.h` includes it; the
declarations are inert behind `#ifndef HAVE_MEMSTREAM`.

---

## Schwung plugin API header

- **Upstream:** https://github.com/charlesvestal/schwung
- **Licence:** MIT
- **Location:** `src/host/plugin_api_v1.h`
- **Used in:** the ABI contract between `dsp.so` and the Schwung chain host
- **Modified:** no. Byte-identical to upstream, and `scripts/test.sh` checks
  that on every run when a schwung checkout is available — a drifted copy of
  this header is what once boot-looped a device.

The file carries no in-file notice upstream, so MIT's attribution requirement
is satisfied here:

```
MIT License

Copyright (c) 2025-2026 Charles Vestal

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

MIT is GPL-compatible, so including this header in a GPL-2.0-or-later work is
permitted. The header itself remains MIT and is reusable as MIT by anyone.
