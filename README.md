# 2026_Analysis_Karchive

## About the project:

Analysis of an open-sourced project done for a course called Software Verification, as a part of Masters of Informatics degree on the <a href = "https://www.bg.edu.rs/">University of Belgrade</a>, <a href = "https://www.matf.bg.ac.rs/">Faculty of Mathematics</a>.

Project that was analysed: <a href = "https://github.com/KDE/karchive">Karchive</a> from <a href = "https://kde.org/">KDE</a><br>
Branch of project: master<br>
Specific hash commit: <a href = "https://github.com/KDE/karchive/commit/633dc0960438ef3a7286b8c5b8d6549cad532889">633dc09</a><br>
Author: Luka Stanković (<a href = "https://github.com/lukastan">lukastan</a>)<br>

KArchive is a KDE Frameworks library for reading and writing archives (tar, zip,
7z, ar, rcc) and for transparent (de)compression (gzip, bzip2, xz, zstd) behind
a single `KArchive` API. It is a pure library that parses **untrusted binary
input**, which is the property that shaped this whole analysis: the effort is
weighted toward how the parsers behave on malformed archives, because that is
where a library like this is actually at risk — and where KArchive's own recent
commit history sits (two of the five commits before the analysed revision are
*"7z: Fix infinite loop in malformed file"*).

The analysed project is included as a **git submodule** under
[`vendor/karchive`](vendor/karchive), pinned to commit `633dc09`. KArchive is
**not modified** by this analysis: every tool that needs instrumentation
(coverage, sanitizers, fuzzing) gets it injected from the wrapper build, so the
analysed source stays byte-identical to upstream and there is no `custom.patch`.

## Building

Prerequisites that stock Ubuntu 24.04 does **not** satisfy, and which must be in
place before anything below will configure:

- **extra-cmake-modules ≥ 6.21** — Ubuntu ships 5.115.0, so a newer ECM must be
  installed from source (it lands in `/usr/local/share/ECM`, which CMake finds).
- **Qt ≥ 6.8** with the `Core`, `Test` and `Network` modules.

Then, from the repository root:

```bash
git submodule update --init            # fetch vendor/karchive at 633dc09
cmake -S . -B build -G Ninja
cmake --build build
```

The remaining apt packages used by individual tools: `clang` / `clang-tools`,
`afl++`, `cppcheck`, `lcov`, `gcovr`, `heaptrack`, `valgrind`.

## Tools used:

Each tool lives in its own directory with a `run_*.sh` reproduction script and a
`TRIAGE.md` / write-up. Six tools were applied, satisfying the course rule of at
least six with at least two not covered in the exercises (⭐).

| # | Tool | Directory | Reproduce | What it found |
|---|---|---|---|---|
| 1 | **Unit/integration tests + coverage** (QTest, gcov/lcov/gcovr) | [`unit_tests/`](unit_tests) | `./unit_tests/run_tests.sh` | branch coverage 66.3% → 68.1%; findings 1, 2 |
| 2 | **clang-tidy** (style/static-checker slot) | [`clang_tidy/`](clang_tidy) | `./clang_tidy/run_clang_tidy.sh` | 497 findings triaged; config was silently misconfigured |
| 3 | **cppcheck** ⭐ | [`cppcheck/`](cppcheck) | `./cppcheck/run_cppcheck.sh` | independent static engine; 1 confirmed lifetime defect |
| 4 | **AddressSanitizer + UBSan** | [`sanitizers/`](sanitizers) | `./sanitizers/run_sanitizers.sh` | clean on the corpus; confirmed finding 1 at instruction level |
| 5 | **Valgrind memcheck** (Valgrind slot) | [`valgrind/memcheck/`](valgrind/memcheck) | `./valgrind/memcheck/run_memcheck.sh` | finding 3 (uninitialised memory ASan cannot see) |
| 6 | **AFL++** ⭐ | [`afl/`](afl) | `./afl/build_fuzzers.sh` then `./afl/run_campaign.sh k7z 300` | findings 5, 6 |
| + | **heaptrack** ⭐ (extra, heap profiler) | [`heaptrack/`](heaptrack) | `./heaptrack/run_heaptrack.sh` | finding 4 (memory scaling) |

Coverage tracking is mandatory and is part of tool 1. Only one Valgrind tool
(memcheck) and one style checker (clang-tidy) are counted, per the rules. KLEE
and CBMC were considered and deliberately rejected — KArchive is inseparable
from Qt and four C compression libraries, so both would need extensive stubbing
for little return; the reasoning is in [`ProjectAnalysisReport.md`](ProjectAnalysisReport.md).

> ⚠️ **Memory warning for the fuzzer.** Finding 6 is a resource-amplification
> bug: a 153-byte malformed 7z can drive K7Zip to allocate 8–12 GB of RAM.
> `run_campaign.sh` caps this, but when reproducing a saved crash or hang by
> hand, always bound it:
> `ASAN_OPTIONS=hard_rss_limit_mb=2048 timeout 10 <fuzzer> <file>`.

## Key takeaways:

The analysis found **six distinct, independently reproducible defects** in
KArchive at `633dc09`, all but one concentrated in the 7z code (`k7zip.cpp`), the
largest and least-validated parser in the library. Full write-ups are in
[`unit_tests/FINDINGS.md`](unit_tests/FINDINGS.md); the tool-by-tool reasoning is
in [`ProjectAnalysisReport.md`](ProjectAnalysisReport.md).

| # | Component | Defect | Found by |
|---|---|---|---|
| 1 | `KRcc` | crash (SIGBUS/SIGSEGV) on a corrupted `.rcc` — one flipped byte | tests; confirmed by ASan |
| 2 | `K7Zip` | corrupts every non-ASCII filename; writes archives it cannot reopen | tests |
| 3 | `KZstdFilter` | uninitialised-memory use on an empty archive | memcheck |
| 4 | `K7Zip` | write path uses 2.84 GB of heap for a 64 MiB archive | heaptrack |
| 5 | `K7Zip` | out-of-bounds read on a malformed coder stream graph | AFL++ |
| 6 | `K7Zip` | resource-amplification DoS — 153 bytes → >12 GB RAM | AFL++ |

Two themes stand out. First, **each tool found what the others structurally
could not**: cppcheck's value-flow analysis found a lifetime bug clang-tidy's
type analysis missed; memcheck found uninitialised memory ASan cannot track;
heaptrack answered a question no memory checker asks; and AFL++ reached parser
code no static corpus exercised. Second, the tools **corroborated each other** —
every one that measures per-file distribution pointed at `k7zip.cpp` as the
weakest spot, which is independently where upstream keeps fixing bugs.

A methodological note worth its own mention: two of the "findings" were about the
tooling itself — `clang-tidy` was misconfigured so that its command line silently
overrode the project's own check selection, and `lcov --list` is broken in the
version Ubuntu 24.04 ships. Both are documented, because getting a tool to report
the truth is part of the analysis.

## License:
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.<br>
This project contains KArchive as a submodule, which is licensed under the LGPL.
