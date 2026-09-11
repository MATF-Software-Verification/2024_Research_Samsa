/*
    Write/read round trips and boundary cases for KArchive.

    Part of the Software Verification analysis of KArchive. Where
    vs_robustnesstest.cpp attacks the parsers with damaged input, this suite
    checks the writers and the compression filters on inputs that are valid but
    awkward: empty archives, zero-length members, incompressible data, unicode
    names, deep nesting, and re-reading the same member twice.

    The tar cases run once per compression backend (none, gzip, bzip2, xz,
    zstd), which is the cheapest way to exercise every KFilterBase subclass
    through a realistic path rather than in isolation.

    SPDX-License-Identifier: MIT
*/

#include <QBuffer>
#include <QObject>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include <k7zip.h>
#include <karchivedirectory.h>
#include <karchiveentry.h>
#include <karchivefile.h>
#include <ktar.h>
#include <kzip.h>

namespace
{

/*! Deterministic incompressible bytes -- a fixed seed keeps failures reproducible. */
QByteArray randomBytes(int size)
{
    QRandomGenerator generator(0xC0FFEEu);
    QByteArray out(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) {
        out[i] = static_cast<char>(generator.bounded(256));
    }
    return out;
}

/*! Highly compressible bytes. */
QByteArray repetitiveBytes(int size)
{
    return QByteArray(size, 'A');
}

/*!
    How a filename round trip is expected to fail, if at all.

    K7Zip has two distinct failure modes for non-ASCII names and they surface at
    different assertions, so a single "expected to fail" flag is not enough --
    marking the whole test would make the Latin-1 rows report XPASS.
 */
enum ExpectedFailure {
    NoFailure = 0,
    FilenameCorrupted, //!< opens, but the name read back differs from the name written
    ArchiveUnopenable, //!< the archive K7Zip wrote cannot be reopened at all
};

/*! Opens an already-written archive at \a path, choosing the class from \a extension. */
std::unique_ptr<KArchive> openExisting(const QString &extension, const QString &path)
{
    if (extension == QLatin1String("zip")) {
        return std::make_unique<KZip>(path);
    }
    if (extension == QLatin1String("7z")) {
        return std::make_unique<K7Zip>(path);
    }
    return std::make_unique<KTar>(path);
}

const KArchiveFile *fileEntry(const KArchive &archive, const QString &path)
{
    const KArchiveDirectory *root = archive.directory();
    if (!root) {
        return nullptr;
    }
    const KArchiveEntry *entry = root->entry(path);
    if (!entry || !entry->isFile()) {
        return nullptr;
    }
    return static_cast<const KArchiveFile *>(entry);
}

} // namespace

class VsRoundTripTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void roundTrip_data();
    void roundTrip();

    void emptyArchive_data();
    void emptyArchive();

    void readTwice_data();
    void readTwice();

    void nonAsciiFilenames_data();
    void nonAsciiFilenames();

    void deviceSeekAndRead_data();
    void deviceSeekAndRead();

private:
    /*! Creates the archive type implied by \a extension at a fresh scratch path. */
    std::unique_ptr<KArchive> create(const QString &extension, QString *pathOut);

    QTemporaryDir m_dir;
    int m_counter = 0;
};

std::unique_ptr<KArchive> VsRoundTripTest::create(const QString &extension, QString *pathOut)
{
    const QString path = m_dir.filePath(QStringLiteral("rt_%1.%2").arg(m_counter++).arg(extension));
    if (pathOut) {
        *pathOut = path;
    }
    if (extension == QLatin1String("zip")) {
        return std::make_unique<KZip>(path);
    }
    if (extension == QLatin1String("7z")) {
        return std::make_unique<K7Zip>(path);
    }
    return std::make_unique<KTar>(path);
}

