#include "qvimagemetadata.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace {

constexpr qint64 kMaxMetadataChunkBytes = 32 * 1024 * 1024;
constexpr qint64 kMaxTiffFileBytes = 32 * 1024 * 1024;

QByteArray decompressZlib(const QByteArray &compressed)
{
    if (compressed.isEmpty())
        return {};

    // qUncompress expects a 4-byte big-endian expected uncompressed length prefix.
    const quint32 expectedSize = qMax<quint32>(
            static_cast<quint32>(qMin<qint64>(static_cast<qint64>(compressed.size()) * 4,
                                              kMaxMetadataChunkBytes)),
            256);

    QByteArray withHeader;
    withHeader.reserve(compressed.size() + 4);
    uchar sizeBuf[4];
    qToBigEndian(expectedSize, sizeBuf);
    withHeader.append(reinterpret_cast<const char *>(sizeBuf), 4);
    withHeader.append(compressed);
    return qUncompress(withHeader);
}

quint16 readU16(const uchar *ptr, bool littleEndian)
{
    return littleEndian ? qFromLittleEndian<quint16>(ptr) : qFromBigEndian<quint16>(ptr);
}

quint32 readU32(const uchar *ptr, bool littleEndian)
{
    return littleEndian ? qFromLittleEndian<quint32>(ptr) : qFromBigEndian<quint32>(ptr);
}

qint16 readS16(const uchar *ptr, bool littleEndian)
{
    return static_cast<qint16>(readU16(ptr, littleEndian));
}

qint32 readS32(const uchar *ptr, bool littleEndian)
{
    return static_cast<qint32>(readU32(ptr, littleEndian));
}

int tiffTypeSize(quint16 type)
{
    switch (type) {
    case 1:  // BYTE
    case 2:  // ASCII
    case 6:  // SBYTE
    case 7:  // UNDEFINED
        return 1;
    case 3:  // SHORT
    case 8:  // SSHORT
        return 2;
    case 4:  // LONG
    case 9:  // SLONG
    case 11: // FLOAT
        return 4;
    case 5:  // RATIONAL
    case 10: // SRATIONAL
    case 12: // DOUBLE
        return 8;
    default:
        return 0;
    }
}

QString trimTrailingNulls(QString s)
{
    while (!s.isEmpty() && s.endsWith(QChar('\0')))
        s.chop(1);
    return s.trimmed();
}

QString decodeUtf16Bytes(const uchar *data, int byteLen, bool littleEndian)
{
    if (byteLen < 2)
        return {};

    // Respect BOM if present
    if (data[0] == 0xFF && data[1] == 0xFE) {
        littleEndian = true;
        data += 2;
        byteLen -= 2;
    } else if (data[0] == 0xFE && data[1] == 0xFF) {
        littleEndian = false;
        data += 2;
        byteLen -= 2;
    }

    const int charCount = byteLen / 2;
    QString result;
    result.reserve(charCount);
    for (int i = 0; i < charCount; ++i) {
        const quint16 ch = readU16(data + i * 2, littleEndian);
        if (ch == 0)
            break;
        result.append(QChar(ch));
    }
    return result.trimmed();
}

QString formatDecimal(double val, int maxDecimals = 2)
{
    QString s = QString::number(val, 'f', maxDecimals);
    if (s.contains('.')) {
        while (s.endsWith('0'))
            s.chop(1);
        if (s.endsWith('.'))
            s.chop(1);
    }
    return s;
}

QString formatOrientation(quint32 v)
{
    switch (v) {
    case 1: return QStringLiteral("Normal");
    case 2: return QStringLiteral("Mirrored horizontally");
    case 3: return QStringLiteral("Rotated 180°");
    case 4: return QStringLiteral("Mirrored vertically");
    case 5: return QStringLiteral("Mirrored horizontally, rotated 270° CW");
    case 6: return QStringLiteral("Rotated 90° CW");
    case 7: return QStringLiteral("Mirrored horizontally, rotated 90° CW");
    case 8: return QStringLiteral("Rotated 270° CW");
    default: return QString::number(v);
    }
}

QString formatResolutionUnit(quint32 v)
{
    switch (v) {
    case 1: return QStringLiteral("None");
    case 2: return QStringLiteral("Inches");
    case 3: return QStringLiteral("Centimetres");
    default: return QString::number(v);
    }
}

QString formatExposureProgram(quint32 v)
{
    switch (v) {
    case 0: return QStringLiteral("Not defined");
    case 1: return QStringLiteral("Manual");
    case 2: return QStringLiteral("Program AE");
    case 3: return QStringLiteral("Aperture-priority AE");
    case 4: return QStringLiteral("Shutter-priority AE");
    case 5: return QStringLiteral("Creative");
    case 6: return QStringLiteral("Action");
    case 7: return QStringLiteral("Portrait");
    case 8: return QStringLiteral("Landscape");
    default: return QString::number(v);
    }
}

