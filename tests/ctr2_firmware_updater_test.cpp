// Ctr2FirmwareUpdater against a simulated device. The device side runs the
// MIT firmware reference (tools/ctr2-firmware-reference) for its framing, so
// the updater is held to the same session a device firmware runs: a
// FIRMWARE session opened in place of READY, Status for every message, a
// restart after End, and a new image that keeps itself only once it answers
// HELLO. No hidapi, no sockets, no device.

#include "core/Ctr2FirmwareUpdater.h"
#include "models/Ctr2ProxyModel.h"
#include "models/FirmwareInstallModel.h"
#include "core/Ctr2HidFraming.h"
#include "core/Ctr2HidPort.h"

extern "C" {
#include "ctr2_link.h"
}

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::ctr2hid;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!predicate()) {
            QThread::usleep(200);
        }
    }
    return predicate();
}

// What survives the device's restarts: its flash and its behavior.
struct Device {
    QString version{QStringLiteral("0.9.0")};
    bool pending{false};
    bool offersFirmware{true};
    bool reportsVersion{true};
    bool answersHello{true};
    bool rejectImage{false};       // End fails the signature check
    bool rollsBack{false};         // restarts on the old image after End
    int absentPolls{0};            // after a restart: enumerations it is missing from
    bool gone{false};
    QByteArray stored;
    QString incomingVersion;       // set from the image the session received
    int begins{0}, aborts{0}, closes{0}, hellos{0}, opens{0};
    bool hasOtherFirmware{true};   // SWITCH has somewhere to go
    bool hasBootloaderOp{true};    // firmware that answers BOOTLOADER
    QString restartedInto;         // "other" or "bootloader" after a restart request
    int dropsOnOpen{0};            // this open (1 = the first) fails before it negotiates
};

void collect(void* ctx, const std::uint8_t report[CTR2_REPORT_BYTES])
{
    static_cast<QByteArray*>(ctx)->append(reinterpret_cast<const char*>(report), CTR2_REPORT_BYTES);
}

// The device's end of one opened HID interface, as a device firmware
// behaves: answer HELLO, accept a FIRMWARE header in place of READY, answer
// every FIRMWARE message with Status.
class SimPort final : public Ctr2HidPort {
public:
    SimPort(Device* device, quint32 hostCaps)
        : m_dev(device)
    {
        const int open = ++m_dev->opens;
        ctr2_rx_init(&m_rx);
        const quint32 offered = m_dev->offersFirmware ? CTR2_CAP(CTR2_EXT_FIRMWARE) : 0;
        m_caps = offered & hostCaps;
        ctr2_rx_set_extensions(&m_rx, m_caps);
        QTimer::singleShot(0, this, [this, open, offered] {
            if (open == m_dev->dropsOnOpen) {
                m_open = false;
                emit failed(QStringLiteral("device left the bus"));
                return;
            }
            std::optional<deviceinfo::Info> info;
            if (m_dev->reportsVersion) {
                deviceinfo::Info i;
                const QStringList v = m_dev->version.split(QLatin1Char('.'));
                i.major = v.value(0).toInt();
                i.minor = v.value(1).toInt();
                i.patch = v.value(2).toInt();
                i.flags = (m_dev->pending ? deviceinfo::kPending : 0) | deviceinfo::kSigned;
                i.build = QStringLiteral("sim");
                info = i;
            }
            setExtensions(m_caps, info, offered);
        });
    }

    bool isOpen() const override { return m_open; }
    void send(const std::vector<Report>& reports) override
    {
        for (const Report& r : reports) {
            if (m_open) {
                onReport(r.data());
            }
        }
    }
    void discardQueued() override {}
    void shutdown(const std::vector<Report>& finalReports) override
    {
        send(finalReports);
        m_open = false;
        deleteLater();
    }
    QString description() const override { return QStringLiteral("Simulated CTR2-class device"); }

private:
    void reply(const QByteArray& reports)
    {
        QPointer<SimPort> self(this);
        QTimer::singleShot(0, this, [self, reports] {
            if (self && self->m_open) {
                emit self->reportsReceived(reports);
            }
        });
    }

