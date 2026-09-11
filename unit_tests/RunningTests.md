# Tests and code coverage

Tools: QTest (Qt 6.8.3) · gcov 13.3.0 · gcovr 7.0 · lcov 2.0 (see the caveat below)
Target: KArchive at commit `633dc09`

Under the course rules all test categories count as a single tool, with code
coverage tracking mandatory alongside them. This directory is that item.

---

## Reproducing

```bash
./unit_tests/run_tests.sh
```

That script builds everything with gcov instrumentation, then measures coverage
**twice** — once running only KArchive's own autotests, once running those plus
ours — so our contribution shows up as a delta rather than a claim. Outputs land
in `unit_tests/coverage/`:

| File | Contents |
|---|---|
| `baseline.txt`, `baseline-branch.txt` | upstream autotests only |
| `combined.txt`, `combined-branch.txt` | upstream + ours |
| `delta.txt` | per-file difference, line and branch |
| `coverage.html` | browsable combined report (self-contained) |
| `ctest.log`, `ctest-baseline.log` | the test runs themselves |

To run the suites alone, without coverage:

```bash
cmake -S . -B build -G Ninja && cmake --build build
cd build && ctest --output-on-failure
```

Prerequisites not met by stock Ubuntu 24.04: **extra-cmake-modules ≥ 6.21**
(Ubuntu ships 5.115.0, so a newer ECM must be installed from source — it lands in
`/usr/local/share/ECM`) and **Qt ≥ 6.8** with Core, Test and Network.

---

## What we wrote, and why

Upstream's own autotests are good: they already reach **82.8% line coverage**.
Adding more line coverage was never going to be the useful contribution. But
**branch coverage was only 66.3%**, and the uncovered branches are concentrated
in the error paths taken when an archive is damaged — which is exactly where
KArchive's real bug history lives. Two of the five upstream commits before our
pinned revision are *"7z: Fix infinite loop in malformed file"*, and
`autotests/data/` contains regression files named after OSS-Fuzz issues.

So both suites aim at branches, not lines.

### `tests/vs_robustnesstest.cpp` — parsers vs. malformed input

187 test cases across all five formats (7z, zip, tar.gz, ar, rcc):

- **truncation** at 0/1/5/10/25/50/75/90/99% of the original file;
- **byte corruption** at offsets 0–511, set to `0x00` and `0xFF`, keeping file
  length intact so that length-based sanity checks still pass;
- **degenerate input** — empty files, all-zeros, all-`0xFF`, plain ASCII, and
  bare format signatures with nothing after them.

The contract asserted is deliberately weak, because it is the only one a parser
of untrusted input can honestly promise: `open()` may succeed or fail but must
return rather than crash; if it succeeds, `directory()` must be non-null and
every reachable entry must be readable without crashing. We do **not** assert
that damaged archives fail to open — many formats are partly recoverable, and
demanding failure would encode our guesses rather than the library's contract.

Of 185 malformed inputs, 91 were accepted by `open()` and 94 rejected.

### `tests/vs_roundtriptest.cpp` — writers and filters vs. awkward valid input

41 cases: write/read round trips for every writable container and compression
backend (tar, tar.gz, tar.bz2, tar.xz, tar.zst, zip, 7z), empty archives,
zero-length members, incompressible payloads, reading a member twice, seeking
through `createDevice()`, and non-ASCII filenames.

---

## Results

```
                 baseline      combined     delta
lines             82.8%         84.1%       +1.3pp
functions         91.2%         91.7%       +0.5pp
branches          66.3%         68.1%       +1.8pp   (+49 branches)
```

Branch coverage by file, largest gains first:

| File | Baseline | Combined | Delta |
|---|---:|---:|---:|
| `kar.cpp` | 50% | 60% | **+10pp** |
| `krcc.cpp` | 37% | 43% | **+6pp** |
| `kzstdfilter.cpp` | 66% | 70% | +4pp |
| `kgzipfilter.cpp` | 67% | 70% | +3pp |
| `ktar.cpp` | 68% | 71% | +3pp |
| `kzip.cpp` | 65% | 68% | +3pp |
| `k7zip.cpp` | 67% | 68% | +1pp |
| `karchive.cpp` | 67% | 68% | +1pp |

The gains land on the two parsers that were weakest to begin with, which is where
they were aimed. The headline percentages are modest, and that is the honest
result — against an already well-tested library, a couple of points of branch
coverage is what a focused negative-testing suite buys.

**The real result is two defects**, both reproduced deterministically:

1. **`KRcc` crashes (SIGBUS/SIGSEGV) on a corrupted `.rcc`** — a single flipped
   byte is enough.
2. **`K7Zip` corrupts every non-ASCII filename**, and for characters above
   U+00FF writes archives it cannot itself reopen.

Full write-ups, root causes and minimised cases: [`FINDINGS.md`](FINDINGS.md).
Standalone reproducers: `./unit_tests/reproducers/build_and_run.sh`.

---

## Two things worth knowing about the tooling

**`lcov --list` is broken in lcov 2.0-1 on Ubuntu 24.04.** It reported 11.5% line
coverage and 0.0% function coverage for data that `genhtml` reads as 82.3%/88.6%
and `gcovr` reads as 82.8%/91.2%. The `.info` file itself is correct — checking
the raw records for `k7zip.cpp` gives `LF:1758 LH:1469`, i.e. 83.6%, matching
gcovr exactly — so the fault is in `--list`'s summary only. The contradiction
(non-zero line coverage alongside 0% function coverage) is what gave it away.
`run_tests.sh` uses gcovr for all numbers.

**One suite withholds part of its work on purpose.** `vs_robustnesstest` exercises
`.rcc` parsing but does not read entry data for that format, because doing so
crashes the process — finding 1. `open()` is safe; only the read crashes, so the
parse path stays covered. A crash cannot be caught by QTest, so the alternative
would have been a suite that always aborts. The reason is recorded in the source
at `rccReadsEntryData()` rather than left as an unexplained gap.