QString formatMeteringMode(quint32 v)
{
    switch (v) {
    case 0: return QStringLiteral("Unknown");
    case 1: return QStringLiteral("Average");
    case 2: return QStringLiteral("Center-weighted average");
    case 3: return QStringLiteral("Spot");
    case 4: return QStringLiteral("Multi-spot");
    case 5: return QStringLiteral("Multi-segment");
    case 6: return QStringLiteral("Partial");
    case 255: return QStringLiteral("Other");
    default: return QString::number(v);
    }
}

QString formatFlash(quint32 v)
{
    if (v == 0)
        return QStringLiteral("Did not fire");
    const bool fired = (v & 0x01) != 0;
    return fired ? QStringLiteral("Fired") : QStringLiteral("Did not fire");
}

QString formatWhiteBalance(quint32 v)
{
    switch (v) {
    case 0: return QStringLiteral("Auto");
    case 1: return QStringLiteral("Manual");
    default: return QString::number(v);
    }
}

QString formatColorSpace(quint32 v)
{
    switch (v) {
    case 1: return QStringLiteral("sRGB");
    case 2: return QStringLiteral("Adobe RGB");
    case 65535: return QStringLiteral("Uncalibrated");
    default: return QString::number(v);
    }
}

QString exifTagName(quint16 tag)
{
    switch (tag) {
    // IFD0 tags
    case 0x010E: return QStringLiteral("Description");
    case 0x010F: return QStringLiteral("Make");
    case 0x0110: return QStringLiteral("Model");
    case 0x0112: return QStringLiteral("Orientation");
    case 0x011A: return QStringLiteral("X Resolution");
    case 0x011B: return QStringLiteral("Y Resolution");
    case 0x0128: return QStringLiteral("Resolution Unit");
    case 0x0131: return QStringLiteral("Software");
    case 0x0132: return QStringLiteral("Date Modified");
    case 0x013B: return QStringLiteral("Artist");
    case 0x013E: return QStringLiteral("White Point");
    case 0x8298: return QStringLiteral("Copyright");
    case 0x9C9B: return QStringLiteral("XP Title");
    case 0x9C9C: return QStringLiteral("XP Comment");
    case 0x9C9D: return QStringLiteral("XP Author");
    case 0x9C9E: return QStringLiteral("XP Keywords");
    case 0x9C9F: return QStringLiteral("XP Subject");
    // ExifIFD tags
    case 0x829A: return QStringLiteral("Exposure Time");
    case 0x829D: return QStringLiteral("F-Number");
    case 0x8822: return QStringLiteral("Exposure Program");
    case 0x8827: return QStringLiteral("ISO");
    case 0x8832: return QStringLiteral("Recommended Exposure Index");
    case 0x9000: return QStringLiteral("Exif Version");
    case 0x9003: return QStringLiteral("Date Taken");
    case 0x9004: return QStringLiteral("Date Digitized");
    case 0x9010: return QStringLiteral("Offset Time");
    case 0x9011: return QStringLiteral("Offset Time Original");
    case 0x9201: return QStringLiteral("Shutter Speed");
    case 0x9202: return QStringLiteral("Aperture");
    case 0x9203: return QStringLiteral("Brightness");
    case 0x9204: return QStringLiteral("Exposure Bias");
    case 0x9205: return QStringLiteral("Max Aperture");
    case 0x9206: return QStringLiteral("Subject Distance");
    case 0x9207: return QStringLiteral("Metering Mode");
    case 0x9208: return QStringLiteral("Light Source");
    case 0x9209: return QStringLiteral("Flash");
    case 0x920A: return QStringLiteral("Focal Length");
    case 0x9286: return QStringLiteral("User Comment");
    case 0x9290: return QStringLiteral("Subsec Time");
    case 0x9291: return QStringLiteral("Subsec Time Original");
    case 0xA001: return QStringLiteral("Color Space");
    case 0xA002: return QStringLiteral("Exif Image Width");
    case 0xA003: return QStringLiteral("Exif Image Height");
    case 0xA20E: return QStringLiteral("Focal Plane X Resolution");
    case 0xA20F: return QStringLiteral("Focal Plane Y Resolution");
    case 0xA402: return QStringLiteral("Exposure Mode");
    case 0xA403: return QStringLiteral("White Balance");
    case 0xA404: return QStringLiteral("Digital Zoom Ratio");
    case 0xA405: return QStringLiteral("Focal Length (35mm)");
    case 0xA406: return QStringLiteral("Scene Capture Type");
    case 0xA420: return QStringLiteral("Image Unique ID");
    case 0xA430: return QStringLiteral("Camera Owner");
    case 0xA431: return QStringLiteral("Body Serial Number");
    case 0xA432: return QStringLiteral("Lens Specification");
    case 0xA433: return QStringLiteral("Lens Make");
    case 0xA434: return QStringLiteral("Lens Model");
    case 0xA435: return QStringLiteral("Lens Serial Number");
    default: return {};
    }
}

