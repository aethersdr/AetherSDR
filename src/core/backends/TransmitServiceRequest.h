#pragma once

#include <QChar>
#include <QMetaType>
#include <QString>
#include <functional>
#include <optional>

namespace AetherSDR {

// Acceptance acknowledges a command, not execution or radio-idle evidence.
struct TransmitServiceReply {
    enum class Outcome { NotDispatched, Rejected, Accepted, Unknown };
    Outcome outcome{Outcome::NotDispatched};
    uint code{0};
    QString detail;
};
using TransmitServiceCompletion = std::function<void(const TransmitServiceReply&)>;

struct DvkRequest {
    enum class Operation {
        RecordStart, RecordStop, PreviewStart, PreviewStop, PlaybackStart,
        PlaybackStop, Clear, Remove, Name
    };
    Operation operation;
    int slot{-1};
    QString name;
    bool operator==(const DvkRequest&) const = default;
    bool starts() const
    {
        return operation == Operation::RecordStart || operation == Operation::PreviewStart
            || operation == Operation::PlaybackStart;
    }
    bool stops() const
    {
        return operation == Operation::RecordStop || operation == Operation::PreviewStop
            || operation == Operation::PlaybackStop;
    }
    bool valid() const
    {
        if (operation < Operation::RecordStart || operation > Operation::Name) { return false; }
        if (stops()) { return name.isEmpty(); }
        if (slot < 1 || slot > 12) { return false; }
        if (operation != Operation::Name) { return name.isEmpty(); }
        if (name.isEmpty() || name.toUtf8().size() > 61) { return false; }
        for (const QChar ch : name) {
            if (ch.isNull() || ch.category() == QChar::Other_Control || ch == QLatin1Char('"')
                || ch == QLatin1Char('\'') || ch == QLatin1Char('|') || ch == QLatin1Char('\\')) {
                return false;
            }
        }
        return true;
    }
};

struct NavtexRequest {
    QChar transmitter;
    QChar subject;
    QString text;
    std::optional<uint> serial;
    // Bound retained intent and the escaped single-line transport frame.
    static constexpr qsizetype kMaxTextBytes = 4096;
    bool valid() const
    {
        const auto letter = [](QChar ch) { return ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'); };
        if (!letter(transmitter) || !letter(subject) || text.isEmpty()
            || text.toUtf8().size() > kMaxTextBytes) { return false; }
        for (const QChar ch : text) {
            if (ch.isNull() || ch.category() == QChar::Other_Control || ch == QLatin1Char('|')) {
                return false;
            }
        }
        return true;
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DvkRequest)
Q_DECLARE_METATYPE(AetherSDR::NavtexRequest)
Q_DECLARE_METATYPE(AetherSDR::TransmitServiceReply)
