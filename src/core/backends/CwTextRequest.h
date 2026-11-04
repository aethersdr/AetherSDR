#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <functional>
#include <limits>
#include <utility>

namespace AetherSDR {

// Plain text and logical block identity, never a fragment of vendor grammar.
// These desktop intents grant neither a daemon method nor transmit authority.
struct CwTextRequest {
    enum class Field { Send, SendMacro, SaveMacro, Speed, Delay, Qsk, Erase, Clear };
    enum class Origin { Operator, Transient, Restore };
    Field field{Field::Clear};
    int value{0};
    QString text;
    int block{0};
    Origin origin{Origin::Operator};

    CwTextRequest(Field requestedField = Field::Clear, int requestedValue = 0,
                  QString plainText = {}, int blockId = 0, Origin requestedOrigin = Origin::Operator)
        : field(requestedField), value(requestedValue), text(std::move(plainText)),
          block(blockId), origin(requestedOrigin) {}

    bool valid() const
    {
        if (text.size() > std::numeric_limits<int>::max()) { return false; }
        if (origin != Origin::Operator
            && !((origin == Origin::Transient || origin == Origin::Restore)
                 && field == Field::Speed)) { return false; }
        for (const QChar ch : text) {
            if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) { return false; }
        }
        switch (field) {
        case Field::Send: return !text.isEmpty() && block > 0 && value == 0;
        case Field::SendMacro: return text.isEmpty() && block == 0 && value >= 1 && value <= 12;
        case Field::SaveMacro: return block == 0 && value >= 0 && value < 12;
        case Field::Speed: return text.isEmpty() && block == 0 && value >= 5 && value <= 100;
        case Field::Delay: return text.isEmpty() && block == 0 && value >= 0 && value <= 2000;
        case Field::Qsk: return text.isEmpty() && block == 0 && (value == 0 || value == 1);
        case Field::Erase: return text.isEmpty() && block == 0 && value >= 0;
        case Field::Clear: return text.isEmpty() && block == 0 && value == 0;
        }
        return false;
    }
    bool keys() const { return field == Field::Send || field == Field::SendMacro; }
    bool configuration() const
    {
        return origin == Origin::Operator && (field == Field::Speed || field == Field::Delay
            || field == Field::Qsk || field == Field::SaveMacro);
    }
};

// Additional buffered-keyer controls. Native text keyers retain their existing
// sendCwText/abortCwText adapter; absence does not manufacture stored macros.
struct CwTextControl {
    enum class Completion { LocalHandoff, RadioCharacterIndex };
    QList<CwTextRequest::Field> fields;
    Completion completion{Completion::LocalHandoff};
    bool supports(CwTextRequest::Field field) const { return fields.contains(field); }
};

struct CwTextReply {
    enum class Kind { Queued, Failed, Invalid };
    Kind kind{Kind::Invalid};
    int startIndex{-1};
    int block{0};
    int code{-1};
};
using CwTextCompletion = std::function<void(const CwTextReply&)>;

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::CwTextRequest)
Q_DECLARE_METATYPE(AetherSDR::CwTextReply)
