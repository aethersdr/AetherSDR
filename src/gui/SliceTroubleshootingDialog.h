#pragma once

#include <QDialog>
#include <QJsonObject>
#include <functional>

class QLabel;
class QPlainTextEdit;

namespace MasterSDR {

class RadioModel;
class AudioEngine;

class SliceTroubleshootingDialog : public QDialog {
    Q_OBJECT

public:
    using SnapshotProvider = std::function<QJsonObject()>;

    explicit SliceTroubleshootingDialog(RadioModel* model,
                                        AudioEngine* audio = nullptr,
                                        QWidget* parent = nullptr,
                                        SnapshotProvider controlDevicesProvider = {});

private:
    void refreshSnapshot();
    void copySummary();
    void copyJson();
    void exportJson();
    void setStatusMessage(const QString& message);

    static QString buildSummary(const QJsonObject& snapshot);

    RadioModel* m_model{nullptr};
    AudioEngine* m_audio{nullptr};
    SnapshotProvider m_controlDevicesProvider;
    QJsonObject m_snapshot;
    QString m_summaryText;
    QString m_jsonText;

    QPlainTextEdit* m_summaryView{nullptr};
    QPlainTextEdit* m_jsonView{nullptr};
    QLabel* m_statusLabel{nullptr};
};

} // namespace MasterSDR
