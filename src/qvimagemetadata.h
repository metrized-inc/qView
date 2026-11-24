#ifndef QVIMAGEMETADATA_H
#define QVIMAGEMETADATA_H

#include <QList>
#include <QString>

struct QVMetadataEntry
{
    QString key;
    QString value;
};

struct QVImageMetadata
{
    QList<QVMetadataEntry> textEntries;
    QList<QVMetadataEntry> exifEntries;

    bool isEmpty() const
    {
        return textEntries.isEmpty() && exifEntries.isEmpty();
    }
};

QVImageMetadata readImageMetadata(const QString &filePath);
bool isJsonMetadataValue(const QString &text);
QString formatJsonMetadataValue(const QString &text, int indent = 1);

#endif // QVIMAGEMETADATA_H
