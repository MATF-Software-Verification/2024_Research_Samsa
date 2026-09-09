# clang-tidy — configuration, results and triage

Tool: clang-tidy 18.1.3
Target: KArchive at commit `633dc09`, `vendor/karchive/src/*.cpp` (14 translation
units; the ECM-generated `ECMQmLoader` is excluded)
Reproduce: `./clang_tidy/run_clang_tidy.sh`
Raw output: [`clang_tidy_report.txt`](clang_tidy_report.txt) · counts: [`summary.txt`](summary.txt)

clang-tidy occupies the single "code formatting/style checker" slot permitted by the
course rules, so clang-format is deliberately not also counted.

---

## 1. Two configuration defects fixed before trusting any result

**The check selection never actually ran.** The original `run_clang_tidy.sh` passed
`-checks='-*,bugprone-*'` on the command line, which *overrides* the `.clang-tidy` file
rather than adding to it. So although `.clang-tidy` requested `modernize-*`,
`performance-*` and three `readability-*` checks, only `bugprone-*` was ever applied.
The command-line override is gone; `.clang-tidy` is now the single source of truth.

**Generated headers dominated the output.** `HeaderFilterRegex` was `.*`, so warnings
from build-directory headers (`karchive_export.h`, `karchive_version.h`,
`config-compression.h`, moc output) were reported as if they were KArchive defects. They
are ECM's code generator's output, not this project's code.

Scoping this is harder than it looks: a CMake build directory mirrors the source layout,
so the generated headers live in `<build>/vendor/karchive/src/` and match any pattern
written against `vendor/karchive/src`. clang-tidy 18 has no negative filter
(`ExcludeHeaderFilterRegex` arrived in 19), so the script passes `--header-filter`
anchored at the *absolute* source path. That narrows paths only — it never changes which
checks run, which is precisely the mistake being corrected above.

## 2. Two checks disabled, on measured evidence

Both were measured before being switched off, not assumed:

| Check | Findings | Why disabled |
|---|---|---|
| `modernize-use-trailing-return-type` | **457 of 1011 (45%)** | Pure stylistic preference for `auto f() -> int`. No defect risk, and KDE's own style does not use it. It alone was burying every finding that matters. |
| `bugprone-easily-swappable-parameters` | 22 | Flags any two adjacent same-typed parameters. A design smell, not a defect, and unactionable without changing public API. |

Net effect: **1011 → 497 findings**, and the top entry became
`bugprone-narrowing-conversions` instead of a formatting preference.

## 3. Results

497 findings, all severity `warning`.

| Count | Check | Class |
|---:|---|---|
| 140 | `bugprone-narrowing-conversions` | **integer safety** |
| 82 | `modernize-use-nodiscard` | API hygiene |
| 62 | `modernize-use-default-member-init` | style |
| 29 | `modernize-macro-to-enum` | style |
| 27 | `modernize-avoid-c-arrays` | style |
| 27 | `readability-named-parameter` | style |
| 23 | `modernize-loop-convert` | style |
| 20 | `modernize-return-braced-init-list` | style |
| 20 | `modernize-use-auto` | style |
| 13 | `modernize-deprecated-headers` | style |
| 11 | `bugprone-implicit-widening-of-multiplication-result` | **integer safety** |
| 8 | `performance-enum-size` | performance |
| 8 | `modernize-pass-by-value` | performance |
| 6 | `bugprone-reserved-identifier` | correctness |
| 5 | `modernize-use-equals-default` | style |
| 3 | `bugprone-macro-parentheses` | correctness |
| 3 | `bugprone-signed-char-misuse` | **integer safety** |
| 3 | `bugprone-suspicious-string-compare` | correctness |
| 2 | `bugprone-switch-missing-default-case` | correctness |
| 2 | `bugprone-branch-clone` | correctness |
| 2 | `bugprone-suspicious-include` | correctness |
| 1 | `bugprone-assignment-in-if-condition` | correctness |

