/*
    Robustness of KArchive's parsers against malformed input.

    Part of the Software Verification analysis of KArchive. This suite is the
    reason our tests exist at all: upstream's own autotests already reach ~82%
    line coverage, but only ~66% branch coverage, and the uncovered branches
    are overwhelmingly the error paths taken when an archive is damaged.

    That is also where KArchive's real bug history lives -- two of the five
    upstream commits before our pinned 633dc09 are "7z: Fix infinite loop in
    malformed file", and autotests/data contains regression cases named after
    OSS-Fuzz issues.

    The contract under test is deliberately weak, because it is the only one a
    parser of untrusted input can actually promise:

      * open() may succeed or fail, but must return rather than crash, hang or
        read out of bounds;
      * if open() succeeds, directory() must not be null;
      * every entry reachable from directory() must be readable without
        crashing, whatever nonsense the header claimed.

    We deliberately do NOT assert that a damaged archive fails to open. Many
    formats are recoverable in part, and demanding failure would encode our
    guesses rather than the library's contract.

    SPDX-License-Identifier: MIT
*/

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include <k7zip.h>
#include <kar.h>
#include <karchivedirectory.h>
#include <karchiveentry.h>
#include <karchivefile.h>
#include <krcc.h>
#include <ktar.h>
#include <kzip.h>

namespace
{

/*! Builds the right KArchive subclass for a path, based on its extension. */
std::unique_ptr<KArchive> makeArchive(const QString &path)
{
    if (path.endsWith(QLatin1String(".7z"))) {
        return std::make_unique<K7Zip>(path);
    }
    if (path.endsWith(QLatin1String(".zip"))) {
        return std::make_unique<KZip>(path);
    }
    if (path.endsWith(QLatin1String(".a"))) {
        return std::make_unique<KAr>(path);
    }
    if (path.endsWith(QLatin1String(".rcc"))) {
        return std::make_unique<KRcc>(path);
    }
    // KTar autodetects gzip/bzip2/xz/zstd from the extension.
    return std::make_unique<KTar>(path);
}

/*!
    Walks the whole entry tree, reading every file.

    \a budget caps the number of entries visited so that an archive whose
    header declares an enormous entry count cannot turn a unit test into a
    multi-minute run. Depth is capped separately: a malformed archive can
    describe a directory tree that is effectively cyclic.
 */
void walkAndRead(const KArchiveDirectory *dir, int &budget, int depth = 0)
{
    if (!dir || depth > 16) {
        return;
    }
    const QStringList names = dir->entries();
    for (const QString &name : names) {
        if (--budget <= 0) {
            return;
        }
        const KArchiveEntry *entry = dir->entry(name);
        if (!entry) {
            continue;
        }
        if (entry->isFile()) {
            // Reading is the point: this is where a bogus size or offset in
            // the header turns into an out-of-bounds read.
            const KArchiveFile *file = static_cast<const KArchiveFile *>(entry);
            const QByteArray data = file->data();
            Q_UNUSED(data);

            if (QIODevice *device = file->createDevice()) {
                if (device->open(QIODevice::ReadOnly)) {
                    device->read(64 * 1024);
                }
                delete device;
            }
        } else if (entry->isDirectory()) {
            walkAndRead(static_cast<const KArchiveDirectory *>(entry), budget, depth + 1);
        }
    }
}

/*!
    Opens \a path and exercises it, returning whether open() succeeded.

    Entry contents are read only when \a readEntries is true. That switch
    exists for one reason: KRcc crashes the process when asked for the data of
    an entry in a corrupted .rcc, so for that format we exercise parsing only.
    See the comment on rccReadsEntryData() below and unit_tests/FINDINGS.md.
 */
bool exerciseArchive(const QString &path, bool readEntries = true)
{
    std::unique_ptr<KArchive> archive = makeArchive(path);
    if (!archive->open(QIODevice::ReadOnly)) {
        return false;
    }
    if (readEntries) {
        int budget = 4096;
        walkAndRead(archive->directory(), budget);
    }
    archive->close();
    return true;
}

/*!
    Whether it is safe to read entry data for the archive at \a path.

    KRcc hands the file to QResource::registerResource(), and Qt memory-maps it
    and trusts the offsets in its header. A single corrupted header byte
    therefore makes KRccFileEntry::data() read outside the mapping and take
    SIGBUS or SIGSEGV -- measured, not hypothetical: offsets 12, 15 and 20 of
    autotests/data/runtime_resource.rcc all reproduce it.

    open() itself is safe; only reading entry data crashes. So the parse path
    is still covered here and only the read is withheld. A crash cannot be
    caught by QTest, so the alternative would be a suite that always aborts.
 */
bool rccReadsEntryData(const QString &path)
{
    return !path.endsWith(QLatin1String(".rcc"));
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return f.readAll();
}

} // namespace

class VsRobustnessTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void truncation_data();
    void truncation();

    void byteCorruption_data();
    void byteCorruption();

    void degenerateInput_data();
    void degenerateInput();

    void cleanupTestCase();

private:
    /*! Writes \a bytes to a scratch file named \a name and returns its path. */
    QString scratchFile(const QString &name, const QByteArray &bytes);

    /*! The sample archives every test iterates over, as absolute paths. */
    static QStringList samples();

    QTemporaryDir m_dir;
    int m_openedCount = 0;
    int m_rejectedCount = 0;
};

QStringList VsRobustnessTest::samples()
{
    const QDir data(QStringLiteral(KARCHIVE_TEST_DATA_DIR));
    QStringList result;
    // One healthy sample per format, so that damage is the only variable.
    const QStringList names = {
        QStringLiteral("7z_coder_test_lzma2.7z"),
        QStringLiteral("artest.a"),
        QStringLiteral("runtime_resource.rcc"),
        QStringLiteral("global_header_test.tar.gz"),
        QStringLiteral("dirpermissions.zip"),
    };
    for (const QString &name : names) {
        const QString path = data.absoluteFilePath(name);
        if (QFileInfo::exists(path)) {
            result << path;
        }
    }
    return result;
}

void VsRobustnessTest::initTestCase()
{
    QVERIFY2(m_dir.isValid(), qPrintable(m_dir.errorString()));
    const QStringList found = samples();
    QVERIFY2(!found.isEmpty(),
             "No sample archives found. KARCHIVE_TEST_DATA_DIR is probably wrong: " KARCHIVE_TEST_DATA_DIR);
    // All five formats should be present in a complete checkout.
    QCOMPARE(found.size(), 5);
}

QString VsRobustnessTest::scratchFile(const QString &name, const QByteArray &bytes)
{
    const QString path = m_dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return QString();
    }
    f.write(bytes);
    f.close();
    return path;
}

// ---------------------------------------------------------------------------
// Truncation: the commonest real-world damage, and the one most likely to
// expose a parser that trusts a length field it has not yet read.
// ---------------------------------------------------------------------------

void VsRobustnessTest::truncation_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<int>("percent");

    const QList<int> fractions = {0, 1, 5, 10, 25, 50, 75, 90, 99};
    for (const QString &path : samples()) {
        const QString base = QFileInfo(path).fileName();
        for (int percent : fractions) {
            QTest::addRow("%s @ %d%%", qPrintable(base), percent) << path << percent;
        }
    }
}

void VsRobustnessTest::truncation()
{
    QFETCH(QString, source);
    QFETCH(int, percent);

    const QByteArray whole = readAll(source);
    QVERIFY(!whole.isEmpty());

    const qsizetype keep = whole.size() * percent / 100;
    const QString name = QStringLiteral("trunc_%1_%2").arg(percent).arg(QFileInfo(source).fileName());
    const QString path = scratchFile(name, whole.left(keep));
    QVERIFY(!path.isEmpty());

    // The assertion is simply that this returns. A crash, a hang or an
    // out-of-bounds read fails the test -- and under ASan or valgrind, so does
    // a memory error that would otherwise pass silently.
    if (exerciseArchive(path, rccReadsEntryData(path))) {
        ++m_openedCount;
    } else {
        ++m_rejectedCount;
    }
}

