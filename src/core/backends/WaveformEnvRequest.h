#pragma once

#include "core/backends/RadioControlTypes.h"

#include <QList>
#include <QPair>
#include <QString>

namespace AetherSDR {

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

    // A POSIX-style variable name: a letter or underscore, then letters, digits
    // or underscores. Rules out the '=' and spaces the wire uses as delimiters.
    static bool keyValid(const QString& key)
    {
        if (key.isEmpty() || key.size() > kMaxKeyChars) { return false; }
        for (qsizetype i = 0; i < key.size(); ++i) {
            const ushort c = key.at(i).unicode();
            const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
            const bool digit = c >= '0' && c <= '9';
            if (!letter && !(digit && i > 0)) { return false; }
        }
        return true;
    }

    // The value ends the command line, so a line break or other control
    // character would end it early or inject a second command.
    static bool valueValid(const QString& value)
    {
        if (value.size() > kMaxValueChars) { return false; }
        for (const QChar ch : value) {
            if (ch.category() == QChar::Other_Control || ch == QChar::LineSeparator
                || ch == QChar::ParagraphSeparator) { return false; }
        }
        return true;
    }

    // Same token rule as the existing waveform restart/remove commands.
    static bool nameValid(const QString& name)
    {
        if (name.isEmpty() || name.size() > 64) { return false; }
        for (const QChar ch : name) {
            const ushort c = ch.unicode();
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                  || c == '.' || c == '_' || c == '-')) { return false; }
        }
        return true;
    }

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

// The outcome of RadioModel::applyWaveformEnvironment.
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
};

} // namespace AetherSDR