    void onReport(const std::uint8_t* r)
    {
        if (r[0] == CTR2_MARKER && r[3] == CTR2_TYPE_HELLO) {
            ++m_dev->hellos;
            if (!m_dev->answersHello) {
                return;
            }
            m_dev->pending = false;   // a completed link keeps the image
            ctr2_tx_reset(&m_tx);
            QByteArray out;
            ctr2_tx_send(&m_tx, CTR2_TYPE_READY, nullptr, 0, collect, &out);
            reply(out);
            m_waiting = true;
            m_session = false;
            return;
        }
        if (m_waiting) {
            const bool firmware = r[0] == CTR2_MARKER && r[3] == CTR2_EXT_FIRMWARE
                && (m_caps & CTR2_CAP(CTR2_EXT_FIRMWARE));
            if (!firmware && (r[0] != CTR2_MARKER
                              || (r[3] != CTR2_TYPE_READY && r[3] != CTR2_TYPE_CLOSED))) {
                return;
            }
            m_waiting = false;
            ctr2_rx_reset(&m_rx);
            if (firmware) {
                ctr2_rx_start(&m_rx, r[2]);
                m_session = true;
            }
        }
        std::uint8_t type = 0;
        const std::uint8_t* data = nullptr;
        std::uint16_t len = 0;
        const ctr2_rx_result res = ctr2_rx_feed(&m_rx, r, &type, &data, &len);
        check(res != CTR2_RX_ERROR, "the device never sees a framing error");
        if (res != CTR2_RX_MESSAGE) {
            return;
        }
        if (type == CTR2_TYPE_CLOSED) {
            ++m_dev->closes;
            m_session = false;
            m_waiting = true;
        } else if (type == CTR2_EXT_FIRMWARE && m_session) {
            onFirmware(data, len);
        }
    }

    void status(std::uint8_t op, std::uint8_t code)
    {
        const quint32 v = static_cast<quint32>(m_dev->stored.size());
        const std::uint8_t s[CTR2_FW_STATUS_LEN] = {CTR2_FW_STATUS, op, code,
            std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)};
        QByteArray out;
        ctr2_tx_send_extension(&m_tx, CTR2_EXT_FIRMWARE, s, sizeof s, collect, &out);
        reply(out);
    }

    void onFirmware(const std::uint8_t* d, std::uint16_t len)
    {
        switch (d[0]) {
        case CTR2_FW_BEGIN:
            ++m_dev->begins;
            m_size = (quint32(d[1]) << 24) | (quint32(d[2]) << 16) | (quint32(d[3]) << 8) | d[4];
            m_sha = QByteArray(reinterpret_cast<const char*>(d + 5), 32);
            m_dev->stored.clear();
            m_active = true;
            status(CTR2_FW_BEGIN, CTR2_FW_OK);
            break;
        case CTR2_FW_DATA: {
            const quint32 off = (quint32(d[1]) << 24) | (quint32(d[2]) << 16) | (quint32(d[3]) << 8) | d[4];
            if (!m_active || off != quint32(m_dev->stored.size())) {
                status(CTR2_FW_DATA, CTR2_FW_BAD_OFFSET);
                break;
            }
            m_dev->stored.append(reinterpret_cast<const char*>(d + CTR2_FW_DATA_HEADER),
                                 len - CTR2_FW_DATA_HEADER);
            status(CTR2_FW_DATA, CTR2_FW_OK);
            break;
        }
        case CTR2_FW_END: {
            m_active = false;
            if (QCryptographicHash::hash(m_dev->stored, QCryptographicHash::Sha256) != m_sha
                || quint32(m_dev->stored.size()) != m_size) {
                status(CTR2_FW_END, CTR2_FW_HASH_MISMATCH);
                break;
            }
            if (m_dev->rejectImage) {
                status(CTR2_FW_END, CTR2_FW_REJECTED);
                break;
            }
            status(CTR2_FW_END, CTR2_FW_OK);
            {
                // Restarts on the new image (or, simulating a rollback, the
                // old one) whatever the host does next.
                Image im;
                QString problem;
                Ctr2FirmwareUpdater::readImage(m_dev->stored, &im, &problem);
                if (!m_dev->rollsBack) {
                    m_dev->version = im.reportedVersion;   // as its sscanf reads it
                    m_dev->pending = true;
                }
                m_dev->gone = true;
            }
            // ...and leaves the bus, if the host has not closed this port yet.
            QTimer::singleShot(5, this, [this] {
                if (m_open) {
                    m_open = false;
                    emit failed(QStringLiteral("device left the bus"));
                }
            });
            break;
        }
        case CTR2_FW_ABORT:
            ++m_dev->aborts;
            m_active = false;
            status(CTR2_FW_ABORT, CTR2_FW_OK);
            break;
        case CTR2_FW_SWITCH:
        case CTR2_FW_BOOTLOADER: {
            const bool loader = d[0] == CTR2_FW_BOOTLOADER;
            if (loader ? !m_dev->hasBootloaderOp : false) {
                status(d[0], CTR2_FW_BAD_MESSAGE);
                break;
            }
            if (!loader && !m_dev->hasOtherFirmware) {
                status(d[0], CTR2_FW_BAD_STATE);
                break;
            }
            status(d[0], CTR2_FW_OK);
            m_dev->restartedInto = loader ? QStringLiteral("bootloader") : QStringLiteral("other");
            m_dev->gone = true;
            QTimer::singleShot(5, this, [this] {
                if (m_open) {
                    m_open = false;
                    emit failed(QStringLiteral("device left the bus"));
                }
            });
            break;
        }
        default:
            status(d[0], CTR2_FW_BAD_MESSAGE);
            break;
        }
    }

    using Image = Ctr2FirmwareUpdater::Image;
    Device* m_dev;
    quint32 m_caps{0};
    ctr2_rx m_rx{};
    ctr2_tx m_tx{};
    bool m_open{true};
    bool m_waiting{true};
    bool m_session{false};
    bool m_active{false};
    quint32 m_size{0};
    QByteArray m_sha;
};

