// deepcw_replay — replay a recorded CW WAV through the DeepCW engine offline.
//
// The regression harness asked for on RFC #4817: run a recorded take through
// the decode paths and compare the text they produce on identical audio.
//
//   deepcw_replay <model.onnx> <in.wav> <outPrefix>
//       <outPrefix>-window.txt  each 15 s window decoded once, whole (what the
//                               model reads with full context; no streaming)
//       <outPrefix>-grow.txt    the pre-DeepCwCommitter loop (grow to 15 s,
//                               prefix-extension emission, hard reset) as the
//                               panel showed it via QTextEdit::insertHtml
//   deepcw_replay <model.onnx> <in.wav> <outPrefix> commit [holdSec]
//       <outPrefix>-commit-H<hold>.txt  the shipped path: DeepCwCommitter fed
//                               in 200 ms drains, exactly as decodeLoopDeep()
//
// Input: PCM16 WAV at 24 kHz, mono or stereo (the QSO recorder's format).

#include "core/DeepCwCommitter.h"
#include "core/DeepCwEngine.h"
#include "core/Resampler.h"

#include <QApplication>
#include <QByteArray>
#include <QTextCursor>
#include <QTextEdit>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using AetherSDR::DeepCwCommitter;
using AetherSDR::DeepCwEngine;
using AetherSDR::Resampler;

namespace {

constexpr int kRate = DeepCwEngine::kModelSampleRate;
constexpr size_t kDrain = 4800;   // 200 ms of 24 kHz, the worker's drain cadence

bool readWav(const char* path, std::vector<float>& mono24k)
{
    std::ifstream f(path, std::ios::binary);
    char riff[12];
    if (!f.read(riff, 12) || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4)) return false;
    uint16_t channels = 0, bits = 0, fmt = 0;
    uint32_t rate = 0;
    while (f) {
        char id[4];
        uint32_t len = 0;
        f.read(id, 4);
        f.read(reinterpret_cast<char*>(&len), 4);
        if (!f) break;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<char> b(len);
            f.read(b.data(), len);
            std::memcpy(&fmt, b.data(), 2);
            std::memcpy(&channels, b.data() + 2, 2);
            std::memcpy(&rate, b.data() + 4, 4);
            std::memcpy(&bits, b.data() + 14, 2);
        } else if (!std::memcmp(id, "data", 4)) {
            if (fmt != 1 || bits != 16 || rate != 24000 || channels < 1 || channels > 2) {
                std::fprintf(stderr, "need PCM16 24 kHz mono/stereo\n");
                return false;
            }
            std::vector<int16_t> s(len / 2);
            f.read(reinterpret_cast<char*>(s.data()), static_cast<std::streamsize>(s.size() * 2));
            const size_t n = s.size() / channels;
            mono24k.resize(n);
            for (size_t i = 0; i < n; ++i) {   // same downmix as CwDecoder::feedAudio
                const float l = s[i * channels] / 32768.0f;
                const float r = channels == 2 ? s[i * channels + 1] / 32768.0f : l;
                mono24k[i] = (l + r) * 0.5f;
            }
            return true;
        } else {
            f.seekg(len + (len & 1), std::ios::cur);
        }
    }
    return false;
}

// 24 kHz -> 3200 Hz in drain-sized blocks, as the worker does.
std::vector<std::vector<float>> resampleInDrains(const std::vector<float>& in24k)
{
    Resampler rs(24000.0, kRate);
    std::vector<std::vector<float>> out;
    for (size_t off = 0; off < in24k.size(); off += kDrain) {
        const int n = static_cast<int>(std::min(kDrain, in24k.size() - off));
        const QByteArray o = rs.process(in24k.data() + off, n);
        const auto* r = reinterpret_cast<const float*>(o.constData());
        out.emplace_back(r, r + o.size() / int(sizeof(float)));
    }
    return out;
}

bool writeText(const std::string& path, const std::string& header, const std::string& body)
{
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "%s\n%s\n", header.c_str(), body.c_str());
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <model.onnx> <in.wav> <outPrefix> [commit [holdSec]]\n", argv[0]);
        return 2;
    }
    const std::string out = argv[3];
    DeepCwEngine eng;
    if (!eng.loadModel(argv[1])) return 1;
    std::vector<float> in24k;
    if (!readWav(argv[2], in24k)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    const auto drains = resampleInDrains(in24k);

    if (argc > 4 && std::string(argv[4]) == "commit") {
        const double hold = argc > 5 ? std::atof(argv[5]) : 5.0;
        DeepCwCommitter c(hold);
        std::string text;
        for (const auto& d : drains) text += c.push(d.data(), d.size(), eng).text;
        text += c.flush(eng).text;
        char name[48];
        std::snprintf(name, sizeof name, "-commit-H%.0f.txt", hold);
        writeText(out + name, "hold=" + std::to_string(hold), text);
        std::printf("commit hold=%.1f s: %zu chars\n", hold, text.size());
        return 0;
    }

    // Whole 15 s windows, each decoded once.
    std::vector<float> all;
    for (const auto& d : drains) all.insert(all.end(), d.begin(), d.end());
    const size_t win = static_cast<size_t>(kRate) * 15;
    std::string windows;
    for (size_t s0 = 0; s0 + static_cast<size_t>(kRate * DeepCwEngine::kMinWindowSec) <= all.size(); s0 += win) {
        const std::vector<float> seg(all.begin() + s0, all.begin() + std::min(all.size(), s0 + win));
        windows += eng.decode(seg, kRate) + " | ";
    }
    writeText(out + "-window.txt", "15 s windows, each decoded once ( | = window boundary)", windows);

    // The pre-DeepCwCommitter loop, rendered the way the panel rendered it.
    const size_t minDecode = kRate * 5, hop = kRate * 2, maxSeg = kRate * 15;
    std::vector<float> seg;
    std::string emitted;
    size_t lastDecode = 0;
    int dropped = 0;
    QTextEdit panel;
    for (const auto& d : drains) {
        seg.insert(seg.end(), d.begin(), d.end());
        if (seg.size() >= minDecode && seg.size() >= lastDecode + hop) {
            const std::string text = eng.decode(seg, kRate);
            lastDecode = seg.size();
            if (text.size() >= emitted.size() && text.compare(0, emitted.size(), emitted) == 0) {
                const std::string delta = text.substr(emitted.size());
                if (!delta.empty()) {
                    panel.moveCursor(QTextCursor::End);
                    panel.insertHtml(QString("<span>%1</span>")
                                         .arg(QString::fromStdString(delta).toHtmlEscaped()));
                    emitted = text;
                }
            } else {
                ++dropped;   // divergent revision: adopted silently, never shown
                emitted = text;
            }
        }
        if (seg.size() >= maxSeg) { seg.clear(); emitted.clear(); lastDecode = 0; }
    }
    writeText(out + "-grow.txt",
              "pre-commit loop as shown via insertHtml; divergent revisions dropped: " + std::to_string(dropped),
              panel.toPlainText().toStdString());
    std::printf("window + grow views written; %d divergent revisions dropped\n", dropped);
    return 0;
}
