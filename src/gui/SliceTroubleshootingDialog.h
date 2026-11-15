#pragma once

#include "PersistentDialog.h"
#include <QJsonObject>
#include <functional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTabWidget;

namespace AetherSDR {

class RadioModel;
class AudioEngine;

class SliceTroubleshootingDialog : public PersistentDialog {
    Q_OBJECT

public:
    using SnapshotProvider = std::function<QJsonObject()>;

    explicit SliceTroubleshootingDialog(RadioModel* model,
                                        AudioEngine* audio = nullptr,
                                        QWidget* parent = nullptr,
                                        SnapshotProvider controlDevicesProvider = {},
                                        SnapshotProvider rendererProvider = {});

private:
    void refreshSnapshot();
    void copySummary();
    void copyJson();
    void exportJson();
    void setStatusMessage(const QString& message);
    void updateSearchHighlights();
    void findNextMatch();
    QPlainTextEdit* activeTextView() const;

    static QString buildSummary(const QJsonObject& snapshot);

    RadioModel* m_model{nullptr};
    AudioEngine* m_audio{nullptr};
    SnapshotProvider m_controlDevicesProvider;
    SnapshotProvider m_rendererProvider;
    QJsonObject m_snapshot;
    QString m_summaryText;
    QString m_jsonText;

    QTabWidget* m_tabs{nullptr};
    QPlainTextEdit* m_summaryView{nullptr};
    QPlainTextEdit* m_jsonView{nullptr};
    QLineEdit* m_searchEdit{nullptr};
    QLabel* m_statusLabel{nullptr};
};

} // namespace AetherSDR
