// Offline test for RemoteAsrBackend (RFC #4338, Phase 6). Spins up a local
// QTcpServer that mimics an OpenAI-compatible /v1/audio/transcriptions endpoint,
// points the backend at it, and verifies the request shape + response parsing.
// Also checks the 16-bit WAV encoding. No network beyond loopback.

#include "asr/RemoteAsrBackend.h"

#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;
void expect(bool c, const char* d)
{
    std::printf("%s %s\n", c ? "[ OK ]" : "[FAIL]", d);
    if (!c) {
        ++g_failures;
    }
}

// Minimal HTTP mock: reads a full request (honouring Content-Length), records it,
// and replies with a fixed JSON body.
class MockTranscribeServer : public QTcpServer {
public:
    QByteArray responseJson;
    QByteArray lastRequest;
    bool gotRequest = false;

    explicit MockTranscribeServer(const QByteArray& json) : responseJson(json)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket* sock = nextPendingConnection();
            auto* buf = new QByteArray();
            connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
                buf->append(sock->readAll());
                const int headerEnd = buf->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                for (const QByteArray& line : buf->left(headerEnd).split('\n')) {
                    const QByteArray l = line.trimmed();
                    if (l.toLower().startsWith("content-length:")) {
                        contentLength = l.mid(15).trimmed().toInt();
                    }
                }
                if (buf->size() - (headerEnd + 4) < contentLength) {
                    return; // wait for the rest of the body
                }
                lastRequest = *buf;
                gotRequest = true;
                const QByteArray resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(responseJson.size()) + "\r\nConnection: close\r\n\r\n"
                    + responseJson;
                sock->write(resp);
                sock->flush();
                sock->disconnectFromHost();
                delete buf;
            });
        });
    }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- WAV encoding ------------------------------------------------------
    {
        std::vector<float> pcm(160, 0.5f); // 10 ms @ 16 kHz
        const QByteArray wav = RemoteAsrBackend::encodeWav16(pcm);
        expect(wav.left(4) == "RIFF" && wav.mid(8, 4) == "WAVE", "WAV has RIFF/WAVE tags");
        expect(wav.mid(12, 4) == "fmt " && wav.mid(36, 4) == "data", "WAV has fmt/data chunks");
        // sampleRate at offset 24 (LE), bits at 34, data size at 40
        const quint32 rate = static_cast<quint8>(wav[24]) | (static_cast<quint8>(wav[25]) << 8);
        expect(rate == 16000, "WAV sample rate is 16000");
        const quint16 bits = static_cast<quint8>(wav[34]) | (static_cast<quint8>(wav[35]) << 8);
        expect(bits == 16, "WAV is 16-bit");
        expect(wav.size() == 44 + 160 * 2, "WAV size = 44-byte header + 16-bit samples");
    }

    // ---- Remote transcription round-trip ----------------------------------
    {
        MockTranscribeServer server(
            R"({"text":"hello world","segments":[{"avg_logprob":-0.2}]})");
        expect(server.listen(QHostAddress::LocalHost), "mock server listening");
        const quint16 port = server.serverPort();

        RemoteAsrConfig cfg;
        cfg.url = QStringLiteral("http://127.0.0.1:%1/v1/audio/transcriptions").arg(port);
        cfg.model = QStringLiteral("whisper-1");
        cfg.timeoutMs = 5000;

        RemoteAsrBackend backend(cfg);
        QString err;
        expect(backend.load(QString(), &err), "load() succeeds with a configured URL");

        std::vector<float> pcm(1600, 0.25f); // 100 ms
        const AsrTranscript result = backend.transcribe(pcm, &err);
        expect(err.isEmpty(), "transcribe reports no error");
        expect(result.text == QStringLiteral("hello world"), "parsed text from the endpoint");
        expect(std::abs(result.confidence - std::exp(-0.2f)) < 0.02f,
               "confidence derived from avg_logprob");

        expect(server.gotRequest, "server received the request");
        expect(server.lastRequest.startsWith("POST "), "request is a POST");
        expect(server.lastRequest.contains("/v1/audio/transcriptions"), "request hits the endpoint path");
        expect(server.lastRequest.contains("filename=\"audio.wav\""), "request includes the WAV file part");
        expect(server.lastRequest.contains("name=\"model\""), "request includes the model field");
    }

    // ---- No URL configured -> load fails ----------------------------------
    {
        RemoteAsrBackend backend(RemoteAsrConfig{});
        QString err;
        expect(!backend.load(QString(), &err) && !err.isEmpty(),
               "load() fails when no endpoint is configured");
    }

    std::printf(g_failures == 0 ? "\nRemote ASR backend: ALL PASS\n"
                                : "\nRemote ASR backend: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
