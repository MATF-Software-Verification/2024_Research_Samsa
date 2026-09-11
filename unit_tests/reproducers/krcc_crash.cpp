/*
    Reproducer for finding 1 in unit_tests/FINDINGS.md:
    KRcc crashes when reading entries of a corrupted .rcc file.

    Corrupts a single byte of the given .rcc, then opens it with KRcc and reads
    every entry. Exits normally if the archive survives; dies with SIGBUS or
    SIGSEGV if the bug is present.

    Usage: krcc_crash <source.rcc> <offset> <byte-value>

    Known-crashing cases against autotests/data/runtime_resource.rcc:
        offset 12 value 255  -> SIGSEGV
        offset 15 value 0    -> SIGBUS
        offset 15 value 255  -> SIGBUS
        offset 20 value 255  -> SIGBUS

    This lives outside the CTest suite on purpose: a crash cannot be caught by
    QTest, so including it would mean a suite that always aborts.

    SPDX-License-Identifier: MIT
*/
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDebug>
#include <krcc.h>
#include <karchivedirectory.h>
#include <karchiveentry.h>
#include <karchivefile.h>

static void walk(const KArchiveDirectory *d, int depth = 0) {
    if (!d || depth > 8) return;
    for (const QString &n : d->entries()) {
        const KArchiveEntry *e = d->entry(n);
        if (!e) continue;
        if (e->isFile()) { const QByteArray b = static_cast<const KArchiveFile*>(e)->data(); Q_UNUSED(b); }
        else if (e->isDirectory()) walk(static_cast<const KArchiveDirectory*>(e), depth+1);
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QFile in(QString::fromLocal8Bit(argv[1]));
    if (!in.open(QIODevice::ReadOnly)) return 2;
    QByteArray bytes = in.readAll();
    const int off = atoi(argv[2]);
    if (off >= bytes.size()) return 3;
    bytes[off] = static_cast<char>(atoi(argv[3]));
    QTemporaryDir dir;
    const QString p = dir.filePath(QStringLiteral("corrupt.rcc"));
    { QFile o(p); o.open(QIODevice::WriteOnly); o.write(bytes); }
    KRcc rcc(p);
    if (!rcc.open(QIODevice::ReadOnly)) { fprintf(stderr, "open-rejected\n"); return 0; }
    walk(rcc.directory());
    rcc.close();
    fprintf(stderr, "survived\n");
    return 0;
}
