// Socket-free Linux bridge test. Own anonymous pipes replace its output FDs;
// no PulseAudio modules, audio devices, radio or helper process are opened.
#include "core/PipeWireAudioBridge.h"
#include <QCoreApplication>
#include <QScopeGuard>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace AetherSDR {
class PipeWireRxBridgeTest {
public:
    static int run()
    {
        int failures = 0;
        const auto check = [&](bool ok,const char* name) {
            if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",name); }
        };
        PipeWireAudioBridge bridge;
        std::array<int,8> readers{};
        readers.fill(-1);
        const auto closeReaders = qScopeGuard([&] {
            for (int fd : readers) { if (fd >= 0) { ::close(fd); } }
        });
        bridge.m_open.store(true);
        for (int i = 0; i < 8; ++i) {
            int fds[2];
            if (::pipe2(fds,O_NONBLOCK|O_CLOEXEC) != 0) { return 1; }
            readers[i] = fds[0];
            bridge.m_rx[i].fd = fds[1];
            bridge.m_rx[i].drainFd = ::dup(fds[0]);
            check(::fcntl(fds[1],F_SETPIPE_SZ,65536) >= 65536,"bounded native receive pipe capacity available");
            bridge.setChannelGain(i+1,0.5f);
            const std::array<float,4> stereo{float(i+1)/8.0f,0,float(i+1)/8.0f,0};
            bridge.feedDaxAudio(i+1,QByteArray(reinterpret_cast<const char*>(stereo.data()),sizeof(stereo)));
            std::array<float,4> output{};
            const ssize_t count=::read(readers[i],output.data(),sizeof(output));
            check(count==sizeof(output) && std::abs(output[0]-float(i+1)/64.0f)<1e-6f
                      && std::abs(output[1]-float(i+1)/32.0f)<1e-6f
                      && output[2]==output[1] && output[3]==output[1],
                  "all eight real bridge channels downmix stereo24 and interpolate mono48 with independent gain");
            bridge.feedDaxAudio(i+1,QByteArray(reinterpret_cast<const char*>(stereo.data()),sizeof(stereo)));
            bridge.resetRxChannel(i+1);
            check(::read(readers[i],output.data(),sizeof(output)) < 0,
                  "route reset drains retained output from the old channel owner");
            const std::array<float,4> silence{};
            bridge.feedDaxAudio(i+1,QByteArray(reinterpret_cast<const char*>(silence.data()),sizeof(silence)));
            output.fill(1);
            check(::read(readers[i],output.data(),sizeof(output))==sizeof(output)
                      && output==silence,"route reset discards interpolation history before the new owner's silence");
        }
        bridge.close();
        std::printf("PipeWire bridge: %d failures\n",failures);
        return failures ? 1 : 0;
    }
};
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    return AetherSDR::PipeWireRxBridgeTest::run();
}