Concentration follows parser complexity almost exactly — `k7zip.cpp` 197, `kzip.cpp` 59,
`ktar.cpp` 32, `karchive.cpp` 25. The 7z parser is 3330 LOC and carries 40% of all
findings.

## 4. Triage of the integer-safety findings

These are the findings that matter for this target: KArchive parses attacker-controlled
binary headers, and its upstream bug history is malformed-archive handling (two of the
five commits before our pin are *"7z: Fix infinite loop in malformed file"*).

### 4.1 `bugprone-narrowing-conversions` — 140, systemic

Overwhelmingly `quint64`/`qsizetype` values read from archive headers being assigned to
`int`. Examples in `k7zip.cpp` at 310, 401, 714, 776, 812, 813. Distribution: k7zip 68,
kzip 35, ktar 11, filters 18, karchive 4.

This is a **systemic pattern rather than 140 separate bugs**: KArchive's internal APIs use
`int` for sizes and offsets while both Qt and the file formats use 64-bit types. Each site
is individually harmless where the value is bounded by an earlier check, and individually
dangerous where it is not. Auditing all 140 by hand is out of proportion; the right
response is to let the fuzzing stage decide which ones are reachable with hostile values.
**Carried forward as targeting information for AFL++.**

### 4.2 `bugprone-implicit-widening-of-multiplication-result` — 11, verified individually

All 11 were checked by reading the code. **All are benign**, for two distinct reasons:

- `k7zip.cpp:2732` — `static const qsizetype MAX_FILE_NUMBER = 1000 * 1000 * 1000;`
  computed in `int`. 10⁹ fits in a 32-bit `int` (max ≈2.1×10⁹), so no overflow occurs.
  The *pattern* is fragile — one more factor of ten would overflow silently — but the code
  as written is correct.
- `k7zip.cpp:1164` — calendar arithmetic (`5 * DAYSPERNORMALQUADRENNIUM` = 7305) with
  compile-time-bounded constants.
- The remaining nine are buffer-size arithmetic on values already bounded by prior checks.

**Verdict: 11 false positives.** Worth stating plainly — a static analyser reporting an
unproven risk is doing its job, and confirming it is unfounded is a result.

### 4.3 The most serious issue was found *because of* a false positive

`k7zip.cpp:1337` was flagged as an overflowing multiplication:
`result.reserve(saltPassword.size() + rounds * 8)`. Reading `calculateKey` shows `rounds`
is capped at `1 << 6` = 64, so `rounds * 8` ≤ 512 — **the finding is a false positive**.

But the surrounding function, `decryptAES` (`k7zip.cpp:1357`), turned out to contain two
real defects that neither analyser reported. Both are documented in
[`../cppcheck/TRIAGE.md` §4](../cppcheck/TRIAGE.md) alongside cppcheck's own findings so
that all confirmed defects sit in one place; in summary:

1. **Heap buffer over-read.** The caller guards with `coder->properties.size() >= 2`, but
   `salt` and `iv` are constructed from offsets up to `2 + saltSize + ivSize`, which the
   archive controls and which reaches 34. A 2-byte properties array therefore reads up to
   32 bytes past the end.
2. **Undefined behaviour.** `stages = 1 << (numCyclesPower - catCycle)` where
   `numCyclesPower = firstByte & 0x3F` ≤ 63, shifting an `int` by up to 57 bits.

This is the honest characterisation, and it is worth being explicit about at the defense:
**clang-tidy did not find these — it directed attention to the right function, and reading
the code found them.** That is a real and typical mode of value for static analysis, and
it is a better story than pretending the tool reported them.

## 5. Conclusions

- The tool was **misconfigured in two ways that silently suppressed most of its value**;
  correcting that mattered more than any single finding.
- The `bugprone-*` findings are heavily concentrated in `k7zip.cpp`, which is both the
  largest parser and the one with upstream's recent bug history — an independent
  corroboration that attention belongs there.
- Every finding in the two integer-overflow categories that could be checked by hand
  turned out to be **benign**; the value came from the audit the warnings prompted.
- The 140 narrowing conversions are a systemic API-design issue. They are handed to the
  fuzzing stage rather than triaged by hand.
