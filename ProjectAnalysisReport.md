# Project Analysis Report — KArchive

**Course:** Verifikacija softvera (Software Verification), Master of Informatics,
Faculty of Mathematics, University of Belgrade.
**Author:** Luka Stanković ([lukastan](https://github.com/lukastan)).
**Analysed project:** [KDE KArchive](https://github.com/KDE/karchive), branch
`master`, commit [`633dc09`](https://github.com/KDE/karchive/commit/633dc0960438ef3a7286b8c5b8d6549cad532889).

This report describes the analysis and its conclusions. Per-tool detail lives in
each tool's `TRIAGE.md`; every defect is written up in
[`unit_tests/FINDINGS.md`](unit_tests/FINDINGS.md); this document ties them
together and explains the choices behind them.

---

## 1. The target and what shaped the analysis

KArchive is a KDE Frameworks C++ library providing a uniform API over archive
formats (tar, zip, 7z, ar, Qt rcc) and compression filters (gzip, bzip2, xz,
zstd, none). It is ~9.5k lines of library source, built with CMake + KDE's Extra
CMake Modules, depending on Qt 6.8+ and on the four C compression libraries.

Five properties of the target drove every decision:

1. **It is a library, not a program.** There is no `main()`, so every dynamic
   tool needs a driver we write. This is why the test suite is not just one item
   of six but the *harness the other tools run on*.
2. **It parses untrusted binary input.** The archive parsers consume
   attacker-controllable byte streams. This is the canonical domain for fuzzing,
   sanitizers and memory analysis.
3. **Its real bug history is exactly that.** Two of the five commits before the
   analysed revision are *"7z: Fix infinite loop in malformed file"*, and
   upstream carries an OSS-Fuzz integration and a `KArchiveFiledataDEFENSE`
   branch. The threat model is upstream's own.
4. **It wraps C compression libraries.** Buffer ownership crosses the C/C++
   boundary on every filter, and those system libraries are not instrumented
   when we build — which directly determines the ASan-vs-memcheck split (§4).
5. **It is already fuzzed.** KArchive is in Google's OSS-Fuzz with harnesses
   shipped in `autotests/ossfuzz`. Novel crashes were therefore not the
   expectation; reach, reproducibility and methodology were.

The consequence, stated up front so the tool choices make sense: the analysis is
weighted toward **robustness against malformed input**, not toward concurrency
(KArchive is single-threaded) or UI responsiveness (there is none).

## 2. Methodology

The tools were not run as an independent checklist; they form a pipeline where
each stage feeds the next, with two feedback loops that are the point of the
whole exercise:

```
STATIC (clang-tidy, cppcheck)  ── suspicious functions ─┐
                                                         v
TESTS + COVERAGE ── baseline shows untested branches ── writes targeted tests
        │  the test binary is the driver for everything below   ^
        v                                                        │
DYNAMIC (ASan/UBSan, memcheck, heaptrack) ── errors on valid input
        │  uncovered regions                                     │
        v                                                        │
FUZZING (AFL++) ── crashes/hangs ── minimise ── new test cases ──┘
```

Coverage aims the fuzzer; the fuzzer's crashes become test cases. That is what
makes this an *analysis* rather than seven disconnected runs.

**One reproducible build underpins all of it.** A thin wrapper `CMakeLists.txt`
builds the pinned submodule together with our tests, and injects instrumentation
(`--coverage`, `-fsanitize=…`, AFL) from the parent scope *before*
`add_subdirectory(vendor/karchive)`. That reaches KArchive's own sources without
editing them — verified: the coverage build emits `.gcno` for all 15 library
sources and the submodule is byte-identical to upstream afterward. This is why
the project ships **no `custom.patch`**, which is a stronger position than
having one.

## 3. Tool selection and the rules

The course requires **≥ 6 tools**, with tests-plus-coverage counting as one
(coverage mandatory), **at most one Valgrind tool**, **at most one style
checker**, and **≥ 2 tools not covered in the exercises**. The exercises covered,
among others: QtTest, lcov, libFuzzer, the Valgrind suite, perf, KLEE, CBMC and
Clang. So the "not covered" credit had to come from elsewhere.

| Tool | Rule slot | Not covered? |
|---|---|---|
| QTest suite + gcov/lcov/gcovr | the mandatory tests+coverage item | — |
| clang-tidy | the one style/static-checker slot | — |
| cppcheck | — | ⭐ |
| ASan + UBSan | one dynamic-analysis item | — |
| AFL++ | — | ⭐ |
| Valgrind memcheck | the one Valgrind slot | — |
| heaptrack (7th, for margin) | — | ⭐ |

Three "not covered" tools where two are required — deliberate margin, in case a
grader argues cppcheck counts against the style-checker limit.

**Rejected, and why it matters to be able to say so:** *KLEE* and *CBMC* were
considered and dropped. Both want self-contained code they can turn into
whole-program bitcode or bounded models; KArchive is inseparable from Qt's
`QIODevice`/`QByteArray`/meta-object machinery and links four C libraries, so
either would need weeks of stubbing to say more about the stubs than about
KArchive — and AFL++ reaches the same malformed-input bug class far more cheaply
on this target. *libFuzzer* is superseded by AFL++ here but its harnesses are
reused. *callgrind/massif* are blocked by the one-Valgrind rule; heaptrack covers
heap profiling instead, and being non-Valgrind it costs no slot.

## 4. The two most consequential choices

**memcheck over callgrind/massif for the Valgrind slot.** ASan only sees code it
recompiled, so it is blind inside the uninstrumented system compression
libraries, and it cannot detect uninitialised-memory reads at all. memcheck needs
no recompilation and does both. That is not a theoretical distinction: memcheck
found **finding 3** — an uninitialised `ZSTD_inBuffer.src` read inside libzstd —
on a codebase where ASan reported nothing. The two tools were deployed for
different jobs (ASan as the always-on fuzzing runtime; memcheck as a deep offline
pass), not redundantly.

**AFL++ over libFuzzer.** Besides not being covered in the exercises, AFL++ has a
hang/timeout detector, which is exactly matched to KArchive's documented
malformed-input bug class — and it is what surfaced **finding 6**, the
amplification DoS, as "hangs". A subtlety worth recording: Ubuntu's `afl++` is
built against LLVM 17 while the rest of the toolchain is 18, so the fuzz build
links AFL++'s `libAFLDriver.a` rather than LLVM 18's libFuzzer runtime, keeping
the whole fuzz build clang-17 and avoiding a cross-version link.

## 5. Findings

Six distinct defects, all reproduced deterministically against unmodified
`633dc09`. Full detail and reproducers are in `unit_tests/FINDINGS.md`.

| # | Component | Defect | Severity | Found by | Corroborated by |
|---|---|---|---|---|---|
| 1 | `KRcc` | crash (SIGBUS/SIGSEGV) on corrupted `.rcc`, one byte | high (crash on untrusted input) | tests | ASan (instruction-level) |
| 2 | `K7Zip` | corrupts non-ASCII filenames; unreadable archives | high (silent data loss) | tests | — |
| 3 | `KZstdFilter` | uninitialised-memory use on empty archive | low | memcheck | — |
| 4 | `K7Zip` | 2.84 GB heap to write 64 MiB | medium (scalability) | heaptrack | — |
| 5 | `K7Zip` | out-of-bounds read on malformed stream graph | high (memory safety) | AFL++ | ASan |
| 6 | `K7Zip` | amplification DoS, 153 B → >12 GB RAM | high (DoS) | AFL++ | — |

Selected root causes (the rest are in the findings doc):

- **#1** — `KRcc::openArchive` hands an untrusted file to
  `QResource::registerResource`, which memory-maps it and trusts its internal
  offsets; a corrupted offset makes `KRccFileEntry::data()` read outside the
  mapping (SIGBUS is the tell). KArchive routes untrusted input into an API
  designed for trusted, compiled-in resources.
- **#2** — the 7z name writer funnels each character through
  `QChar::toLatin1()` (k7zip.cpp:2480), which returns a *signed* char (so U+00E9
  → U+FFE9 via sign-extension) and `'\0'` above U+00FF (7z's terminator, so the
  archive becomes unreadable). The correct `QStringEncoder(Utf16LE)` idiom is
  already used elsewhere in the same file.
- **#5** and **#6** — both in the coder stream-graph and header parsing:
  `findInStream` falls through without signalling failure (OOB read), and header
  count fields read as 64-bit numbers size containers with no bound against the
  input length (`reserve(numCoders)`, DoS).

## 6. Coverage

Upstream's own autotests already reach **82.8% line coverage**, so adding line
coverage was never the useful contribution; **branch coverage was 66.3%**, and
the uncovered branches are the error paths taken on damaged input. Our two suites
(228 cases: truncation, byte-corruption, degenerate input, and write/read round
trips) target those branches. Measured as a baseline-vs-combined delta:

```
              baseline   combined    delta
lines           82.8%      84.1%    +1.3 pp
branches        66.3%      68.1%    +1.8 pp  (+49 branches)
```

The gains land where aimed — `kar.cpp` +10 pp and `krcc.cpp` +6 pp branch
coverage, the two weakest parsers — and `krcc.cpp` having the lowest coverage
(37%) is not unrelated to its being where the crash (finding 1) lives. The
headline percentages are modest, and that is the honest result against an
already well-tested library; the real yield of the tests item is findings 1 and
2, not the coverage number.

## 7. Honesty notes

Getting a tool to tell the truth is part of the work, so two tooling problems and
one unproven claim are recorded rather than hidden:

- **clang-tidy was misconfigured.** Its run script passed `-checks=` on the
  command line, which *overrides* the project `.clang-tidy` rather than adding to
  it, so the `modernize-*`/`performance-*`/`readability-*` checks configured
  there had never run. Fixed; a single 45%-of-output style check
  (`use-trailing-return-type`) was then disabled on measured evidence, taking the
  signal from 1011 findings to 497 with `bugprone-narrowing-conversions` on top.
- **`lcov --list` is broken in lcov 2.0-1** (Ubuntu 24.04): it reported 11.5%
  line / 0.0% function coverage for data `genhtml` and `gcovr` both read as
  ~82%/~89%. The impossible non-zero-lines-with-zero-functions combination gave
  it away; all coverage numbers come from gcovr.
- **The `decryptAES` static findings are not fuzzer-confirmed.** cppcheck's and
  clang-tidy's investigation of `decryptAES` surfaced a heap over-read and a
  shift-UB by source reading (`cppcheck/TRIAGE.md` §6), but reaching that path
  needs a validly-structured AES coder the fuzzer did not assemble in the
  campaign budget. They remain source-confirmed only; a crafted AES-header seed
  is saved (`afl/seeds/aes_header.7z`) for a longer run. This report does not
  claim them as runtime-proven.

## 8. Conclusion

KArchive is a mature, already-fuzzed library, and the analysis reflects that: no
tool found a problem in the common tar/zip read paths, and the sanitizer suite is
clean on the whole corpus. The defects are concentrated in **`k7zip.cpp`** — the
largest parser, with the weakest validation and the most complex on-disk format —
across correctness (filename corruption), memory safety (OOB read, uninitialised
use) and resource safety (write-side memory blow-up, read-side amplification
DoS). That every distribution-measuring tool independently pointed at the same
file, and that it is where upstream keeps landing fixes, is the analysis's
central corroborated result.

The broader methodological takeaway is that the tools are complementary by
construction, not interchangeable: each of the six found something the others
structurally could not, and the value came as much from that division of labour —
and from correcting the tools when they lied — as from any single finding.
