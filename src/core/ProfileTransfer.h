#pragma once

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVersionNumber>

#include <optional>

class QFile;
class QSaveFile;
class QTcpServer;
class QTcpSocket;
class QTimer;

namespace MasterSDR {

class RadioModel;

struct MemoryGroupSelection {
    QString owner;
    QString group;
};

struct ExportSelection {
    bool globalProfiles{true};
    bool txProfiles{true};
    bool micProfiles{true};
    bool memories{false};
    bool preferences{false};
    bool tnf{false};
    bool xvtr{false};
    bool usbCables{false};

    QStringList globalProfileNames;
    QStringList txProfileNames;
    QStringList micProfileNames;
    QList<MemoryGroupSelection> memoryGroups;

    bool anySelected() const
    {
        return globalProfiles || txProfiles || micProfiles || memories
            || preferences || tnf || xvtr || usbCables;
    }
};

inline QString cleanMetaSubsetToken(QString token)
{
    token.replace(QLatin1Char('\r'), QLatin1Char(' '));
    token.replace(QLatin1Char('\n'), QLatin1Char(' '));
    token.replace(QLatin1Char('^'), QLatin1Char(' '));
    return token.trimmed();
}

inline QString cleanMemoryToken(QString token)
{
    token = cleanMetaSubsetToken(std::move(token));
    token.replace(QLatin1Char(' '), QChar(0x7f));
    return token;
}

inline void appendProfileSubsetLine(QByteArray& out, const char* tag, const QStringList& names)
{
    out += tag;
    out += '^';
    for (const QString& rawName : names) {
        const QString name = cleanMetaSubsetToken(rawName);
        if (name.isEmpty())
            continue;
        out += name.toUtf8();
        out += '^';
    }
    out += "\r\n";
}

inline QByteArray buildMetaSubset(const ExportSelection& selection)
{
    QByteArray out;

    if (selection.globalProfiles)
        appendProfileSubsetLine(out, "GLOBAL_PROFILES", selection.globalProfileNames);
    if (selection.txProfiles)
        appendProfileSubsetLine(out, "HW_PROFILES", selection.txProfileNames);
    if (selection.micProfiles)
        appendProfileSubsetLine(out, "MIC_PROFILES", selection.micProfileNames);
    if (selection.memories) {
        out += "MEMORIES^";
        for (const MemoryGroupSelection& memory : selection.memoryGroups) {
            const QString owner = cleanMemoryToken(memory.owner);
            const QString group = cleanMemoryToken(memory.group);
            if (owner.isEmpty() && group.isEmpty())
                continue;
            out += owner.toUtf8();
            out += '|';
            out += group.toUtf8();
            out += '^';
        }
        out += "\r\n";
    }
    if (selection.preferences) {
        out += "BAND_PERSISTENCE^\r\n";
        out += "MODE_PERSISTENCE^\r\n";
        out += "GLOBAL_PERSISTENCE^\r\n";
    }
    if (selection.tnf)
        out += "TNFS^\r\n";
    if (selection.xvtr)
        out += "XVTRS^\r\n";
    if (selection.usbCables)
        out += "USB_CABLES^\r\n";

    return out;
}

inline QVersionNumber parseSmartSdrVersionFromFilename(const QString& fileName)
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:ASDR|SSDR)_Config_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_v(\d+(?:\.\d+){1,3})\.ssdr_cfg$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = re.match(fileName);
    if (!match.hasMatch())
        return {};
    return QVersionNumber::fromString(match.captured(1));
}

inline QVersionNumber parseFirmwareVersion(const QString& text)
{
    static const QRegularExpression re(QStringLiteral(R"((\d+(?:\.\d+){0,3}))"));
    const auto match = re.match(text);
    if (!match.hasMatch())
        return {};
    return QVersionNumber::fromString(match.captured(1));
}

inline int compareFirmwareVersions(const QString& lhs, const QString& rhs)
{
    const QVersionNumber left = parseFirmwareVersion(lhs);
    const QVersionNumber right = parseFirmwareVersion(rhs);
    if (left.isNull() || right.isNull())
        return 0;

    const int cmp = QVersionNumber::compare(left.normalized(), right.normalized());
    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    return 0;
}