bool shouldSkipExifTag(quint16 tag)
{
    switch (tag) {
    // Structural / binary / internal TIFF & EXIF tags
    case 0x00FE: // NewSubfileType
    case 0x0100: // ImageWidth (already in File Info)
    case 0x0101: // ImageLength (already in File Info)
    case 0x0102: // BitsPerSample
    case 0x0103: // Compression
    case 0x0106: // PhotometricInterpretation
    case 0x0111: // StripOffsets
    case 0x0115: // SamplesPerPixel
    case 0x0116: // RowsPerStrip
    case 0x0117: // StripByteCounts
    case 0x011C: // PlanarConfiguration
    case 0x0142: // TileWidth
    case 0x0143: // TileLength
    case 0x0144: // TileOffsets
    case 0x0145: // TileByteCounts
    case 0x0201: // JPEGInterchangeFormat
    case 0x0202: // JPEGInterchangeFormatLength
    case 0x0211: // YCbCrCoefficients
    case 0x0212: // YCbCrSubSampling
    case 0x0213: // YCbCrPositioning
    case 0x0214: // ReferenceBlackWhite
    case 0x02BC: // XMP packet
    case 0x8769: // ExifIFD pointer
    case 0x8773: // ICC Profile
    case 0x8825: // GPSInfo pointer
    case 0x9101: // ComponentsConfiguration
    case 0x9102: // CompressedBitsPerPixel
    case 0x927C: // MakerNote (proprietary binary blob)
    case 0xA000: // FlashpixVersion
    case 0xA005: // InteropIFD pointer
    case 0xA300: // FileSource
    case 0xA301: // SceneType
    case 0xA302: // CFAPattern
    case 0xC4A5: // PrintImageMatching
    case 0xEA1C: // Padding
        return true;
    default:
        return false;
    }
}

QString decodeUserComment(const uchar *data, int byteLen, bool littleEndian)
{
    if (byteLen <= 8)
        return {};

    const QByteArray prefix(reinterpret_cast<const char *>(data), 8);
    const uchar *payload = data + 8;
    const int payloadLen = byteLen - 8;

    if (prefix.startsWith("ASCII")) {
        return trimTrailingNulls(
                QString::fromUtf8(reinterpret_cast<const char *>(payload), payloadLen));
    }
    if (prefix.startsWith("UNICODE")) {
        return decodeUtf16Bytes(payload, payloadLen, littleEndian);
    }
    // Undefined / empty charset prefix: treat as UTF-8 if printable
    QString text = trimTrailingNulls(
            QString::fromUtf8(reinterpret_cast<const char *>(payload), payloadLen));
    return text;
}

bool getRational(const uchar *data, int byteLen, bool littleEndian, bool isSigned, double &outValue,
                 qint64 &outNum, qint64 &outDen)
{
    if (byteLen < 8)
        return false;
    if (isSigned) {
        outNum = readS32(data, littleEndian);
        outDen = readS32(data + 4, littleEndian);
    } else {
        outNum = readU32(data, littleEndian);
        outDen = readU32(data + 4, littleEndian);
    }
    if (outDen == 0)
        return false;
    outValue = static_cast<double>(outNum) / static_cast<double>(outDen);
    return true;
}

