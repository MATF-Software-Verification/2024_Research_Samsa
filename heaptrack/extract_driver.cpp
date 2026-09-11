/*
    A realistic workload for heap profiling: build a moderately large archive
    in each writable format, then extract every entry by reading its data.
    This is the shape of work Ark or KIO does, and it is where KArchive's heap
    behaviour actually matters -- it reads whole entries into QByteArray, and
    Qt's implicit sharing makes the allocation pattern non-obvious.

    Usage: extract_driver <format-extension> [entry-size-bytes] [entry-count]
      e.g. extract_driver tar.gz 1048576 64

    SPDX-License-Identifier: MIT
*/
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QRandomGenerator>
#include <QDebug>
#include <memory>
#include <k7zip.h>
#include <kzip.h>
#include <ktar.h>
#include <karchivedirectory.h>
#include <karchiveentry.h>
#include <karchivefile.h>

static std::unique_ptr<KArchive> make(const QString &ext, const QString &path) {
    if (ext == QLatin1String("zip")) return std::make_unique<KZip>(path);
    if (ext == QLatin1String("7z"))  return std::make_unique<K7Zip>(path);
    return std::make_unique<KTar>(path);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString ext  = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("tar.gz");
    const int entrySize = argc > 2 ? atoi(argv[2]) : 1024 * 1024;
    const int entryCount = argc > 3 ? atoi(argv[3]) : 64;

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("workload.%1").arg(ext));

    // Half compressible, half incompressible, so the filter does real work.
    QByteArray compressible(entrySize, 'A');
    QByteArray incompressible(entrySize, Qt::Uninitialized);
    QRandomGenerator gen(0x5EED);
    for (int i = 0; i < entrySize; ++i) incompressible[i] = char(gen.bounded(256));

    {
        auto ar = make(ext, path);
        if (!ar->open(QIODevice::WriteOnly)) { qWarning() << "write open failed" << ar->errorString(); return 2; }
        for (int i = 0; i < entryCount; ++i)
            ar->writeFile(QStringLiteral("entry_%1.bin").arg(i), (i & 1) ? incompressible : compressible);
        if (!ar->close()) { qWarning() << "write close failed"; return 3; }
    }
    qInfo() << "wrote" << ext << QFileInfo(path).size() << "bytes," << entryCount << "entries of" << entrySize;

    // The measured part: extract everything.
    qint64 total = 0;
    {
        auto ar = make(ext, path);
        if (!ar->open(QIODevice::ReadOnly)) { qWarning() << "read open failed" << ar->errorString(); return 4; }
        const KArchiveDirectory *root = ar->directory();
        const QStringList names = root->entries();
        for (const QString &n : names) {
            const KArchiveEntry *e = root->entry(n);
            if (e && e->isFile())
                total += static_cast<const KArchiveFile *>(e)->data().size();
        }
        ar->close();
    }
    qInfo() << "extracted" << total << "bytes total";
    return 0;
}
