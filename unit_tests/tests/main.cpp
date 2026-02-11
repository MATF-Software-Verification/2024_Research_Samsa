#include <QCoreApplication>
#include <QDebug>
#include <k7zip.h>
#include <kzip.h>
#include <qdir.h>

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);

  K7Zip archive("bin/test_file_compressed.7z");

  if (archive.open(QIODevice::ReadOnly)) {
    qDebug() << "Successfully opened archive";
    const KArchiveDirectory *root = archive.directory();
    QStringList entries = root->entries();

    for (const QString &name : entries) {
      const KArchiveEntry *entry = root->entry(name);

      if (entry->isFile()) {
        const KArchiveFile *file = static_cast<const KArchiveFile *>(entry);

        QIODevice *device = file->createDevice();
        if (device->open(QIODevice::ReadOnly)) {
          QByteArray content = device->readAll();
          qDebug() << "File:" << name;
          qDebug() << "Contents:" << content;
        }
        // not deleting means valgrind returns definetly lost bytes
        delete device;
      }
    }

    qDebug() << "Root: " << root;
    archive.close();
  } else {
    qDebug() << "FAILURE: Could not open archive. Error:"
             << archive.device()->errorString();
  }

  return 0;
}