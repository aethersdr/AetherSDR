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
    initial.modeList = QStringList{"USB", "LSB", "AM", "SAM", "FM", "NFM", "DIGU", "DIGL", "RTTY", "NT", "CW", "CWU", "CWL", "DFM", "RADE"};
    initial.filterLow = 150;
    initial.filterHigh = 3000;
    initial.active = true;
    initial.squelchOn = false;
    initial.squelchLevel = 60;
    emit source->sliceChanged(0, initial);
    if (!window.startAutomationBridge()) { return 1; }
    window.show();
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, &window, [&window] {
        for (SpectrumWidget* spectrum : window.findChildren<SpectrumWidget*>()) {
            spectrum->updateSpectrum(QVector<float>(1024, -110.0f));
        }
    });
    frames.start(100);
    return app.exec();
}
} // namespace AetherSDR