QString formatExifValue(quint16 tag, quint16 type, quint32 count, const uchar *data, int byteLen,
                        bool littleEndian)
{
    if (byteLen <= 0 || count == 0)
        return {};

    // Special encodings first
    if (tag == 0x9286 && (type == 7 || type == 1)) // UserComment
        return decodeUserComment(data, byteLen, littleEndian);

    if (tag >= 0x9C9B && tag <= 0x9C9F && type == 1) // Windows XP* UTF-16LE tags
        return decodeUtf16Bytes(data, byteLen, true);

    if (tag == 0x9000 && type == 7 && byteLen == 4) { // ExifVersion (e.g. "0232")
        QString ver = QString::fromLatin1(reinterpret_cast<const char *>(data), 4);
        if (ver.size() == 4) {
            int major = ver.left(2).toInt();
            int minor = ver.mid(2, 2).toInt();
            return QStringLiteral("%1.%2").arg(major).arg(minor, 2, 10, QChar('0'));
        }
        return ver;
    }

    if (type == 2) { // ASCII
        return trimTrailingNulls(
                QString::fromUtf8(reinterpret_cast<const char *>(data), byteLen));
    }

    // Single integer tags with custom formatting
    if ((type == 3 || type == 4) && count == 1) {
        const quint32 v = (type == 3) ? readU16(data, littleEndian) : readU32(data, littleEndian);
        switch (tag) {
        case 0x0112: return formatOrientation(v);
        case 0x0128: return formatResolutionUnit(v);
        case 0x8822: return formatExposureProgram(v);
        case 0x9207: return formatMeteringMode(v);
        case 0x9209: return formatFlash(v);
        case 0xA001: return formatColorSpace(v);
        case 0xA403: return formatWhiteBalance(v);
        case 0xA405: return QStringLiteral("%1 mm").arg(v);
        default: return QString::number(v);
        }
    }

    // Single rational tags with custom formatting
    if ((type == 5 || type == 10) && count == 1) {
        double val = 0.0;
        qint64 num = 0;
        qint64 den = 1;
        if (!getRational(data, byteLen, littleEndian, type == 10, val, num, den))
            return {};

        switch (tag) {
        case 0x829A: // ExposureTime
            if (val > 0.0 && val < 1.0) {
                const qint64 recip = qRound64(1.0 / val);
                if (recip > 1)
                    return QStringLiteral("1/%1 s").arg(recip);
            }
            return QStringLiteral("%1 s").arg(formatDecimal(val, 2));
        case 0x829D: // FNumber
            return QStringLiteral("f/%1").arg(formatDecimal(val, 1));
        case 0x9204: // ExposureBiasValue
            if (std::fabs(val) < 0.005)
                return QStringLiteral("0 EV");
            return QStringLiteral("%1%2 EV").arg(val > 0 ? "+" : "", formatDecimal(val, 2));
        case 0x920A: // FocalLength
            return QStringLiteral("%1 mm").arg(formatDecimal(val, 1));
        case 0x011A: // XResolution
        case 0x011B: // YResolution
            return formatDecimal(val, 2);
        default:
            return formatDecimal(val, 3);
        }
    }

    // Generic multi-value / remaining numeric types
    const quint32 maxItems = qMin<quint32>(count, 8);
    QStringList parts;
    const int elemSize = tiffTypeSize(type);
    if (elemSize <= 0)
        return {};

    for (quint32 i = 0; i < maxItems; ++i) {
        const int off = static_cast<int>(i) * elemSize;
        if (off + elemSize > byteLen)
            break;
        switch (type) {
        case 1: parts.append(QString::number(data[off])); break;
        case 6: parts.append(QString::number(static_cast<qint8>(data[off]))); break;
        case 3: parts.append(QString::number(readU16(data + off, littleEndian))); break;
        case 8: parts.append(QString::number(readS16(data + off, littleEndian))); break;
        case 4: parts.append(QString::number(readU32(data + off, littleEndian))); break;
        case 9: parts.append(QString::number(readS32(data + off, littleEndian))); break;
        case 5:
        case 10: {
            double val = 0.0;
            qint64 n = 0, d = 1;
            if (getRational(data + off, byteLen - off, littleEndian, type == 10, val, n, d))
                parts.append(formatDecimal(val, 3));
            break;
        }
        case 11: {
            quint32 bits = readU32(data + off, littleEndian);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(float));
            parts.append(formatDecimal(f, 3));
            break;
        }
        case 12: {
            quint64 bits = littleEndian ? qFromLittleEndian<quint64>(data + off)
                                        : qFromBigEndian<quint64>(data + off);
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(double));
            parts.append(formatDecimal(d, 3));
            break;
        }
        default:
            return {};
        }
    }
    if (parts.isEmpty())
        return {};
    if (count > maxItems)
        parts.append(QStringLiteral("…"));
    return parts.join(QStringLiteral(", "));
}

bool locateIfdEntryData(const QByteArray &tiff, quint32 entryPos, quint16 type, quint32 count,
                        bool littleEndian, const uchar *&outData, int &outByteLen)
{
    const int elemSize = tiffTypeSize(type);
    if (elemSize <= 0)
        return false;

    const quint64 totalBytes64 = static_cast<quint64>(elemSize) * static_cast<quint64>(count);
    if (totalBytes64 == 0 || totalBytes64 > static_cast<quint64>(kMaxMetadataChunkBytes))
        return false;

    const int totalBytes = static_cast<int>(totalBytes64);
    const uchar *base = reinterpret_cast<const uchar *>(tiff.constData());

    if (totalBytes <= 4) {
        if (static_cast<qint64>(entryPos) + 12 > tiff.size())
            return false;
        outData = base + entryPos + 8;
        outByteLen = totalBytes;
        return true;
    }

    const quint32 valueOffset = readU32(base + entryPos + 8, littleEndian);
    if (static_cast<qint64>(valueOffset) + totalBytes > tiff.size())
        return false;

    outData = base + valueOffset;
    outByteLen = totalBytes;
    return true;
}

bool readGpsDms(const uchar *data, int byteLen, bool littleEndian, double &outDegrees)
{
    if (byteLen < 24)
        return false;
    double d = 0.0, m = 0.0, s = 0.0;
    qint64 n = 0, den = 1;
    if (!getRational(data, 8, littleEndian, false, d, n, den)
        || !getRational(data + 8, 8, littleEndian, false, m, n, den)
        || !getRational(data + 16, 8, littleEndian, false, s, n, den)) {
        return false;
    }
    outDegrees = d + m / 60.0 + s / 3600.0;
    return true;
}