Ctr2HidPort::DeviceInfo simInfo()
{
    Ctr2HidPort::DeviceInfo d;
    d.path = QStringLiteral("/sim/hidraw0");
    d.vendorId = 0x303A;
    d.productId = 0x1001;
    d.product = QStringLiteral("AetherSim");
    d.serial = QStringLiteral("0123456789AB");
    return d;
}

// An ESP32-S3 application image as the updater expects one.
QByteArray makeImage(const char* version, int size = 5000, const char* project = "simfw")
{
    QByteArray b(size, '\0');
    for (int i = 0; i < size; ++i) {
        b[i] = static_cast<char>((i * 37 + 11) & 0xFF);
    }
    b[0] = static_cast<char>(0xE9);
    b[12] = 9;
    b[13] = 0;
    const std::uint8_t magic[4] = {0x32, 0x54, 0xCD, 0xAB};
    std::memcpy(b.data() + 32, magic, 4);
    std::memset(b.data() + 48, 0, 64);
    std::memcpy(b.data() + 48, version, std::strlen(version));
    std::memcpy(b.data() + 80, project, std::strlen(project));
    return b;
}

struct Run {
    bool done{false};
    bool ok{false};
    QString message;
    QString version;
    qint64 lastStored{0};
    int progressEvents{0};
    QStringList steps;
};

Ctr2FirmwareUpdater::Timing fastTiming()
{
    Ctr2FirmwareUpdater::Timing t;
    t.helloRetryMs = 30;
    t.answerTimeoutMs = 400;
    t.endTimeoutMs = 400;
    t.returnTimeoutMs = 1500;
    t.firstLookMs = 20;
    t.pollMs = 20;
    return t;
}

