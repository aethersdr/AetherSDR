#pragma once

#include "core/backends/RadioControlTypes.h"

#include <QList>
#include <QPair>
#include <QString>

namespace AetherSDR {

// A waveform name as the radio's waveform verbs take it: one token of letters,
// digits, '.', '_' or '-', at most 64 characters. Shared by every waveform
// command (restart, remove, uninstall, recreate).
inline bool waveformNameTokenValid(const QString& name)
{
    if (name.isEmpty() || name.size() > 64) { return false; }
    for (const QChar ch : name) {
        const ushort c = ch.unicode();
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '.' || c == '_' || c == '-')) { return false; }
    }
    return true;
}

// One step of a container-environment change (SmartSDR 4.3.16, FlexLib
// Radio.cs:8841-8861). The radio keeps a staged environment: Clear empties it,
// Stage adds one variable, Recreate rebuilds a container from its installed image
// with whatever is staged.
struct WaveformEnvRequest {
    enum class Action { Clear, Stage, Recreate };
    Action action{Action::Clear};
    QString key;    // Stage
    QString value;  // Stage; may be empty or contain spaces
    QString name;   // Recreate: the container

    static constexpr int kMaxKeyChars = 64;
    static constexpr int kMaxValueChars = 512;

    // `waveform env clear` is the clear verb, so a variable cannot be named that.
    static bool keyReserved(const QString& key)
    {
        return key.compare(QLatin1String("clear"), Qt::CaseInsensitive) == 0;
    }

    // A POSIX-style variable name: a letter or underscore, then letters, digits
    // or underscores. Rules out the '=' and spaces the wire uses as delimiters,
    // and the reserved name `clear`.
    static bool keyValid(const QString& key)
    {
        if (key.isEmpty() || key.size() > kMaxKeyChars || keyReserved(key)) { return false; }
        for (qsizetype i = 0; i < key.size(); ++i) {
            const ushort c = key.at(i).unicode();
            const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
            const bool digit = c >= '0' && c <= '9';
            if (!letter && !(digit && i > 0)) { return false; }
        }
        return true;
    }

    // Why a value cannot be staged as typed. The value ends the command line, so
    // a line break or other control character would end it early or inject a
    // second command. The radio rebuilds the value from its space-separated
    // tokens, so it collapses runs of spaces (measured, FLEX-8600 4.3.16) and a
    // value with edge or repeated whitespace would arrive changed.
    enum class ValueIssue { None, TooLong, ControlCharacter, EdgeWhitespace, RepeatedWhitespace };
    static ValueIssue valueIssue(const QString& value)
    {
        if (value.size() > kMaxValueChars) { return ValueIssue::TooLong; }
        for (const QChar ch : value) {
            if (ch.category() == QChar::Other_Control || ch == QChar::LineSeparator
                || ch == QChar::ParagraphSeparator) { return ValueIssue::ControlCharacter; }
        }
        if (!value.isEmpty() && (value.front().isSpace() || value.back().isSpace())) {
            return ValueIssue::EdgeWhitespace;
        }
        for (qsizetype i = 1; i < value.size(); ++i) {
            if (value.at(i).isSpace() && value.at(i - 1).isSpace()) {
                return ValueIssue::RepeatedWhitespace;
            }
        }
        return ValueIssue::None;
    }
    static bool valueValid(const QString& value) { return valueIssue(value) == ValueIssue::None; }

    static bool nameValid(const QString& name) { return waveformNameTokenValid(name); }

    bool valid() const
    {
        switch (action) {
        case Action::Clear: return key.isEmpty() && value.isEmpty() && name.isEmpty();
        case Action::Stage: return keyValid(key) && valueValid(value) && name.isEmpty();
        case Action::Recreate: return nameValid(name) && key.isEmpty() && value.isEmpty();
        }
        return false;
    }
};

using WaveformEnvironment = QList<QPair<QString, QString>>;

// The outcome of RadioModel::applyWaveformEnvironment. Recreating the container
// and cleaning up the radio's staging are separate facts, reported separately.
struct WaveformEnvApplyResult {
    enum class Outcome {
        Recreated,       // the radio reported result=ok for the recreate
        RecreateFailed,  // the radio reported result=error
        Refused,         // a step was refused or failed; nothing was recreated
        NoConfirmation,  // recreate accepted, but no `restarted` status arrived
        Cancelled,       // the radio session ended first
    };
    Outcome outcome{Outcome::Refused};
    QString message;
    // Recreated only: a container status carrying exactly the requested
    // environment arrived. The first status after `restarted` still carries the
    // old one (measured), so the restart alone does not show it.
    bool environmentConfirmed{false};
    // The final `waveform env clear` was acknowledged by the radio.
    bool stagingCleared{false};
    QString stagingMessage;
};

} // namespace AetherSDR