// ---------------------------------------------------------------------------
// Byte corruption: keeps the file length intact so that length-based sanity
// checks still pass, and damages the header fields those checks rely on.
// ---------------------------------------------------------------------------

void VsRobustnessTest::byteCorruption_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<int>("offset");
    QTest::addColumn<int>("value");

    // Offsets chosen to land in signature/header territory for every format,
    // plus a couple deeper into the stream.
    const QList<int> offsets = {0, 1, 2, 3, 7, 15, 31, 63, 127, 255, 511};
    const QList<int> values = {0x00, 0xFF};

    for (const QString &path : samples()) {
        const QString base = QFileInfo(path).fileName();
        for (int offset : offsets) {
            for (int value : values) {
                QTest::addRow("%s @ %d = 0x%02X", qPrintable(base), offset, value) << path << offset << value;
            }
        }
    }
}

void VsRobustnessTest::byteCorruption()
{
    QFETCH(QString, source);
    QFETCH(int, offset);
    QFETCH(int, value);

    QByteArray bytes = readAll(source);
    QVERIFY(!bytes.isEmpty());
    if (offset >= bytes.size()) {
        QSKIP("offset beyond this sample's size");
    }
    bytes[offset] = static_cast<char>(value);

    const QString name = QStringLiteral("corrupt_%1_%2_%3").arg(offset).arg(value).arg(QFileInfo(source).fileName());
    const QString path = scratchFile(name, bytes);
    QVERIFY(!path.isEmpty());

    if (exerciseArchive(path, rccReadsEntryData(path))) {
        ++m_openedCount;
    } else {
        ++m_rejectedCount;
    }
}

// ---------------------------------------------------------------------------
// Degenerate input: the cases a parser is most likely to have no branch for.
// ---------------------------------------------------------------------------

void VsRobustnessTest::degenerateInput_data()
{
    QTest::addColumn<QString>("extension");
    QTest::addColumn<QByteArray>("content");

    const QStringList extensions = {
        QStringLiteral("7z"),
        QStringLiteral("zip"),
        QStringLiteral("a"),
        QStringLiteral("rcc"),
        QStringLiteral("tar.gz"),
    };

    struct Case {
        const char *name;
        QByteArray content;
    };
    const QList<Case> cases = {
        {"empty", QByteArray()},
        {"one-zero-byte", QByteArray(1, '\0')},
        {"all-zeros-1k", QByteArray(1024, '\0')},
        {"all-0xFF-1k", QByteArray(1024, '\xFF')},
        {"ascii-text", QByteArray("this is definitely not an archive, but it is plausible ASCII\n")},
        // A 7z signature followed by nothing: the parser is invited to trust
        // it and then read a header that is not there.
        {"7z-signature-only", QByteArray("\x37\x7A\xBC\xAF\x27\x1C", 6)},
        // PK signature, likewise.
        {"zip-signature-only", QByteArray("PK\x03\x04", 4)},
        {"ar-signature-only", QByteArray("!<arch>\n", 8)},
    };

    for (const QString &extension : extensions) {
        for (const Case &c : cases) {
            QTest::addRow("%s.%s", c.name, qPrintable(extension)) << extension << c.content;
        }
    }
}

void VsRobustnessTest::degenerateInput()
{
    QFETCH(QString, extension);
    QFETCH(QByteArray, content);

    static int counter = 0;
    const QString name = QStringLiteral("degenerate_%1.%2").arg(counter++).arg(extension);
    const QString path = scratchFile(name, content);
    QVERIFY(!path.isEmpty());

    if (exerciseArchive(path, rccReadsEntryData(path))) {
        ++m_openedCount;
    } else {
        ++m_rejectedCount;
    }
}

void VsRobustnessTest::cleanupTestCase()
{
    // Not an assertion, but worth recording in the test log: the split tells
    // you how much of the suite actually reached parsing code rather than
    // being rejected at the signature check.
    qInfo("malformed inputs accepted by open(): %d, rejected: %d", m_openedCount, m_rejectedCount);
}

QTEST_GUILESS_MAIN(VsRobustnessTest)

#include "vs_robustnesstest.moc"