// One update of `device` with `image`; returns what finished() reported.
// setup runs before the update starts, with the updater and the run record.
Run update(Device& device, const QByteArray& image, int window = 4,
           const std::function<void(Ctr2FirmwareUpdater&, Run&)>& setup = {})
{
    Ctr2FirmwareUpdater updater(
        [&device] {
            if (device.gone && device.absentPolls-- > 0) {
                return QList<Ctr2HidPort::DeviceInfo>();
            }
            device.gone = false;
            return QList<Ctr2HidPort::DeviceInfo>{simInfo()};
        },
        [&device](const Ctr2HidPort::DeviceInfo&, QString*) -> Ctr2HidPort* {
            return new SimPort(&device, capabilityBit(MessageType::Firmware));
        });
    updater.setTiming(fastTiming());
    updater.setWindow(window);
    Run run;
    QObject::connect(&updater, &Ctr2FirmwareUpdater::finished,
                     [&run](bool ok, const QString& message, const QString& version) {
        run.done = true;
        run.ok = ok;
        run.message = message;
        run.version = version;
    });
    QObject::connect(&updater, &Ctr2FirmwareUpdater::progress, [&run](qint64 stored, qint64) {
        run.lastStored = stored;
        ++run.progressEvents;
    });
    QObject::connect(&updater, &Ctr2FirmwareUpdater::stepChanged,
                     [&run](const QString& step) { run.steps << step; });
    if (setup) {
        setup(updater, run);
    }
    Ctr2FirmwareUpdater::Image im;
    QString problem;
    check(Ctr2FirmwareUpdater::readImage(image, &im, &problem), "the test image reads");
    check(updater.start(simInfo(), im), "the update starts");
    check(waitUntil([&run] { return run.done; }), "the update finishes");
    check(!updater.isRunning(), "the updater is idle afterwards");
    return run;
}

void testReadImage()
{
    Ctr2FirmwareUpdater::Image im;
    QString problem;
    check(Ctr2FirmwareUpdater::readImage(makeImage("1.2.3"), &im, &problem)
              && im.version == QStringLiteral("1.2.3") && im.project == QStringLiteral("simfw"),
          "readImage takes an application image and its version and project");
    check(!Ctr2FirmwareUpdater::readImage(QByteArray(5000, 'x'), &im, &problem) && !problem.isEmpty(),
          "readImage refuses a file that is not a firmware image");
    QByteArray merged = makeImage("1.0.0");
    merged[32] = 0;   // a bootloader or merged image: no app description at 32
    check(!Ctr2FirmwareUpdater::readImage(merged, &im, &problem)
              && problem.contains(QStringLiteral("not an application image")),
          "readImage refuses a merged or bootloader image");
    QByteArray otherChip = makeImage("1.0.0");
    otherChip[12] = 0;   // ESP32
    check(!Ctr2FirmwareUpdater::readImage(otherChip, &im, &problem),
          "readImage refuses an image for another chip");
    check(Ctr2FirmwareUpdater::readImage(makeImage("1.2.3-rc1"), &im, &problem)
              && im.version == QStringLiteral("1.2.3-rc1")
              && im.reportedVersion == QStringLiteral("1.2.3"),
          "a decorated version is checked as the device reports it");
    check(!Ctr2FirmwareUpdater::readImage(makeImage("v1.2.3"), &im, &problem)
              && problem.contains(QStringLiteral("major.minor.patch")),
          "readImage refuses a version the device could not report");
    check(!Ctr2FirmwareUpdater::readImage(makeImage("1.256.0"), &im, &problem),
          "readImage refuses a version part over 255");
    check(!Ctr2FirmwareUpdater::readImage(makeImage("4294967296.1.2"), &im, &problem),
          "readImage refuses a version part too large to be a number");
    check(!Ctr2FirmwareUpdater::readImage(makeImage("1.0.0", Ctr2FirmwareUpdater::kMaxImageBytes + 1),
                                          &im, &problem),
          "readImage refuses an image larger than a slot");
}

void testSuccess()
{
    for (int window : {1, 4}) {
        Device dev;
        const QByteArray image = makeImage("0.9.1", 3 * firmware::kChunkBytes + 77);
        const Run run = update(dev, image, window);
        check(run.ok && run.version == QStringLiteral("0.9.1"),
              "the update succeeds and reports the new version");
        check(dev.stored == image, "the device stored the image byte for byte");
        check(dev.version == QStringLiteral("0.9.1") && !dev.pending,
              "the device runs the new image and has kept it");
        check(run.lastStored == image.size() && run.progressEvents == 4,
              "progress reaches the image size, once per chunk");
        check(dev.begins == 1 && dev.aborts == 0, "one session, no abort");
        check(dev.opens == 3, "opened for the session, after the restart, and to verify");
    }
}

void testDecoratedVersion()
{
    Device dev;
    const Run run = update(dev, makeImage("0.9.3-rc1"));
    check(run.ok && run.version == QStringLiteral("0.9.3"),
          "an image whose version has a suffix updates and is confirmed");
}

