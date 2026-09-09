# cppcheck — configuration, results and triage

Tool: cppcheck 2.13.0
Target: KArchive at commit `633dc09`, 15 translation units from `vendor/karchive/src`
Reproduce: `./cppcheck/run_cppcheck.sh`
Raw output: [`cppcheck_report.txt`](cppcheck_report.txt) ·
[`cppcheck_report.xml`](cppcheck_report.xml) · counts: [`summary.txt`](summary.txt)

cppcheck is one of the two tools in this analysis **not covered in the course exercises**.

---

## 1. Why a second static analyser at all

clang-tidy is a Clang frontend: it builds a real AST with full type information and runs
matchers over it. cppcheck deliberately is not. It does its own parse and then performs
value-flow analysis, propagating possible values along execution paths. The two therefore
fail and succeed in different places, and running both lets each act as a control on the
other. Their overlap on this codebase turned out to be **almost nil** (§5), which is the
clearest possible justification for the choice.

## 2. How it was run

- `--project=` a **filtered** `compile_commands.json`. Using the real compile database
  gives cppcheck the exact include paths and preprocessor defines. The filter drops moc
  and autogen output and ECM's `ECMQmLoader`, none of which are written by the KArchive
  authors.
- `--library=qt` — teaches cppcheck Qt's container and ownership semantics. Without it,
  Qt's implicit sharing produces a large volume of false allocation findings.
- `--check-level=exhaustive` — deeper value-flow analysis than the default.
- `--inconclusive` — also report findings cppcheck cannot fully prove. These are marked
  `inconclusive="true"` in the XML and are triaged separately rather than trusted.
- Suppressed: `unusedFunction` (KArchive is a library — its public API is unused *within*
  the project by definition, so every exported symbol would be reported) and
  `missingIncludeSystem` (reports only that system headers were not parsed, which is
  expected and intended).

## 3. Results

**91 findings** — 3 `error`, 33 `warning`, 55 `style`; 12 marked inconclusive.

| Count | Check id | Severity |
|---:|---|---|
| 21 | `cstyleCast` | style |
| 12 | `funcArgNamesDifferent` | style |
| 11 | `noCopyConstructor` | warning |
| 11 | `noOperatorEq` | warning |
| 10 | `duplInheritedMember` | style |
| 4 | `variableScope` | style |
| 4 | `shadowVariable` | style |
| 4 | `unreadVariable` | style |
| 3 | `noExplicitConstructor` | style |
| 3 | `constVariablePointer` | style |
| 2 | `constParameterPointer` | style |
| 2 | `preprocessorErrorDirective` | **error** |
| 1 | `danglingLifetime` | **error** |
| 1 | `knownConditionTrueFalse` | style |
| 1 | `shadowFunction` | style |
| 1 | `uselessAssignmentPtrArg` | warning |

Distribution again tracks parser complexity: `k7zip.cpp` 28, `karchive.cpp` 19,
`kzip.cpp` 16, `ktar.cpp` 5.

## 4. Triage of the significant findings

### 4.1 `danglingLifetime` — `k7zip.cpp:2630` — **CONFIRMED, latent**

> Non-local variable `d->buffer` will use object that points to local variable `inBuffer`.

This is cppcheck's best finding on this codebase and it is correct.

In `K7Zip::openArchive` (line 2556):

```cpp
QByteArray inBuffer;                 // function-local
inBuffer.resize(nextHeaderSize);
n = dev->read(inBuffer.data(), inBuffer.size());
...
d->buffer = inBuffer.data();         // member points into a local's heap buffer
```

`d->buffer` is `const char *buffer;`, a member of `K7ZipPrivate` (line 472) that outlives
the function. The same pattern repeats at line 2660 with the local `decodedData`. When
`openArchive` returns, both `QByteArray`s are destroyed and `d->buffer` dangles.

Tracing every dereference: `d->buffer` is read only at line 658, inside
`K7ZipPrivate::readByte()` and its siblings (`readUInt64`, `readNumber`), which are called
only from the header-parsing chain that runs *inside* `openArchive`. So at present every
dereference happens while the local is still alive.

**It is not currently exploitable, and it is not cleaned up either.** `K7ZipPrivate::clear()`
does reset `buffer = nullptr` (line 508), but the only caller is `closeArchive()` at line
3047 — which returns early for read-only archives:

```cpp
if ((mode() == QIODevice::ReadOnly)) {
    return true;                     // never reaches d->clear()
}
```

Reading an archive is exactly the untrusted-input path. So for the entire lifetime of a
read-only `K7Zip` object after `open()`, the object holds a pointer to freed memory.

**Verdict: a latent use-after-free.** Not reachable today, one refactor away from being
reachable, and in precisely the parser with upstream's recent infinite-loop fixes. The
correct remedy is to store the header in a member `QByteArray` rather than a raw pointer
to a local's buffer.

### 4.2 `preprocessorErrorDirective` ×2 — **false positives**

```
moc_kcompressiondevice.cpp:19: error: #error "The header file 'kcompressiondevice.h'
                                     doesn't include <QObject>."
```

These fire in Qt's *generated* moc files. moc emits a deliberate `#error` guarded by a
preprocessor condition that is false in a real compile; cppcheck evaluates the
configuration differently and reports the guard as if it had triggered. The files are
generated, not authored, and both compile cleanly.

Two of the three `error`-severity findings are therefore noise — a useful reminder that
severity labels are the tool's opinion, not a triage.

### 4.3 `knownConditionTrueFalse` — `karchive.cpp:319` — **false positive (configuration-dependent)**

