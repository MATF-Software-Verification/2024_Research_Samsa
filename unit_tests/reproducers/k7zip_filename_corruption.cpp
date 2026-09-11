/*
    Reproducer for finding 2 in unit_tests/FINDINGS.md:
    K7Zip corrupts every filename containing a non-ASCII character.

    Writes a single-entry 7z archive for each test name, reads it back, and
    prints the filename as written and as returned, in code points.

    Expected output against upstream 633dc09: ASCII matches; U+00E9 comes back
    as U+FFE9; anything above U+00FF makes the archive unopenable.

    Root cause is k7zip.cpp:2480, `wchar_t c = name[t].toLatin1();`.

    SPDX-License-Identifier: MIT
*/

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDebug>
#include <k7zip.h>
#include <karchivedirectory.h>

static void check(const QString &path, const QString &name)
{
    { K7Zip a(path);
      if (!a.open(QIODevice::WriteOnly)) { qDebug() << "write-open failed"; return; }
      a.writeFile(name, QByteArray("hello"));
      a.close(); }
    K7Zip a(path);
    if (!a.open(QIODevice::ReadOnly)) { qDebug().noquote() << "  UNREADABLE:" << a.errorString(); return; }
    const QStringList got = a.directory()->entries();
    QString shown = got.isEmpty() ? QStringLiteral("<none>") : got.first();
    QString hexIn, hexOut;
    for (QChar ch : name)  hexIn  += QString::asprintf("%04X ", ch.unicode());
    for (QChar ch : shown) hexOut += QString::asprintf("%04X ", ch.unicode());
    qDebug().noquote() << "  wrote U+[" << hexIn.trimmed() << "]  read U+[" << hexOut.trimmed() << "]"
                       << (shown == name ? " MATCH" : " *** CORRUPTED ***");
    a.close();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir; int n = 0;
    auto run = [&](const char *label, const QString &nm) {
        qDebug().noquote() << label;
        check(dir.filePath(QStringLiteral("c%1.7z").arg(n++)), nm);
    };
    run("ASCII 'ab'",            QStringLiteral("ab"));
    run("Latin-1 'e-acute' U+00E9", QString::fromUtf8("\xc3\xa9"));
    run("Latin-1 'a-ring'  U+00E5", QString::fromUtf8("\xc3\xa5"));
    run("Beyond Latin-1 U+010D",    QString::fromUtf8("\xc4\x8d"));
    return 0;
}