void parseGpsIfd(const QByteArray &tiff, quint32 ifdOffset, bool littleEndian,
                 QList<QVMetadataEntry> &entries)
{
    if (static_cast<qint64>(ifdOffset) + 2 > tiff.size())
        return;

    const uchar *base = reinterpret_cast<const uchar *>(tiff.constData());
    const quint16 countEntries = readU16(base + ifdOffset, littleEndian);
    if (static_cast<qint64>(ifdOffset) + 2 + static_cast<qint64>(countEntries) * 12 > tiff.size())
        return;

    QString latRef, lonRef, dateStamp, timeStamp;
    double latDeg = 0.0, lonDeg = 0.0, altVal = 0.0;
    bool hasLat = false, hasLon = false, hasAlt = false;
    int altRef = 0;

    for (quint16 i = 0; i < countEntries; ++i) {
        const quint32 entryPos = ifdOffset + 2 + static_cast<quint32>(i) * 12;
        const quint16 tag = readU16(base + entryPos, littleEndian);
        const quint16 type = readU16(base + entryPos + 2, littleEndian);
        const quint32 count = readU32(base + entryPos + 4, littleEndian);

        const uchar *valData = nullptr;
        int valLen = 0;
        if (!locateIfdEntryData(tiff, entryPos, type, count, littleEndian, valData, valLen))
            continue;

        switch (tag) {
        case 0x0001: // GPSLatitudeRef
            if (type == 2)
                latRef = trimTrailingNulls(
                        QString::fromLatin1(reinterpret_cast<const char *>(valData), valLen));
            break;
        case 0x0002: // GPSLatitude
            if (type == 5 && count >= 3)
                hasLat = readGpsDms(valData, valLen, littleEndian, latDeg);
            break;
        case 0x0003: // GPSLongitudeRef
            if (type == 2)
                lonRef = trimTrailingNulls(
                        QString::fromLatin1(reinterpret_cast<const char *>(valData), valLen));
            break;
        case 0x0004: // GPSLongitude
            if (type == 5 && count >= 3)
                hasLon = readGpsDms(valData, valLen, littleEndian, lonDeg);
            break;
        case 0x0005: // GPSAltitudeRef
            if (type == 1 && valLen >= 1)
                altRef = valData[0];
            break;
        case 0x0006: // GPSAltitude
            if (type == 5 && count >= 1) {
                qint64 n = 0, d = 1;
                hasAlt = getRational(valData, valLen, littleEndian, false, altVal, n, d);
            }
            break;
        case 0x0007: // GPSTimeStamp
            if (type == 5 && count >= 3 && valLen >= 24) {
                double h = 0.0, m = 0.0, s = 0.0;
                qint64 n = 0, d = 1;
                if (getRational(valData, 8, littleEndian, false, h, n, d)
                    && getRational(valData + 8, 8, littleEndian, false, m, n, d)
                    && getRational(valData + 16, 8, littleEndian, false, s, n, d)) {
                    timeStamp = QStringLiteral("%1:%2:%3 UTC")
                                        .arg(qRound(h), 2, 10, QChar('0'))
                                        .arg(qRound(m), 2, 10, QChar('0'))
                                        .arg(qRound(s), 2, 10, QChar('0'));
                }
            }
            break;
        case 0x001D: // GPSDateStamp
            if (type == 2)
                dateStamp = trimTrailingNulls(
                        QString::fromLatin1(reinterpret_cast<const char *>(valData), valLen));
            break;
        default:
            break;
        }
    }

    if (hasLat) {
        QString s = QString::number(latDeg, 'f', 6) + QStringLiteral("°");
        if (!latRef.isEmpty())
            s += QLatin1Char(' ') + latRef;
        entries.append({QStringLiteral("GPS Latitude"), s});
    }
    if (hasLon) {
        QString s = QString::number(lonDeg, 'f', 6) + QStringLiteral("°");
        if (!lonRef.isEmpty())
            s += QLatin1Char(' ') + lonRef;
        entries.append({QStringLiteral("GPS Longitude"), s});
    }
    if (hasAlt) {
        const double signedAlt = (altRef == 1) ? -altVal : altVal;
        entries.append({QStringLiteral("GPS Altitude"),
                        QStringLiteral("%1 m").arg(formatDecimal(signedAlt, 1))});
    }
    if (!dateStamp.isEmpty() && !timeStamp.isEmpty()) {
        entries.append({QStringLiteral("GPS Timestamp"), dateStamp + QLatin1Char(' ') + timeStamp});
    } else if (!dateStamp.isEmpty()) {
        entries.append({QStringLiteral("GPS Date"), dateStamp});
    } else if (!timeStamp.isEmpty()) {
        entries.append({QStringLiteral("GPS Timestamp"), timeStamp});
    }
}

