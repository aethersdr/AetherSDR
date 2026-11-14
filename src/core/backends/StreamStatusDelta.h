#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// One "stream <id> …" or "tx_audio_stream <id> …" status, as the model's
// stream rules read it: remote audio ownership, the DAX RX registry and the
// DAX stream trace. Values are as the radio reported them; numbers follow
// toInt() and ids parseStatusHandle(), as those rules did.
struct StreamStatusDelta {
    enum class Object { Stream, TxAudioStream };
    Object object{Object::Stream};
    quint32 streamId{0};
    // The removed word or key, or in_use=0.
    bool removed{false};
    QString type;                       // empty when not reported
    bool ownerReported{false};          // client_handle present
    quint32 owner{0};                   // its value; 0 for 0x0 or unparseable
    std::optional<QString> ip;          // trimmed
    std::optional<QString> compression;
    std::optional<int> daxChannel;
    std::optional<int> daxIqChannel;
    std::optional<int> daxIqRate;
    std::optional<QString> slice;
    std::optional<QString> pan;
    std::optional<bool> active;
    std::optional<bool> tx;
    QStringList keys;                   // the keys the line carried (diagnostics)
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::StreamStatusDelta)