void testDeviceComesBackLater()
{
    Device dev;
    dev.absentPolls = 10;   // the device takes a while to re-enumerate
    const Run run = update(dev, makeImage("0.9.2"));
    check(run.ok && dev.version == QStringLiteral("0.9.2") && !dev.pending,
          "the updater waits for the device to come back");
}

// The device drops off the bus while the new firmware is being checked (the
// new image bouncing USB): the updater keeps looking and still reports success.
void testDropWhileVerifying()
{
    Device dev;
    dev.dropsOnOpen = 3;   // the session, after the restart, then this check
    const Run run = update(dev, makeImage("0.9.2"));
    check(run.ok && run.version == QStringLiteral("0.9.2"),
          "a drop while verifying does not turn a kept update into a failure");
    check(dev.opens == 4, "the updater opened the device again to verify");
}

void testRejected()
{
    Device dev;
    dev.rejectImage = true;
    const Run run = update(dev, makeImage("0.9.1"));
    check(!run.ok && run.message.contains(QStringLiteral("refused"))
              && run.message.contains(QStringLiteral("keeps the firmware it had")),
          "a rejected image fails and says the device keeps its firmware");
    check(dev.version == QStringLiteral("0.9.0"), "the device still runs the old image");
    check(dev.closes == 1, "the session is closed, so the device leaves its update screen");
}

void testNoFirmwareExtension()
{
    Device dev;
    dev.offersFirmware = false;
    const Run run = update(dev, makeImage("0.9.1"));
    check(!run.ok && run.message.contains(QStringLiteral("cannot be updated")),
          "a device that does not offer Firmware is refused before anything is sent");
    check(dev.begins == 0 && dev.hellos == 0, "nothing reached the device");
}

void testNoAnswer()
{
    Device dev;
    dev.answersHello = false;
    const Run run = update(dev, makeImage("0.9.1"));
    check(!run.ok && run.message.contains(QStringLiteral("did not answer")),
          "a device that never answers HELLO times out");
    check(dev.hellos >= 2, "HELLO is repeated while waiting");
}

void testRollback()
{
    Device dev;
    dev.rollsBack = true;
    const Run run = update(dev, makeImage("0.9.1"));
    check(!run.ok && run.message.contains(QStringLiteral("0.9.0")),
          "a device that comes back on its old image is reported, naming it");
}

void testCancel()
{
    Device dev;
    const QByteArray image = makeImage("0.9.1", 40 * firmware::kChunkBytes);
    const Run run = update(dev, image, 1, [](Ctr2FirmwareUpdater& u, Run&) {
        QObject::connect(&u, &Ctr2FirmwareUpdater::progress, &u, [&u](qint64 stored, qint64) {
            if (stored >= 3 * firmware::kChunkBytes) {
                u.cancel();
            }
        });
    });
    check(!run.ok && run.message.contains(QStringLiteral("cancelled")),
          "cancel mid-transfer fails the update");
    check(dev.aborts == 1 && dev.closes == 1, "the device gets ABORT and CLOSED");
    check(dev.version == QStringLiteral("0.9.0"), "the device keeps its firmware after a cancel");
}

Run restart(Device& dev, Ctr2FirmwareUpdater::Restart target)
{
    Ctr2FirmwareUpdater updater(
        [] { return QList<Ctr2HidPort::DeviceInfo>{simInfo()}; },
        [&dev](const Ctr2HidPort::DeviceInfo&, QString*) -> Ctr2HidPort* {
            return new SimPort(&dev, capabilityBit(MessageType::Firmware));
        });
    updater.setTiming(fastTiming());
    Run run;
    QObject::connect(&updater, &Ctr2FirmwareUpdater::finished,
                     [&run](bool ok, const QString& message, const QString&) {
                         run.done = true;
                         run.ok = ok;
                         run.message = message;
                     });
    check(updater.restartDevice(simInfo(), target), "the restart request starts");
    check(waitUntil([&run] { return run.done; }), "the restart request finishes");
    return run;
}