void parseIfd(const QByteArray &tiff, quint32 ifdOffset, bool littleEndian,
              QSet<quint32> &visitedIfds, QList<QVMetadataEntry> &entries,
              quint32 &exifIfdOffset, quint32 &gpsIfdOffset)
{
    if (visitedIfds.contains(ifdOffset))
        return;
    if (static_cast<qint64>(ifdOffset) + 2 > tiff.size())
        return;
    visitedIfds.insert(ifdOffset);

    const uchar *base = reinterpret_cast<const uchar *>(tiff.constData());
    const quint16 countEntries = readU16(base + ifdOffset, littleEndian);
    if (static_cast<qint64>(ifdOffset) + 2 + static_cast<qint64>(countEntries) * 12 > tiff.size())
        return;

    for (quint16 i = 0; i < countEntries; ++i) {
        const quint32 entryPos = ifdOffset + 2 + static_cast<quint32>(i) * 12;
        const quint16 tag = readU16(base + entryPos, littleEndian);
        const quint16 type = readU16(base + entryPos + 2, littleEndian);
        const quint32 count = readU32(base + entryPos + 4, littleEndian);

        if (tag == 0x8769 && (type == 4 || type == 3) && count == 1) {
            const uchar *ptrData = nullptr;
            int ptrLen = 0;
            if (locateIfdEntryData(tiff, entryPos, type, count, littleEndian, ptrData, ptrLen)) {
                exifIfdOffset = (type == 4) ? readU32(ptrData, littleEndian)
                                            : readU16(ptrData, littleEndian);
            }
            continue;
        }
        if (tag == 0x8825 && (type == 4 || type == 3) && count == 1) {
            const uchar *ptrData = nullptr;
            int ptrLen = 0;
            if (locateIfdEntryData(tiff, entryPos, type, count, littleEndian, ptrData, ptrLen)) {
                gpsIfdOffset = (type == 4) ? readU32(ptrData, littleEndian)
                                           : readU16(ptrData, littleEndian);
            }
            continue;
        }

        if (shouldSkipExifTag(tag))
            continue;

        const QString name = exifTagName(tag);
        if (name.isEmpty())
            continue;

        const uchar *valData = nullptr;
        int valLen = 0;
        if (!locateIfdEntryData(tiff, entryPos, type, count, littleEndian, valData, valLen))
            continue;

        const QString formatted = formatExifValue(tag, type, count, valData, valLen, littleEndian);
        if (!formatted.isEmpty())
            entries.append({name, formatted});
    }
}

void parseTiffExif(const QByteArray &tiff, QList<QVMetadataEntry> &entries)
{
    if (tiff.size() < 8)
        return;

    const uchar *base = reinterpret_cast<const uchar *>(tiff.constData());
    bool littleEndian = false;
    if (base[0] == 'I' && base[1] == 'I')
        littleEndian = true;
    else if (base[0] == 'M' && base[1] == 'M')
        littleEndian = false;
    else
        return;

    if (readU16(base + 2, littleEndian) != 42)
        return;

    const quint32 ifd0Offset = readU32(base + 4, littleEndian);
    if (ifd0Offset < 8 || static_cast<qint64>(ifd0Offset) >= tiff.size())
        return;

    QSet<quint32> visitedIfds;
    quint32 exifIfdOffset = 0;
    quint32 gpsIfdOffset = 0;

    parseIfd(tiff, ifd0Offset, littleEndian, visitedIfds, entries, exifIfdOffset, gpsIfdOffset);

    if (exifIfdOffset != 0) {
        quint32 unusedExif = 0;
        quint32 unusedGps = 0;
        parseIfd(tiff, exifIfdOffset, littleEndian, visitedIfds, entries, unusedExif, unusedGps);
    }

    if (gpsIfdOffset != 0 && !visitedIfds.contains(gpsIfdOffset)) {
        visitedIfds.insert(gpsIfdOffset);
        parseGpsIfd(tiff, gpsIfdOffset, littleEndian, entries);
    }
}

