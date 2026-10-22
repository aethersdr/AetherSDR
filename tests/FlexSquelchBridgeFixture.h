#pragma once

// Manual-only fixture for the production MainWindow and agent automation bridge.
// It injects normalized state using Flex's real capability record. There is no
// firmware peer or radio connection. The normal CTest path never starts it.
#include "gui/MainWindow.h"
#include "gui/SpectrumWidget.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/SliceDelta.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QTimer>

namespace AetherSDR {
class FlexSquelchBridgeBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return FlexBackend().capabilities(); }
    bool isConnected() const override { return true; }
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int id, const QString& mode) override {
        SliceDelta delta;
        delta.mode = mode;
        emit sliceChanged(id, delta);
        // Late SQL-On readback exercises the status path after mode gating.
        QTimer::singleShot(150, this, [this, id] {
            SliceDelta late;
            late.squelchOn = true;
            late.squelchLevel = 62;
            emit sliceChanged(id, late);
        });
    }
    ReceiveDispatch requestSliceControl(int id, const SliceControlRequest& request) override {
        if (request.field != SliceControlRequest::Field::ActiveSlice || !request.valid()) {
            return ReceiveDispatch::Unsupported;
        }
        for (int sliceId = 0; sliceId < 4; ++sliceId) {
            SliceDelta delta; delta.active = sliceId == id;
            emit sliceChanged(sliceId, delta);
        }
        return ReceiveDispatch::Dispatched;
    }
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    ReceiveDispatch requestSliceSquelch(int id, const SliceSquelchRequest& request) override {
        SliceDelta delta;
        delta.squelchOn = request.enabled;
        delta.squelchLevel = request.level;
        emit sliceChanged(id, delta);
        return ReceiveDispatch::Dispatched;
    }
};

inline int runFlexSquelchBridgeFixture(QApplication& app)
{
    AppSettings::instance().setValue("AutoConnectToLastRadio", "False");
    MainWindow window;
    window.setWindowTitle("Flex SQL gating fixture — injected state, no radio");
    auto backend = std::make_unique<FlexSquelchBridgeBackend>();
    FlexSquelchBridgeBackend* source = backend.get();
    window.radioModel().setBackendForTest(std::move(backend), QStringLiteral("flex"));
    emit window.radioModel().capabilitiesChanged(true, source->capabilities());
    emit source->panCenterBandwidthChanged(QStringLiteral("0x40000000"), 14.2, 0.1);
    SliceDelta initial;
    initial.panId = QStringLiteral("0x40000000");
    initial.letter = QStringLiteral("A");
    initial.frequency = 14.2;
    initial.mode = QStringLiteral("USB");
    initial.modeList = QStringList{"USB", "LSB", "AM", "SAM", "FM", "NFM", "DIGU", "DIGL", "RTTY", "NT", "CW", "CWU", "CWL", "DFM"};
    initial.filterLow = 150;
    initial.filterHigh = 3000;
    initial.active = true;
    initial.squelchOn = false;
    initial.squelchLevel = 60;
    emit source->sliceChanged(0, initial);
    emit source->panCenterBandwidthChanged(QStringLiteral("0x40000001"), 7.1, 0.1);
    for (int id = 1; id < 4; ++id) {
        SliceDelta next = initial;
        next.panId = id < 2 ? "0x40000000" : "0x40000001";
        next.letter = QString(QChar('A' + id));
        next.frequency = id < 2 ? 14.2 : 7.1;
        next.active = false;
        next.squelchLevel = 54 + id;
        emit source->sliceChanged(id, next);
    }
    SliceDelta active; active.active = true;
    emit source->sliceChanged(0, active);
    if (!window.startAutomationBridge()) { return 1; }
    window.show();
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, &window, [&window] {
        for (SpectrumWidget* spectrum : window.findChildren<SpectrumWidget*>()) {
            spectrum->updateSpectrum(QVector<float>(1024,
                spectrum->panIndex() == 0 ? -110.0f : -80.0f));
        }
    });
    frames.start(100);
    return app.exec();
}
} // namespace AetherSDR