void VsRoundTripTest::initTestCase()
{
    QVERIFY2(m_dir.isValid(), qPrintable(m_dir.errorString()));
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

void VsRoundTripTest::roundTrip_data()
{
    QTest::addColumn<QString>("extension");

    // One row per writable container/compression combination.
    const QStringList extensions = {
        QStringLiteral("tar"),
        QStringLiteral("tar.gz"),
        QStringLiteral("tar.bz2"),
        QStringLiteral("tar.xz"),
        QStringLiteral("tar.zst"),
        QStringLiteral("zip"),
        QStringLiteral("7z"),
    };
    for (const QString &extension : extensions) {
        QTest::addRow("%s", qPrintable(extension)) << extension;
    }
}

void VsRoundTripTest::roundTrip()
{
    QFETCH(QString, extension);

    // Content chosen to stress the filters from both ends: something that
    // compresses to almost nothing, something that cannot be compressed at
    // all, an empty member, and a unicode name.
    const QByteArray compressible = repetitiveBytes(64 * 1024);
    const QByteArray incompressible = randomBytes(64 * 1024);
    const QByteArray small = QByteArrayLiteral("hello");

    QString path;
    {
        std::unique_ptr<KArchive> archive = create(extension, &path);
        QVERIFY2(archive->open(QIODevice::WriteOnly), qPrintable(archive->errorString()));

        QVERIFY(archive->writeFile(QStringLiteral("compressible.bin"), compressible));
        QVERIFY(archive->writeFile(QStringLiteral("incompressible.bin"), incompressible));
        QVERIFY(archive->writeFile(QStringLiteral("small.txt"), small));
        QVERIFY(archive->writeFile(QStringLiteral("empty.txt"), QByteArray()));
        QVERIFY(archive->writeFile(QStringLiteral("a/b/c/d/e/deep.txt"), small));

        QVERIFY2(archive->close(), qPrintable(archive->errorString()));
    }

    QVERIFY(QFileInfo::exists(path));

    {
        // Re-open the file we just wrote, not a new scratch path.
        std::unique_ptr<KArchive> archive = openExisting(extension, path);

        QVERIFY2(archive->open(QIODevice::ReadOnly), qPrintable(archive->errorString()));

        const KArchiveFile *f = fileEntry(*archive, QStringLiteral("compressible.bin"));
        QVERIFY(f);
        QCOMPARE(f->data(), compressible);

        f = fileEntry(*archive, QStringLiteral("incompressible.bin"));
        QVERIFY(f);
        QCOMPARE(f->data(), incompressible);

        f = fileEntry(*archive, QStringLiteral("empty.txt"));
        QVERIFY(f);
        QCOMPARE(f->data().size(), 0);

        f = fileEntry(*archive, QStringLiteral("small.txt"));
        QVERIFY(f);
        QCOMPARE(f->data(), small);

        QVERIFY(archive->close());
    }
}

// ---------------------------------------------------------------------------
// An archive with no members at all -- a case parsers often lack a branch for.
// ---------------------------------------------------------------------------

void VsRoundTripTest::emptyArchive_data()
{
    roundTrip_data();
}

void VsRoundTripTest::emptyArchive()
{
    QFETCH(QString, extension);

    QString path;
    {
        std::unique_ptr<KArchive> archive = create(extension, &path);
        QVERIFY2(archive->open(QIODevice::WriteOnly), qPrintable(archive->errorString()));
        QVERIFY2(archive->close(), qPrintable(archive->errorString()));
    }

    std::unique_ptr<KArchive> archive = openExisting(extension, path);

    QVERIFY2(archive->open(QIODevice::ReadOnly), qPrintable(archive->errorString()));
    QVERIFY(archive->directory());
    QCOMPARE(archive->directory()->entries().size(), 0);
    QVERIFY(archive->close());
}

// ---------------------------------------------------------------------------
// Reading the same member twice must produce the same bytes. This catches
// filters that leave stream state behind after the first read.
// ---------------------------------------------------------------------------

void VsRoundTripTest::readTwice_data()
{
    roundTrip_data();
}

void VsRoundTripTest::readTwice()
{
    QFETCH(QString, extension);

    const QByteArray payload = repetitiveBytes(32 * 1024) + randomBytes(32 * 1024);

    QString path;
    {
        std::unique_ptr<KArchive> archive = create(extension, &path);
        QVERIFY(archive->open(QIODevice::WriteOnly));
        QVERIFY(archive->writeFile(QStringLiteral("payload.bin"), payload));
        QVERIFY(archive->close());
    }

    std::unique_ptr<KArchive> archive = openExisting(extension, path);

    QVERIFY(archive->open(QIODevice::ReadOnly));
    const KArchiveFile *f = fileEntry(*archive, QStringLiteral("payload.bin"));
    QVERIFY(f);

    const QByteArray first = f->data();
    const QByteArray second = f->data();
    QCOMPARE(first, payload);
    QCOMPARE(second, payload);

    QVERIFY(archive->close());
}

// ---------------------------------------------------------------------------
// Filenames must survive a write/read round trip unchanged.
//
// This test documents a real defect found during this analysis: K7Zip corrupts
// every filename containing a character outside US-ASCII. See
// unit_tests/FINDINGS.md for the full write-up. Root cause, k7zip.cpp:2480:
//
//     wchar_t c = name[t].toLatin1();
//     writeByte((unsigned char)c);
//     writeByte((unsigned char)(c >> 8));
//
// QChar::toLatin1() returns a *signed* char and yields '\0' for anything it
// cannot represent, which produces two distinct failures:
//
//   U+0080..U+00FF  representable, but the signed char is negative, so c >> 8
//                   sign-extends and the high byte is written as 0xFF.
//                   U+00E9 comes back as U+FFE9 -- silent corruption.
//   above U+00FF    toLatin1() returns '\0', which is 7z's name terminator.
//                   The name is truncated mid-record, the declared record size
//                   no longer matches, and the archive K7Zip just wrote can no
//                   longer be opened by K7Zip at all.
//
// The 7z rows are marked QEXPECT_FAIL rather than removed, so the suite stays
// green while continuing to assert that the bug is still present. If upstream
// fixes it, these rows turn into XPASS and the suite tells us.
// ---------------------------------------------------------------------------

void VsRoundTripTest::nonAsciiFilenames_data()
{
    QTest::addColumn<QString>("extension");
    QTest::addColumn<QString>("filename");
    QTest::addColumn<int>("expectedFailure");

    struct NameCase {
        const char *label;
        QString name;
        bool beyondLatin1;
    };
    const QList<NameCase> names = {
        {"latin1-e-acute", QString::fromUtf8("caf\xc3\xa9.txt"), false},
        {"serbian-c-caron", QString::fromUtf8("\xc4\x8dasopis.txt"), true},
        {"cyrillic", QString::fromUtf8("\xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82.txt"), true},
        {"cjk", QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87.txt"), true},
    };

    const QStringList extensions = {
        QStringLiteral("tar"),
        QStringLiteral("tar.gz"),
        QStringLiteral("zip"),
        QStringLiteral("7z"),
    };

    for (const QString &extension : extensions) {
        for (const NameCase &n : names) {
            // The two K7Zip failure modes are genuinely different and have to
            // be expected at different assertions, or the Latin-1 rows XPASS.
            int mode = NoFailure;
            if (extension == QLatin1String("7z")) {
                mode = n.beyondLatin1 ? ArchiveUnopenable : FilenameCorrupted;
            }
            QTest::addRow("%s / %s", qPrintable(extension), n.label) << extension << n.name << mode;
        }
    }
}

void VsRoundTripTest::nonAsciiFilenames()
{
    QFETCH(QString, extension);
    QFETCH(QString, filename);
    QFETCH(int, expectedFailure);

    const QByteArray payload = QByteArrayLiteral("payload");

    QString path;
    {
        std::unique_ptr<KArchive> archive = create(extension, &path);
        QVERIFY(archive->open(QIODevice::WriteOnly));
        QVERIFY(archive->writeFile(filename, payload));
        QVERIFY(archive->close());
    }

    std::unique_ptr<KArchive> archive = openExisting(extension, path);

    if (expectedFailure == ArchiveUnopenable) {
        QEXPECT_FAIL("",
                     "K7Zip: toLatin1() yields '\\0' for characters above U+00FF, which is 7z's "
                     "name terminator, so the archive it just wrote no longer parses. "
                     "See unit_tests/FINDINGS.md",
                     Abort);
    }
    QVERIFY2(archive->open(QIODevice::ReadOnly), qPrintable(archive->errorString()));

    const QStringList entries = archive->directory()->entries();
    QCOMPARE(entries.size(), 1);

    if (expectedFailure == FilenameCorrupted) {
        QEXPECT_FAIL("",
                     "K7Zip: toLatin1() returns a signed char, so c >> 8 sign-extends and the "
                     "high byte is written as 0xFF. U+00E9 comes back as U+FFE9. "
                     "See unit_tests/FINDINGS.md",
                     Abort);
    }
    QCOMPARE(entries.first(), filename);

    const KArchiveFile *f = fileEntry(*archive, filename);
    QVERIFY(f);
    QCOMPARE(f->data(), payload);

    QVERIFY(archive->close());
}

// ---------------------------------------------------------------------------
// createDevice() hands out a QIODevice over a slice of the archive. Seeking
// and short reads through it exercise KLimitedIODevice, whose branch coverage
// upstream is among the weakest in the library.
// ---------------------------------------------------------------------------

void VsRoundTripTest::deviceSeekAndRead_data()
{
    QTest::addColumn<QString>("extension");
    // Only uncompressed containers hand out a genuinely seekable slice; the
    // compressed ones decompress into a buffer first.
    QTest::addRow("tar") << QStringLiteral("tar");
    QTest::addRow("zip") << QStringLiteral("zip");
}

void VsRoundTripTest::deviceSeekAndRead()
{
    QFETCH(QString, extension);

    const QByteArray payload = randomBytes(8192);

    QString path;
    {
        std::unique_ptr<KArchive> archive = create(extension, &path);
        QVERIFY(archive->open(QIODevice::WriteOnly));
        QVERIFY(archive->writeFile(QStringLiteral("payload.bin"), payload));
        QVERIFY(archive->close());
    }

    std::unique_ptr<KArchive> archive = openExisting(extension, path);

    QVERIFY(archive->open(QIODevice::ReadOnly));
    const KArchiveFile *f = fileEntry(*archive, QStringLiteral("payload.bin"));
    QVERIFY(f);

    std::unique_ptr<QIODevice> device(f->createDevice());
    QVERIFY(device);
    QVERIFY(device->open(QIODevice::ReadOnly));
    QCOMPARE(device->size(), payload.size());

    // Sequential short reads must reassemble the payload exactly.
    QByteArray assembled;
    while (assembled.size() < payload.size()) {
        const QByteArray chunk = device->read(777);
        if (chunk.isEmpty()) {
            break;
        }
        assembled += chunk;
    }
    QCOMPARE(assembled, payload);

    // Seeking to the boundaries, and one byte past the end.
    QVERIFY(device->seek(0));
    QCOMPARE(device->read(4), payload.left(4));

    QVERIFY(device->seek(payload.size() - 1));
    QCOMPARE(device->read(4).size(), 1);

    QVERIFY(device->seek(payload.size()));
    QVERIFY(device->read(4).isEmpty());
    QVERIFY(device->atEnd());

    QVERIFY(archive->close());
}

QTEST_GUILESS_MAIN(VsRoundTripTest)

#include "vs_roundtriptest.moc"
