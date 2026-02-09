#include <KF6/KArchive/KZip>
#include <QDebug>

int main() {
    KZip zip("test.zip");
    if (zip.open(QIODevice::WriteOnly)) {
        qDebug() << "KArchive KF6 is working!";
        zip.close();
        return 0;
    }
    return 1;
}
