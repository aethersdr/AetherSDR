// Socket-free real DAX applet: injected capabilities, no audio device or radio.
#include "TestSettingsProfile.h"
#include "gui/DaxApplet.h"
#include "gui/MeterSlider.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QLabel>
#include <cstdio>
#include <memory>

using namespace AetherSDR;
class ReceiveBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int,double) override {}
    void setSliceMode(int,const QString&) override {}
    void setSliceFilter(int,int,int) override {}
    void setSliceAgc(int,const QString&,int) override {}
    void setPanCenter(const QString&,double,PanCenterIntent) override {}
    void setKeying(bool,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&,const QString&,quint64,const QVariant&) override {}
};
int main(int argc,char** argv)
{
    TestSettingsProfile profile("dax-receive-applet");
    QApplication app(argc,argv);
    int failures=0;
    const auto check=[&](bool ok,const char* name) {
        if(!ok) {++failures;std::fprintf(stderr,"FAIL: %s\n",name);}
    };
    RadioModel model;
    auto backend=std::make_unique<ReceiveBackend>();
    ReceiveBackend* source=backend.get();
    source->caps.canTransmit=false;
    source->caps.maxSlices=8;
    source->caps.receiveAudioExport=ReceiveAudioExport{{24000,48000},8};
    model.setBackendForTest(std::move(backend),QStringLiteral("test"));
    DaxApplet widget;
    widget.setRadioModel(&model);
    widget.setMaxDaxChannels(8);
    widget.setNativeReceiveRouting(true);
    widget.show();
    MeterSlider* tx=nullptr;
    int rx=0;
    for(MeterSlider* meter:widget.findChildren<MeterSlider*>()) {
        if(meter->accessibleName()=="DAX TX gain") {tx=meter;}
        else if(meter->accessibleName().startsWith("DAX RX ")) {
            ++rx;check(meter->isEnabled() && meter->isVisible(),"all eight DAX receive gain controls are usable");
        }
    }
    check(rx==8,"DAX applet exposes eight receive channels");
    check(tx && !tx->isEnabled() && tx->isVisible() && !tx->toolTip().isEmpty()
              && tx->accessibleDescription().contains("receive-only"),
          "receive-only TX is dimmed with an accessible reason, not hidden");
    for(int i=0;i<8;++i) {
        SliceDelta delta;delta.frequency=100.1+i*0.1;delta.mode=QStringLiteral("FM");
        emit source->sliceChanged(10+i,delta);
        widget.setReceiveChannelSlice(i+1,model.slice(10+i));
        QLabel* label=widget.findChild<QLabel*>(QStringLiteral("daxRxStatus%1").arg(i+1));
        check(label && label->text().contains("Slice"),"native route identifies its assigned slice in every DAX row");
    }
    source->caps.receiveAudioExport.reset();source->caps.hasDaxStreams=true;source->caps.canTransmit=true;
    emit model.capabilitiesChanged(true,source->caps);
    widget.setNativeReceiveRouting(false);
    check(tx && tx->isEnabled() && tx->toolTip().isEmpty()
              && !tx->accessibleDescription().contains("receive-only"),
          "returning to a transmit-capable DAX backend restores its TX control");
    std::printf("DAX applet: %d failures\n",failures);
    return failures?1:0;
}