void parsePngTextChunk(const QByteArray &type, const QByteArray &data,
                       QList<QVMetadataEntry> &entries)
{
    if (type == "tEXt") {
        const int nul = data.indexOf('\0');
        if (nul <= 0)
            return;
        const QString key = QString::fromLatin1(data.constData(), nul);
        const QString value =
                QString::fromLatin1(data.constData() + nul + 1, data.size() - nul - 1);
        entries.append({key, value});
        return;
    }

    if (type == "zTXt") {
        const int nul = data.indexOf('\0');
        if (nul <= 0 || nul + 2 > data.size())
            return;
        const uchar compressionMethod = static_cast<uchar>(data[nul + 1]);
        if (compressionMethod != 0)
            return;
        const QString key = QString::fromLatin1(data.constData(), nul);
        const QByteArray raw = decompressZlib(data.mid(nul + 2));
        entries.append({key, QString::fromLatin1(raw)});
        return;
    }

    if (type == "iTXt") {
        const int nul1 = data.indexOf('\0');
        if (nul1 <= 0 || nul1 + 3 > data.size())
            return;
        const QString key = QString::fromLatin1(data.constData(), nul1);
        const uchar compressionFlag = static_cast<uchar>(data[nul1 + 1]);
        const uchar compressionMethod = static_cast<uchar>(data[nul1 + 2]);

        const int nul2 = data.indexOf('\0', nul1 + 3);
        if (nul2 < 0)
            return;
        const int nul3 = data.indexOf('\0', nul2 + 1);
        if (nul3 < 0)
            return;

        QByteArray payload = data.mid(nul3 + 1);
        if (compressionFlag == 1) {
            if (compressionMethod != 0)
                return;
            payload = decompressZlib(payload);
        } else if (compressionFlag != 0) {
            return;
        }
        entries.append({key, QString::fromUtf8(payload)});
    }
}

void parsePngMetadata(QFile &file, QVImageMetadata &metadata)
{
    if (!file.seek(8))
        return;

    const qint64 fileSize = file.size();
    while (!file.atEnd()) {
        uchar chunkHeader[8];
        if (file.read(reinterpret_cast<char *>(chunkHeader), 8) != 8)
            break;

        const quint32 length = qFromBigEndian<quint32>(chunkHeader);
        const QByteArray type(reinterpret_cast<const char *>(chunkHeader + 4), 4);

        if (type == "IEND")
            break;

        const qint64 dataPos = file.pos();
        const qint64 nextChunkPos = dataPos + static_cast<qint64>(length) + 4;
        if (nextChunkPos < dataPos || nextChunkPos > fileSize)
            break;

        if ((type == "iTXt" || type == "tEXt" || type == "zTXt" || type == "eXIf")
            && static_cast<qint64>(length) <= kMaxMetadataChunkBytes) {
            const QByteArray data = file.read(length);
            if (data.size() != static_cast<int>(length))
                break;

            if (type == "eXIf")
                parseTiffExif(data, metadata.exifEntries);
            else
                parsePngTextChunk(type, data, metadata.textEntries);
        }

        if (!file.seek(nextChunkPos))
            break;
    }
}

void parseJpegExif(QFile &file, QVImageMetadata &metadata)
{
    if (!file.seek(2))
        return;

    const qint64 fileSize = file.size();
    while (!file.atEnd()) {
        char ch = 0;
        if (!file.getChar(&ch))
            break;
        if (static_cast<uchar>(ch) != 0xFF)
            continue;

        // Skip fill 0xFF bytes
        uchar marker = 0xFF;
        while (marker == 0xFF) {
            if (!file.getChar(&ch))
                return;
            marker = static_cast<uchar>(ch);
        }

        // Stop at Start of Scan (0xDA) or End of Image (0xD9)
        if (marker == 0xDA || marker == 0xD9)
            break;

        // Standalone markers without payload length
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
            continue;

        uchar lenBuf[2];
        if (file.read(reinterpret_cast<char *>(lenBuf), 2) != 2)
            break;
        const quint16 segLen = qFromBigEndian<quint16>(lenBuf);
        if (segLen < 2)
            break;

        const qint64 payloadLen = segLen - 2;
        const qint64 nextPos = file.pos() + payloadLen;
        if (nextPos > fileSize)
            break;

        if (marker == 0xE1 && payloadLen >= 6) {
            static const QByteArray kExifHeader("Exif\0\0", 6);
            const QByteArray payload = file.read(payloadLen);
            if (payload.size() == payloadLen && payload.startsWith(kExifHeader)) {
                parseTiffExif(payload.mid(6), metadata.exifEntries);
                break;
            }
        }

        if (!file.seek(nextPos))
            break;
    }
}

void parseWebpExif(QFile &file, QVImageMetadata &metadata)
{
    if (!file.seek(12))
        return;

    const qint64 fileSize = file.size();
    while (!file.atEnd()) {
        uchar chunkHeader[8];
        if (file.read(reinterpret_cast<char *>(chunkHeader), 8) != 8)
            break;

        const QByteArray fourcc(reinterpret_cast<const char *>(chunkHeader), 4);
        const quint32 chunkSize = qFromLittleEndian<quint32>(chunkHeader + 4);
        const qint64 paddedSize = static_cast<qint64>(chunkSize) + (chunkSize & 1u);
        const qint64 nextPos = file.pos() + paddedSize;
        if (nextPos < file.pos() || nextPos > fileSize)
            break;

        if (fourcc == "EXIF" && static_cast<qint64>(chunkSize) <= kMaxMetadataChunkBytes) {
            static const QByteArray kExifHeader("Exif\0\0", 6);
            QByteArray data = file.read(chunkSize);
            if (data.size() == static_cast<int>(chunkSize)) {
                if (data.startsWith(kExifHeader))
                    data = data.mid(6);
                parseTiffExif(data, metadata.exifEntries);
            }
            break;
        }

        if (!file.seek(nextPos))
            break;
    }
}

} // namespace