> Condition `symLinkTarget.isEmpty()` is always true

`symLinkTarget` is assigned at line 316 inside a platform `#if` block that calls
`readlink()`. cppcheck analysed a preprocessor configuration in which that block is
excluded, so in *its* configuration nothing ever assigns the variable. On Linux the branch
is compiled and can assign a non-empty value.

A good illustration of the trade-off behind cppcheck's design: not being a full compiler
frontend is what lets it run without a build and explore multiple configurations, and it
is also what produces this class of false positive.

### 4.4 `uselessAssignmentPtrArg` — `kzip.cpp:180` — **real, benign**

`buffer += 4;` is the last statement affecting `buffer` in `parseExtTimestamp`, and
`buffer` is a by-value `const char *` parameter, so the assignment cannot be observed by
the caller. A genuine dead store. It is harmless, and arguably deliberate: the function
advances `buffer`/`size` uniformly after each field, so the final redundant advance keeps
the pattern regular and stays correct if another field is appended later. **No change
recommended.**

### 4.5 `shadowFunction` — `karchive.cpp:380` — **real, cosmetic**

Local `const QString fileName` shadows the member function `fileName()`. Legal, compiles
correctly, mildly confusing. Cosmetic.

### 4.6 The `noCopyConstructor` / `noOperatorEq` cluster — 22 findings, **by design**

Every `KArchive` subclass and `*Private` class is reported as owning a raw pointer without
a copy constructor or assignment operator. This is the **d-pointer idiom**, which KDE uses
throughout: the classes are deliberately non-copyable QObject-like types. Not defects.
Reported as a cluster rather than 22 entries.

## 5. Comparison with clang-tidy — the reason for running both

| | clang-tidy | cppcheck |
|---|---|---|
| Findings | 497 | 91 |
| Confirmed defects | 0 | **1** (`danglingLifetime`) |
| False positives found by hand-checking | 11/11 integer-overflow findings | 3 of 3 `error`s |
| Headline class | 140 narrowing conversions (systemic) | object-lifetime and API-shape issues |

**The overlap is essentially nil.** Not one finding appears in both reports. They divide
cleanly:

- clang-tidy, with full type information, sees **type-level** issues — narrowing,
  widening, signedness — and found none that were individually exploitable.
- cppcheck, with value-flow across paths, sees **lifetime and dataflow** issues, and found
  the one confirmed defect in the analysis so far: a member pointer outliving the buffer
  it points into.

Neither tool found the heap over-read in `decryptAES` described below; both, however,
concentrated their findings in `k7zip.cpp`, which is where it lives.

## 6. Defects found by review of the code the analysers pointed at

Recorded here so that all confirmed defects are in one place. Neither analyser reported
these; both directed attention to `k7zip.cpp`, and `clang_tidy/TRIAGE.md` §4.3 explains
how investigating one of clang-tidy's false positives led into `decryptAES`
(`k7zip.cpp:1357`).

### 6.1 Heap buffer over-read in `decryptAES` — **confirmed by reading; not yet runtime-proven**

The call site guards the properties array:

```cpp
case k_AES: {
    if (coder->properties.size() >= 2) {          // only 2 bytes guaranteed
        ...
        decryptAES(coder->properties, password, deflatedData);
```

but the callee indexes far beyond that, using sizes taken from the archive itself:

```cpp
int saltSize = ((firstByte >> 7) & 1) + (coderProperties[1] >> 4);   // 0..16
int ivSize   = ((firstByte >> 6) & 1) + (coderProperties[1] & 0x0F); // 0..16

QByteArray salt((const char *)coderProperties.data() + 2, saltSize);
QByteArray iv((const char *)coderProperties.data() + 2 + saltSize, ivSize);
```

The required length is `2 + saltSize + ivSize`, up to **34 bytes**, while only 2 are
checked. A crafted 7z archive declaring an AES coder with a 2-byte properties array and
both nibbles of byte 1 set causes `QByteArray` to copy up to 32 bytes past the end of the
allocation.

Preconditions: the archive declares a `k_AES` coder, KArchive was built with OpenSSL (it
is by default, and is on this machine), and the caller supplied a **non-empty password** —
`decryptAES` is not reached otherwise.

### 6.2 Undefined behaviour in `calculateKey` — same call path

```cpp
quint32 numCyclesPower = firstByte & 0x3F;        // 0..63, straight from the archive
...
stages = 1 << (numCyclesPower - catCycle);        // catCycle == 6
```

For `numCyclesPower` ≥ 38 this shifts an `int` by 32–57 bits. Shifting by more than the
operand's width is undefined behaviour in C++. Even where it happens to produce a large
value, `stages` then drives `for (quint32 i = 0; i < stages; i++)` around an expensive
SHA-256 loop — the same denial-of-service shape as the malformed-input infinite loops
upstream has been fixing.

### 6.3 Status and next step

Both are **confirmed by code reading only**. Neither has been demonstrated at runtime yet,
and this document does not claim otherwise. Proving them is a specific, concrete task for
the later stages:

- **UBSan** should report §6.2 immediately on any archive with `numCyclesPower ≥ 38`.
- **ASan** should report §6.1 as a heap-buffer-overflow READ.
- **AFL++** needs a seed archive with an AES coder plus a non-empty password to reach the
  path at all — a targeted seed, not something a generic corpus will stumble into.

If they reproduce, they are reportable upstream. Until then they are described here as
what they are: strongly-argued reads of the source, awaiting runtime confirmation.