inline std::optional<quint16> parseTransferPort(const QString& replyBody)
{
    const QString trimmed = replyBody.trimmed();

    static const QRegularExpression keyedRe(
        QStringLiteral(R"((?:^|[\s,;|])(?:port|transfer_port|tcp_port)\s*[=:]?\s*(\d{2,5})(?=$|[^\d]))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto keyed = keyedRe.match(trimmed);
    if (keyed.hasMatch()) {
        bool ok = false;
        const int port = keyed.captured(1).toInt(&ok);
        if (ok && port > 0 && port <= 65535)
            return static_cast<quint16>(port);
    }

    bool bareOk = false;
    const int barePort = trimmed.toInt(&bareOk);
    if (bareOk && barePort > 0 && barePort <= 65535)
        return static_cast<quint16>(barePort);

    static const QRegularExpression endpointRe(QStringLiteral(R"(:(\d{2,5})(?=$|[^\d]))"));
    const auto endpoint = endpointRe.match(trimmed);
    if (endpoint.hasMatch()) {
        bool ok = false;
        const int port = endpoint.captured(1).toInt(&ok);
        if (ok && port > 0 && port <= 65535)
            return static_cast<quint16>(port);
    }

    static const QRegularExpression re(QStringLiteral(R"((?:^|[^\d])(\d{2,5})(?:[^\d]|$))"));
    auto it = re.globalMatch(trimmed);
    std::optional<quint16> lastPort;
    while (it.hasNext()) {
        const auto match = it.next();
        bool ok = false;
        const int port = match.captured(1).toInt(&ok);
        if (ok && port > 0 && port <= 65535)
            lastPort = static_cast<quint16>(port);
    }
    return lastPort;
}

class ProfileTransfer : public QObject {
    Q_OBJECT

public:
    enum class Operation {
        ExportDatabase,
        ImportDatabase
    };

    explicit ProfileTransfer(RadioModel* model, QObject* parent = nullptr);
    ~ProfileTransfer() override;

    bool isBusy() const { return m_busy; }

    void exportDatabase(const ExportSelection& selection, const QString& destinationPath);
    void importDatabase(const QString& ssdrCfgPath);
    void cancel();

signals:
    void started(MasterSDR::ProfileTransfer::Operation operation);
    void progress(QString status, qint64 bytesDone, qint64 bytesTotal);
    void finished(MasterSDR::ProfileTransfer::Operation operation, QString path);
    void failed(MasterSDR::ProfileTransfer::Operation operation, QString error);

private:
    enum class Phase {
        Idle,
        UploadMetaSubset,
        DownloadPackage,
        UploadImport,
        WaitingForImport
    };

    void begin(Operation operation, Phase phase);
    void fail(const QString& error);
    void finish(QString path);
    void cleanup();

    ExportSelection expandSelection(ExportSelection selection) const;
    bool validateCommonPreconditions(Operation operation, QString* error) const;
    bool validateExportSelection(const ExportSelection& selection, QString* error) const;
    bool validateExportDestination(const QString& path, QString* error) const;
    bool validateImportFile(const QString& path, QString* error) const;

    void requestUploadPort(const QByteArray& payload, const QString& uploadKind);
    void onUploadPortReceived(int code, const QString& body);
    void connectUploadSocket(quint16 port);
    void tryFallbackUploadPort();
    void onUploadConnected();
    void sendNextUploadChunk();
    void onUploadBytesWritten(qint64 bytes);
    void onUploadDisconnected();
    void onUploadError();

    void requestPackageDownload();
    void onDownloadPortReceived(int code, const QString& body);
    void startDownloadServer(quint16 port);
    void onDownloadConnection();
    void onDownloadReadyRead();
    void onDownloadDisconnected();
    void onDownloadError();

    void waitForImportCompletion();
    void scheduleImportCompletion();
    void completeImport();
    void resetIdleTimer();
    void handleTimeout();
    bool commitExportFile(QString* error);

    RadioModel* m_model{nullptr};
    Operation m_operation{Operation::ExportDatabase};
    Phase m_phase{Phase::Idle};
    bool m_busy{false};
    bool m_cancelled{false};
    bool m_usedFallbackPort{false};
    bool m_downloadFinalized{false};
    bool m_importCompletionScheduled{false};

    QByteArray m_uploadPayload;
    QString m_path;
    qint64 m_bytesDone{0};
    qint64 m_bytesQueued{0};
    qint64 m_bytesTotal{0};
    quint16 m_uploadPort{0};

    QTcpSocket* m_socket{nullptr};
    QTcpServer* m_server{nullptr};
    QSaveFile* m_saveFile{nullptr};
    QTimer* m_timeout{nullptr};
    QTimer* m_idleTimer{nullptr};
    QTimer* m_overallTimer{nullptr};

    static constexpr int kUploadChunkSize = 64 * 1024;
    static constexpr int kCommandTimeoutMs = 10000;
    static constexpr int kConnectTimeoutMs = 10000;
    static constexpr int kIdleTimeoutMs = 30000;
    static constexpr int kOverallTimeoutMs = 180000;
    static constexpr int kMetaSubsetSettleMs = 5000;
    static constexpr int kImportSettleMs = 5000;
    static constexpr quint16 kDefaultUploadPort = 4995;
    static constexpr quint16 kFallbackTransferPort = 42607;
    static constexpr qint64 kMaxImportSize = 250LL * 1024LL * 1024LL;
};

} // namespace MasterSDR