void testRestartRequests()
{
    using R = Ctr2FirmwareUpdater::Restart;
    Device dev;
    Run run = restart(dev, R::OtherFirmware);
    check(run.ok && dev.restartedInto == QStringLiteral("other"),
          "SWITCH restarts the device into its other firmware");
    check(dev.begins == 0 && dev.stored.isEmpty(), "and sends no image");

    Device loader;
    run = restart(loader, R::Bootloader);
    check(run.ok && loader.restartedInto == QStringLiteral("bootloader"),
          "BOOTLOADER restarts the device into download mode");

    Device old;
    old.hasBootloaderOp = false;
    run = restart(old, R::Bootloader);
    check(!run.ok && run.message.contains(QStringLiteral("BOOT")) && old.restartedInto.isEmpty(),
          "firmware without BOOTLOADER says to use the buttons");

    Device alone;
    alone.hasOtherFirmware = false;
    run = restart(alone, R::OtherFirmware);
    check(!run.ok && alone.restartedInto.isEmpty(), "SWITCH with no other firmware fails");
    check(alone.closes == 1, "and the session is closed");
}

// AetherSDR quitting mid-update destroys the updater: the device must still
// hear ABORT and CLOSED, not be left waiting for the next chunk.
void testDestroyedMidUpdate()
{
    Device dev;
    const QByteArray image = makeImage("0.9.1", 40 * firmware::kChunkBytes);
    auto* updater = new Ctr2FirmwareUpdater(
        [] { return QList<Ctr2HidPort::DeviceInfo>{simInfo()}; },
        [&dev](const Ctr2HidPort::DeviceInfo&, QString*) -> Ctr2HidPort* {
            return new SimPort(&dev, capabilityBit(MessageType::Firmware));
        });
    updater->setTiming(fastTiming());
    updater->setWindow(1);
    QPointer<Ctr2FirmwareUpdater> alive(updater);
    bool phaseWasSending = false;
    QObject::connect(updater, &Ctr2FirmwareUpdater::progress, updater,
                     [updater, &phaseWasSending](qint64 stored, qint64) {
                         if (stored >= 3 * firmware::kChunkBytes && !phaseWasSending) {
                             phaseWasSending = true;
                             updater->deleteLater();   // as the app's teardown would
                         }
                     });
    Ctr2FirmwareUpdater::Image im;
    QString problem;
    check(Ctr2FirmwareUpdater::readImage(image, &im, &problem), "the test image reads");
    check(updater->start(simInfo(), im), "the update starts");
    check(waitUntil([&alive] { return alive.isNull(); }), "the updater is destroyed mid-transfer");
    check(waitUntil([&dev] { return dev.aborts == 1 && dev.closes == 1; }),
          "destroying the updater mid-transfer sends ABORT and CLOSED");
    check(dev.version == QStringLiteral("0.9.0"), "the device keeps its firmware");
}

} // namespace

