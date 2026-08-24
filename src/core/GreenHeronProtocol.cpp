#include "core/GreenHeronProtocol.h"

#include <cmath>

namespace AetherSDR {
namespace GreenHeron {

namespace {

const QByteArray& recordTerminator()
{
    static const QByteArray kTerminator = QByteArrayLiteral("\r\n");
    return kTerminator;
}

// TURN's terminator, which is a bare CR and not the CRLF above. See
// kTurnVerb — this is a captured asymmetry, not an oversight.
const QByteArray& turnTerminator()
{
    static const QByteArray kTerminator = QByteArrayLiteral("\r");
    return kTerminator;
}

// QString::fromUtf8 substitutes U+FFFD for invalid sequences rather than
// failing, which is what we want: a mangled byte in one field must not cost us
// the record.
QString decode(const QByteArray& raw)
{
    return QString::fromUtf8(raw);
}

// Fields after the verb, capped. Returns at most kMaxFieldsPerRecord entries.
QList<QByteArray> splitFields(const QByteArray& record)
{
    QList<QByteArray> parts;
    int start = 0;
    while (parts.size() < kMaxFieldsPerRecord) {
        const int idx = record.indexOf(kUnitSeparator, start);
        if (idx < 0) {
            parts.append(record.mid(start));
            break;
        }
        parts.append(record.mid(start, idx - start));
        start = idx + 1;
    }
    return parts;
}

bool containsFramingByte(const QString& text)
{
    for (const QChar ch : text) {
        const char16_t code = ch.unicode();
        if (code == static_cast<char16_t>(static_cast<unsigned char>(kUnitSeparator))
            || code == static_cast<char16_t>(static_cast<unsigned char>(kGroupSeparator))
            || code == u'\r' || code == u'\n') {
            return true;
        }
    }
    return false;
}

} // namespace

QVector<QByteArray> splitRecords(QByteArray& buffer, bool* bytesDropped)
{
    if (bytesDropped != nullptr) {
        *bytesDropped = false;
    }

    QVector<QByteArray> records;
    int start = 0;
    for (;;) {
        const int idx = buffer.indexOf(recordTerminator(), start);
        if (idx < 0) {
            break;
        }
        const QByteArray record = buffer.mid(start, idx - start);
        start = idx + recordTerminator().size();

        // A trailing terminator leaves an empty trailing piece, which is
        // correct and not a record; so is a doubled terminator.
        if (record.isEmpty()) {
            continue;
        }
        if (record.size() > kMaxRecordBytes) {
            if (bytesDropped != nullptr) {
                *bytesDropped = true;
            }
            continue;
        }
        records.append(record);
    }

    buffer.remove(0, start);

    // No terminator in sight and the tail is already larger than any record
    // this protocol produces: the peer is not speaking it. Drop what we hold
    // rather than letting a stream with no CRLF consume memory indefinitely.
    if (buffer.size() > kMaxPendingBytes) {
        buffer.clear();
        if (bytesDropped != nullptr) {
            *bytesDropped = true;
        }
    }

    return records;
}

Record parse(const QByteArray& record)
{
    const QList<QByteArray> parts = splitFields(record);

    Record out;
    out.verb = decode(parts.value(0));

    // Everything after the verb, as strings, for the paths that want them.
    const int restCount = parts.size() - 1;

    if (out.verb == QLatin1String("SWITCHADD") && restCount >= 2) {
        out.type         = RecordType::SwitchAdd;
        out.unknownGroup = decode(parts.at(1));
        out.switchName   = decode(parts.at(2));
        for (int i = 3; i < parts.size() && out.ports.size() < kMaxPortsPerSwitch; ++i) {
            const QList<QByteArray> sub = parts.at(i).split(kGroupSeparator);
            Port port;
            port.name = decode(sub.value(0));
            if (sub.size() > 1) {
                port.unknownA = decode(sub.value(1));
            }
            if (sub.size() > 2) {
                port.unknownB = decode(sub.value(2));
            }
            if (sub.size() > 3) {
                port.unknownFlag = decode(sub.value(3));
            }
            out.ports.append(port);
        }
        return out;
    }

    if (out.verb == QLatin1String("SWITCHUPDATE") && restCount >= 2) {
        out.type           = RecordType::SwitchUpdate;
        out.switchName     = decode(parts.at(1));
        out.selected       = decode(parts.at(2));
        out.unknownC       = parts.size() > 3 ? decode(parts.at(3)) : QString{};
        out.wirelessSignal = parts.size() > 4 ? decode(parts.at(4)) : QString{};
        return out;
    }

    if (out.verb == QLatin1String("SWITCHLOCKS") && restCount >= 1) {
        out.type       = RecordType::SwitchLocks;
        out.switchName = decode(parts.at(1));
        for (int i = 2; i < parts.size(); ++i) {
            out.locks.append(decode(parts.at(i)));
        }
        return out;
    }

    if (out.verb == QLatin1String("ADD") && restCount >= 1) {
        out.type       = RecordType::DeviceAdd;
        out.deviceName = decode(parts.at(1));
        for (int i = 2; i < parts.size(); ++i) {
            out.fields.append(decode(parts.at(i)));
        }
        return out;
    }

    if (out.verb == QLatin1String("POINT") && restCount >= 2) {
        // A heading that will not parse as a number means the field model is
        // wrong for this record, so surface it as Unknown rather than
        // inventing a position for an antenna. toDouble() is the C-locale
        // parse, which is what the wire carries.
        bool ok = false;
        const double heading = decode(parts.at(2)).toDouble(&ok);
        if (ok) {
            out.type        = RecordType::Point;
            out.deviceName  = decode(parts.at(1));
            out.heading     = heading;
            out.pointStatus = parts.size() > 3 ? decode(parts.at(3)) : QString{};
            out.unknownD    = parts.size() > 4 ? decode(parts.at(4)) : QString{};
            return out;
        }
    }

    out.type = RecordType::Unknown;
    out.fields.clear();
    for (int i = 1; i < parts.size(); ++i) {
        out.fields.append(decode(parts.at(i)));
    }
    return out;
}

QByteArray encodeSelect(const QString& switchName, const QString& portName)
{
    if (switchName.isEmpty() || portName.isEmpty()) {
        return {};
    }
    if (containsFramingByte(switchName) || containsFramingByte(portName)) {
        return {};
    }

    QByteArray wire;
    wire += QByteArray(kSelectVerb);
    wire += kUnitSeparator;
    wire += switchName.toUtf8();
    wire += kUnitSeparator;
    wire += portName.toUtf8();
    wire += recordTerminator();
    return wire;
}

QByteArray encodeTurn(const QString& rotorName, double degrees)
{
    if (rotorName.isEmpty() || containsFramingByte(rotorName)) {
        return {};
    }
    // Validated here rather than trusted, because there is no stop verb: a
    // command that starts a wrong rotation cannot be recalled except by
    // walking to the controller and cutting its power.
    if (!std::isfinite(degrees)
        || degrees < kMinHeadingDegrees
        || degrees > kMaxHeadingDegrees) {
        return {};
    }

    QByteArray wire;
    wire += QByteArray(kTurnVerb);
    wire += kUnitSeparator;
    wire += rotorName.toUtf8();
    wire += kUnitSeparator;
    // QString::number is locale-independent; QLocale::toString() is not, and
    // a comma-decimal locale would emit "89,0" here.
    wire += QString::number(degrees, 'f', kHeadingDecimals).toUtf8();
    wire += turnTerminator();
    return wire;
}

double headingDelta(double lhs, double rhs)
{
    if (!std::isfinite(lhs) || !std::isfinite(rhs)) {
        return 0.0;
    }
    // std::fmod keeps the sign of the dividend, so a negative difference has
    // to be brought back into [0, 360) before the fold — otherwise 350 vs 10
    // comes out 340.
    double diff = std::fmod(lhs - rhs, 360.0);
    if (diff < 0.0) {
        diff += 360.0;
    }
    return diff > 180.0 ? 360.0 - diff : diff;
}

bool displayOrderLessThan(const QString& lhs, const QString& rhs)
{
    // "AS-84F-2" → head "AS-84F", tail 2. A name with no trailing number
    // sorts after every numbered one, and ties break on the plain string so
    // the order is total (an unstable comparator on equal keys would let the
    // columns swap between rebuilds).
    const auto split = [](const QString& name, QString& head, int& index) {
        const int dash = name.lastIndexOf(QLatin1Char('-'));
        if (dash <= 0 || dash == name.size() - 1) {
            head = name;
            index = -1;
            return;
        }
        bool ok = false;
        const int value = QStringView{name}.mid(dash + 1).toInt(&ok);
        if (!ok) {
            head = name;
            index = -1;
            return;
        }
        head = name.left(dash);
        index = value;
    };

    QString lhsHead;
    QString rhsHead;
    int lhsIndex = -1;
    int rhsIndex = -1;
    split(lhs, lhsHead, lhsIndex);
    split(rhs, rhsHead, rhsIndex);

    const bool lhsNumbered = lhsIndex >= 0;
    const bool rhsNumbered = rhsIndex >= 0;
    if (lhsNumbered != rhsNumbered) {
        return lhsNumbered;
    }
    if (lhsHead != rhsHead) {
        return lhsHead < rhsHead;
    }
    if (lhsIndex != rhsIndex) {
        return lhsIndex < rhsIndex;
    }
    return lhs < rhs;
}

} // namespace GreenHeron
} // namespace AetherSDR
