#pragma once

#include <QByteArray>
#include <QPointer>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Focus subset of K1XM's OTRSP v0.9: https://www.k1xm.org/OTRSP/
// Each transport/client owns its framing buffer; focus itself belongs to the radio.
class OtrspProtocol {
public:
    OtrspProtocol(RadioModel* model, int radio1Slice, int radio2Slice);

    QByteArray processData(const QByteArray& data);
    void close();

private:
    QByteArray processCommand(const QByteArray& command);
    SliceModel* radioSlice(int number) const;
    QByteArray receiveFocus() const;
    QByteArray transmitFocus() const;

    QPointer<RadioModel> m_model;
    int m_radio1Slice;
    int m_radio2Slice;
    QByteArray m_buffer;
    bool m_discarding{false};
    bool m_closed{false};
};

} // namespace AetherSDR