// The CTR2-Max's stock firmware once it takes the link's FIRMWARE commands
// (SWITCH, BOOTLOADER) but not updates or a version report: the CTR2 window
// restarts it into AetherCTR2 on request, as AetherCTR2 itself, with no
// download mode and no buttons. Firmware without the commands still waits
// for the buttons.
void testStockFirmwareCommands()
{
    using Firmware = FirmwareInstallModel::Firmware;
    Ctr2HidPort::DeviceInfo stockInfo;
    stockInfo.path = QStringLiteral("/sim/stock");
    stockInfo.vendorId = 0x303A;
    stockInfo.productId = 0x1001;
    stockInfo.product = QStringLiteral("ESP32S3_DEV");   // no serial: no MAC

    const auto run = [&](Device& dev, Firmware firmware, bool* waited) {
        Ctr2ProxyModel relay;
        relay.setUsbBackend(
            [stockInfo] { return QList<Ctr2HidPort::DeviceInfo>{stockInfo}; },
            [](const Ctr2HidPort::DeviceInfo&, QString*) -> Ctr2HidPort* { return nullptr; },
            [&dev](const Ctr2HidPort::DeviceInfo&, quint32 caps, QString*) -> Ctr2HidPort* {
                return new SimPort(&dev, caps);
            });
        relay.setTransport(Ctr2ProxyModel::Transport::Usb);
        relay.setUsbDevicePath(stockInfo.path);
        relay.firmwareUpdater()->setTiming(fastTiming());
        waitUntil([&dev] { return dev.opens > 0; });   // the version probe
        waitUntil([] { return false; }, 100);
        FirmwareInstallModel model(&relay);
        model.setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                               [stockInfo] { return QList<Ctr2HidPort::DeviceInfo>{stockInfo}; });
        bool done = false, ok = false;
        QObject::connect(&model, &FirmwareInstallModel::finished, &model,
                         [&](bool o, const QString&) { done = true; ok = o; });
        QString problem;
        const bool takes = model.takesCommands(model.target());
        const bool started = model.restartInto(model.target(), firmware, &problem);
        *waited = model.waitingForDownloadMode();
        if (started && !*waited)
            waitUntil([&done] { return done; });
        model.cancel();
        return takes && started && ok;
    };

    bool waited = false;
    Device stock;
    stock.reportsVersion = false;   // no version report, no updates: only the commands
    check(run(stock, Firmware::AetherCtr2, &waited) && !waited
              && stock.restartedInto == QStringLiteral("other"),
          "stock firmware with the commands restarts into AetherCTR2 on request (SWITCH)");
    Device toLoader;
    toLoader.reportsVersion = false;
    check(run(toLoader, Firmware::Bootloader, &waited) && !waited
              && toLoader.restartedInto == QStringLiteral("bootloader"),
          "and into download mode (BOOTLOADER), with no buttons");

    Device plain;
    plain.reportsVersion = false;
    plain.offersFirmware = false;   // v1.02.00n and earlier
    run(plain, Firmware::AetherCtr2, &waited);
    check(waited && plain.restartedInto.isEmpty(),
          "stock firmware without them waits for download mode (the buttons), sending nothing");

    // AetherCTR2 too is judged by its offer: firmware without the commands
    // gets the buttons, not a command it would not answer.
    stockInfo.product = QStringLiteral("AetherCTR2");
    stockInfo.serial = QStringLiteral("B81F3FC45888");
    Device old;
    old.offersFirmware = false;
    run(old, Firmware::Stock, &waited);
    check(waited && old.restartedInto.isEmpty(),
          "AetherCTR2 without the commands waits for the buttons, sending nothing");
    stockInfo.product = QStringLiteral("ESP32S3_DEV");
    stockInfo.serial.clear();

    // A request whose device cannot be opened fails at once: reported, and
    // not left waiting for download mode behind the failure.
    {
        Device dev;
        dev.reportsVersion = false;
        int opens = 0;
        Ctr2ProxyModel relay;
        relay.setUsbBackend(
            [stockInfo] { return QList<Ctr2HidPort::DeviceInfo>{stockInfo}; },
            [](const Ctr2HidPort::DeviceInfo&, QString*) -> Ctr2HidPort* { return nullptr; },
            [&dev, &opens](const Ctr2HidPort::DeviceInfo&, quint32 caps,
                           QString* error) -> Ctr2HidPort* {
                if (++opens > 1) {   // the version probe opens it; the request cannot
                    *error = QStringLiteral("Permission denied");
                    return nullptr;
                }
                return new SimPort(&dev, caps);
            });
        relay.setTransport(Ctr2ProxyModel::Transport::Usb);
        relay.setUsbDevicePath(stockInfo.path);
        waitUntil([&dev] { return dev.opens > 0; });
        waitUntil([] { return false; }, 100);
        FirmwareInstallModel model(&relay);
        model.setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                               [stockInfo] { return QList<Ctr2HidPort::DeviceInfo>{stockInfo}; });
        bool failed = false;
        QObject::connect(&model, &FirmwareInstallModel::finished, &model,
                         [&](bool o, const QString&) { failed = !o; });
        QString problem;
        model.restoreBackup(model.target(), QStringLiteral("/nonexistent.bin"), &problem);
        waitUntil([&failed] { return failed; }, 1000);
        check(failed && !model.waitingForDownloadMode() && !model.busy(),
              "a restart that fails at once is reported, and nothing is left waiting");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testReadImage();
    testSuccess();
    testDeviceComesBackLater();
    testDecoratedVersion();
    testDropWhileVerifying();
    testRejected();
    testNoFirmwareExtension();
    testNoAnswer();
    testRollback();
    testCancel();
    testDestroyedMidUpdate();
    testRestartRequests();
    testStockFirmwareCommands();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_firmware_updater_test: all checks passed\n");
    return 0;
}
