#include "core/AetherPDProtocol.h"
#include <QFile>
#include <QString>
#include <cstdio>
int main(int argc,char** argv) {
    if(argc!=2) { return 2; }
    using namespace AetherSDR::aetherpd;
    for(unsigned channel=1;channel<=4;++channel) {
        for(unsigned phase=0;phase<3;++phase) {
            QFile file(QString::fromUtf8(argv[1])+QStringLiteral("/channel-%1-phase-%2.json").arg(channel).arg(phase));
            if(!file.open(QIODevice::ReadOnly)) { return 3; }
            const auto status=parseStatus(file.readAll());
            if(!status || !status->feedbackBinding) { return 4; }
            const FeedbackBinding& binding=*status->feedbackBinding;
            if(binding.channel!=channel || binding.pan!=0x40000001 || binding.guiHandle!=0x11223344
                || binding.valid!=(phase==1) || binding.failed!=(phase==2)
                || status->state!=(phase==0?State::FeedbackBindingPending:phase==1?State::Idle:State::FeedbackBindingFailed)) { return 5; }
        }
    }
    std::puts("PASS: all 12 native-to-Go-to-Qt binding states preserve exact channel, pan, GUI and validity");
}
