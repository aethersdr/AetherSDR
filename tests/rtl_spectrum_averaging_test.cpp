#include "RtlInjectedDevice.h"
#include "core/backends/rtl/RtlSdrBackend.h"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <random>

using namespace AetherSDR;
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static RtlSdrDdc& attach(RtlSdrBackend& backend)
    {
        // No reader thread or USB handle. Exercise the real seam setters and
        // production DDC on this acquisition-context test thread.
        backend.m_worker = std::make_unique<RtlSdrWorker>(
            std::make_unique<test::InjectedDevice>(std::make_shared<test::DeviceState>()));
        return *backend.ddc();
    }
};
}
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Probe {
    rtl::RtlSdrBackend backend;
    rtl::RtlSdrDdc& ddc = rtl::RtlCaptureBackendTestAccess::attach(backend);
    QByteArray last, waterfall;
    int frames = 0;
    Probe(int average, bool weighted, int fps = 25)
    {
        backend.setPanFrameRate("0xe1000000", fps);
        backend.setPanAverage("0xe1000000", average);
        backend.setPanWeightedAverage("0xe1000000", weighted);
        QObject::connect(&ddc, &rtl::RtlSdrDdc::spectrumFrameReady,
            [&](int, const QByteArray& frame) { last = frame; ++frames; });
        QObject::connect(&ddc, &rtl::RtlSdrDdc::waterfallRowReady,
            [&](int, const QByteArray& frame) { waterfall = frame; });
    }
    float feed(float amplitude, int count)
    {
        ddc.processIqData(QVector<std::complex<float>>(count, {amplitude, 0}), false);
        float value = -999;
        if (last.size() == rtl::RtlSdrDdc::kSpectrumBinCount * int(sizeof(float))) {
            std::memcpy(&value, last.constData() + last.size()/2, sizeof(value));
        }
        return value;
    }
};

void cadenceAndReceiverIndependence()
{
    constexpr int window = rtl::RtlSdrDdc::kSpectrumBinCount;
    for (int fps : {10,25,60}) {
        Probe raw(0,false,fps), averaged(100,false,fps);
        const int stride = 2400000/fps;
        double power = std::pow(10.,raw.feed(.01f,window)/10.);
        averaged.feed(.01f,window);
        const double retained = std::exp(-stride/2400000.);
        for (int i=0;i<12;++i) {
            const float amplitude = i%3 ? .3f : .03f;
            const float input = raw.feed(amplitude,stride);
            power = retained*power+(1-retained)*std::pow(10.,input/10.);
            check(std::abs(averaged.feed(amplitude,stride)-10*std::log10(power))<.003,
                "10/25/60 FPS uses actual sample intervals, including overlapping observations");
        }
        check(raw.frames==averaged.frames,"averaging never reduces frame count");
    }
    Probe raw(0,false), averaged(100,false);
    const float first = raw.feed(.01f,window);
    averaged.feed(.01f,window);
    raw.feed(.3f,10000);averaged.feed(.3f,10000);
    raw.backend.setPanFrameRate("0xe1000000",60);
    averaged.backend.setPanFrameRate("0xe1000000",60);
    const float input = raw.feed(.3f,40000);
    const double retained = std::exp(-50000/2400000.);
    const double expected = 10*std::log10(retained*std::pow(10.,first/10.)
        +(1-retained)*std::pow(10.,input/10.));
    check(std::abs(averaged.feed(.3f,40000)-expected)<.003,
        "mid-interval FPS change uses elapsed samples, not a guessed new frame duration");

    Probe fastRaw(0,false,5), fastAverage(1,false,5);
    const float strong=fastRaw.feed(.5f,window);
    fastAverage.feed(.5f,window);
    const float weak=fastRaw.feed(.00001f,480000);
    const double tail=std::exp(-20.);
    const double expectedWeak=10*std::log10(tail*std::pow(10.,strong/10.)
        +(1-tail)*std::pow(10.,weak/10.));
    check(std::abs(fastAverage.feed(.00001f,480000)-expectedWeak)<.003,
        "small averaging time preserves weak power and the residual strong-signal tail without float cancellation");

    QByteArray referenceAudio, averagedAudio;
    QObject::connect(&raw.ddc,&rtl::RtlSdrDdc::audioFrameReady,
        [&](const QByteArray& audio,const QByteArray&) { referenceAudio.append(audio); });
    QObject::connect(&averaged.ddc,&rtl::RtlSdrDdc::audioFrameReady,
        [&](const QByteArray& audio,const QByteArray&) { averagedAudio.append(audio); });
    raw.ddc.resetSpectrum();averaged.ddc.resetSpectrum();
    QVector<std::complex<float>> iq(8192);
    int detectors=0;
    for(int block=0;block<60;++block) {
        for(int i=0;i<iq.size();++i) {
            const double t=(block*iq.size()+i)/2400000.;
            iq[i]=std::polar(.2f,float(15*std::sin(2*std::numbers::pi*1000*t)));
        }
        if(block==20) { averaged.backend.setPanWeightedAverage("0xe1000000",true); }
        if(block==40) { averaged.backend.setPanAverage("0xe1000000",5); }
        raw.ddc.processIqData(iq);averaged.ddc.processIqData(iq);
        const auto a=raw.ddc.takeSquelchSpectrum(),b=averaged.ddc.takeSquelchSpectrum();
        check(a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin()),
            "averaging depth/mode changes preserve every raw squelch observation");
        if(!a.empty()) { ++detectors; }
    }
    check(detectors>4 && !referenceAudio.isEmpty() && referenceAudio==averagedAudio,
        "production audio stays byte-identical while display averaging changes");
}