QVImageMetadata readImageMetadata(const QString &filePath)
{
    QVImageMetadata metadata;
    if (filePath.isEmpty())
        return metadata;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return metadata;

    const QByteArray header = file.read(12);
    if (header.size() < 4)
        return metadata;

    static const char kPngSig[8] = { '\x89', 'P', 'N', 'G', '\r', '\n', '\x1A', '\n' };
    if (header.size() >= 8 && std::memcmp(header.constData(), kPngSig, 8) == 0) {
        parsePngMetadata(file, metadata);
    } else if (static_cast<uchar>(header[0]) == 0xFF && static_cast<uchar>(header[1]) == 0xD8) {
        parseJpegExif(file, metadata);
    } else if (header.size() >= 12 && header.startsWith("RIFF") && header.mid(8, 4) == "WEBP") {
        parseWebpExif(file, metadata);
    } else if ((header[0] == 'I' && header[1] == 'I' && static_cast<uchar>(header[2]) == 42
                && header[3] == 0)
               || (header[0] == 'M' && header[1] == 'M' && header[2] == 0
                   && static_cast<uchar>(header[3]) == 42)) {
        if (file.size() <= kMaxTiffFileBytes && file.seek(0))
            parseTiffExif(file.readAll(), metadata.exifEntries);
    }

    return metadata;
}

bool isJsonMetadataValue(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;

    const QChar first = trimmed.at(0);
    if (first != QLatin1Char('{') && first != QLatin1Char('['))
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    return err.error == QJsonParseError::NoError && !doc.isNull();
}

QString formatJsonMetadataValue(const QString &text, int indent)
{
    if (!isJsonMetadataValue(text))
        return text;

    const int indentStep = qMax(0, indent);
    QString out;
    out.reserve(text.size() + text.size() / 4);

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    const int n = text.length();

    auto appendNewlineIndent = [&]() {
        out.append(QLatin1Char('\n'));
        if (depth > 0 && indentStep > 0)
            out.append(QString(depth * indentStep, QLatin1Char(' ')));
    };

    for (int i = 0; i < n; ++i) {
        const QChar ch = text.at(i);

        if (inString) {
            out.append(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }

        if (ch.isSpace())
            continue;

        if (ch == QLatin1Char('"')) {
            inString = true;
            out.append(ch);
        } else if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
            const QChar closing = (ch == QLatin1Char('{')) ? QLatin1Char('}') : QLatin1Char(']');
            int j = i + 1;
            while (j < n && text.at(j).isSpace())
                ++j;
            if (j < n && text.at(j) == closing) {
                out.append(ch);
                out.append(closing);
                i = j;
            } else if (ch == QLatin1Char('[')) {
                // Keep flat arrays of numbers on a single line (e.g. [10, 20, 30])
                int k = j;
                bool numbersOnly = true;
                bool hasDigit = false;
                while (k < n && text.at(k) != QLatin1Char(']')) {
                    const QChar c = text.at(k);
                    if (c.isDigit()) {
                        hasDigit = true;
                    } else if (!(c.isSpace() || c == QLatin1Char(',') || c == QLatin1Char('-')
                                 || c == QLatin1Char('+') || c == QLatin1Char('.')
                                 || c == QLatin1Char('e') || c == QLatin1Char('E'))) {
                        numbersOnly = false;
                        break;
                    }
                    ++k;
                }
                if (numbersOnly && hasDigit && k < n && text.at(k) == QLatin1Char(']')) {
                    out.append(QLatin1Char('['));
                    for (int m = j; m < k; ++m) {
                        const QChar c = text.at(m);
                        if (c.isSpace())
                            continue;
                        if (c == QLatin1Char(','))
                            out.append(QStringLiteral(", "));
                        else
                            out.append(c);
                    }
                    out.append(QLatin1Char(']'));
                    i = k;
                } else {
                    out.append(ch);
                    ++depth;
                    appendNewlineIndent();
                }
            } else {
                out.append(ch);
                ++depth;
                appendNewlineIndent();
            }
        } else if (ch == QLatin1Char('}') || ch == QLatin1Char(']')) {
            if (depth > 0)
                --depth;
            appendNewlineIndent();
            out.append(ch);
        } else if (ch == QLatin1Char(',')) {
            out.append(ch);
            appendNewlineIndent();
        } else if (ch == QLatin1Char(':')) {
            out.append(QStringLiteral(": "));
        } else {
            out.append(ch);
        }
    }

    out.append(QLatin1Char('\n'));
    return out;
}
