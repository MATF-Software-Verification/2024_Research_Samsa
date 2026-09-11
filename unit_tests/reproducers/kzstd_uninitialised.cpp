/*
    Reproducer for finding 3 in unit_tests/FINDINGS.md:
    KZstdFilter reads an uninitialised struct member when compressing an
    empty zstd stream.

    Writes a .tar.zst with no members and closes it, which flushes the zstd
    filter with zero input. Run under memcheck:

        valgrind --track-origins=yes ./kzstd_uninitialised

    memcheck reports a Conditional-jump-on-uninitialised-value inside
    ZSTD_compressStream2, with the value created by KZstdFilter::KZstdFilter()
    at kzstdfilter.cpp:32 -- the ZSTD_inBuffer/ZSTD_outBuffer members of the
    filter's Private are never initialised, and init() sets only inBuffer.size
    and inBuffer.pos, leaving inBuffer.src garbage when no data is written.

    SPDX-License-Identifier: MIT
*/

// Does KZstdFilter read an uninitialised member when compressing empty input?
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDebug>
#include <ktar.h>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString p = dir.filePath(QStringLiteral("empty.tar.zst"));
    KTar a(p);
    if (!a.open(QIODevice::WriteOnly)) return 2;
    a.close();                 // flush with zero data written
    qDebug() << "wrote empty" << p << QFileInfo(p).size() << "bytes";
    return 0;
}