void noiseStatistics()
{
    Probe raw(0,false), low(5,false), high(100,false);
    std::mt19937 generator(5782);
    std::normal_distribution<float> noise(0,.05f);
    QVector<std::complex<float>> iq(96000);
    double sum[3]{},squares[3]{};
    for(int frame=0;frame<500;++frame) {
        for(auto& sample:iq) { sample={noise(generator),noise(generator)}; }
        raw.ddc.processIqData(iq,false);low.ddc.processIqData(iq,false);high.ddc.processIqData(iq,false);
        if(frame<100) { continue; }
        const Probe* probes[]={&raw,&low,&high};
        for(int i=0;i<3;++i) {
            float db=0;std::memcpy(&db,probes[i]->last.constData()+1000*sizeof(float),sizeof(db));
            const double power=std::pow(10.,db/10.);
            sum[i]+=power;squares[i]+=power*power;
        }
    }
    double variance[3]{};
    for(int i=0;i<3;++i) { variance[i]=squares[i]/400-std::pow(sum[i]/400,2); }
    check(variance[1]<variance[0]*.7 && variance[2]<variance[1]*.3,
        "generated Gaussian-noise FFT bins become progressively less variable");
    check(std::abs(sum[2]/sum[0]-1)<.2,
        "strong power averaging preserves measured mean noise power within fixture sampling tolerance");
    std::printf("noise_variance_ratio low=%.6f high=%.6f mean_power_ratio=%.6f\n",
        variance[1]/variance[0],variance[2]/variance[0],sum[2]/sum[0]);
}

void partitionContinuity()
{
    Probe whole(73,false), partitioned(73,false);
    std::vector<QByteArray> a,b;
    QObject::connect(&whole.ddc,&rtl::RtlSdrDdc::spectrumFrameReady,
        [&](int,const QByteArray& frame) { a.push_back(frame); });
    QObject::connect(&partitioned.ddc,&rtl::RtlSdrDdc::spectrumFrameReady,
        [&](int,const QByteArray& frame) { b.push_back(frame); });
    QVector<std::complex<float>> iq(300000);
    for(int n=0;n<iq.size();++n) {
        iq[n]=std::polar(float(.1+.07*std::sin(n*.0001)),float(n*.037));
    }
    whole.ddc.processIqData(iq,false);
    for(int offset=0;offset<iq.size();offset+=257) {
        partitioned.ddc.processIqData(iq.sliced(offset,std::min(257,int(iq.size())-offset)),false);
    }
    check(a.size()==3 && a==b,"USB partitioning does not change averaged bins or sample deadlines");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    constexpr int window = rtl::RtlSdrDdc::kSpectrumBinCount;
    constexpr int stride = 96000; // 25 fps at 2.4 MS/s
    Probe raw(0, false), low(5, false), high(100, false), log(100, true);
    const float first = raw.feed(.01f, window);
    check(std::abs(low.feed(.01f, window)-first)<.001
        && std::abs(high.feed(.01f, window)-first)<.001
        && std::abs(log.feed(.01f, window)-first)<.001,
        "first complete observation seeds every estimator without zero bias");
    const float last = raw.feed(.5f, stride);
    const float lowValue = low.feed(.5f, stride), highValue = high.feed(.5f, stride);
    const float logValue = log.feed(.5f, stride);
    const auto expectedPower = [&](double tau) {
        const double retained = std::exp(-.04 / tau);
        return 10*std::log10(retained*std::pow(10.,first/10.)
            +(1-retained)*std::pow(10.,last/10.));
    };
    check(std::abs(lowValue-expectedPower(.05))<.002,
        "low averaging control reaches real FFT with a 50 ms power time constant");
    check(std::abs(highValue-expectedPower(1.))<.002,
        "high averaging control reaches real FFT with a one-second power time constant");
    check(last>lowValue+1 && lowValue>highValue+5,
        "disabled, low and high are measurably distinct");
    check(std::abs(logValue-(std::exp(-.04)*first+(1-std::exp(-.04))*last))<.002,
        "weighted toggle selects documented log-recursive mode");
    check(raw.frames==2 && low.frames==2 && high.frames==2
        && high.last==high.waterfall && log.last==log.waterfall,
        "averaging preserves cadence and supplies the same samples to spectrum and waterfall history");
    check(high.backend.capabilities().backendPanAveraging.has_value(),
        "backend averaging disables the widget's second EMA through its existing capability");
    high.backend.setPanAverage("0xe1000000",0);
    check(std::abs(high.feed(.5f,stride)-last)<.002,"zero disables averaging immediately");
    log.ddc.resetSpectrum();
    check(std::abs(log.feed(.5f,window)-last)<.002,"capture/discontinuity reset cannot retain old RF average");
    std::printf("step_db off=%.6f low=%.6f high=%.6f weighted=%.6f\n",last,lowValue,highValue,logValue);
    cadenceAndReceiverIndependence();
    noiseStatistics();
    partitionContinuity();
    return failures ? 1 : 0;
}
