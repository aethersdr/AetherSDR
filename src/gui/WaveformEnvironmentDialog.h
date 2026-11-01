#pragma once

#include "CanonWindow.h"
#include "core/backends/WaveformEnvRequest.h"

#include <QList>
#include <QMap>
#include <QPointer>
#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;

// Why the Environment… action is unavailable, or empty when it can be used.
QString waveformEnvironmentUnavailableReason(const RadioModel* model);

// Edits one Docker waveform container's environment (SmartSDR 4.3.16, Waveforms
// → Environment…). The rows start from what the radio reports; Apply & Recreate
// hands the validated rows to the apply handler, which the Waveforms dialog points
// at RadioModel::applyWaveformEnvironment so the outcome still reaches its row if
// this window is closed first. Nothing is kept locally: after an apply the rows
// show the radio's report again (Principle II). Values may be secrets, so they are
// masked until Show values is ticked.
class WaveformEnvironmentDialog : public CanonWindow {
    Q_OBJECT

public:
    WaveformEnvironmentDialog(RadioModel* model, const QString& containerName,
                              QWidget* parent = nullptr);

    // Validates the rows; on success fills environment and returns an empty
    // string, otherwise returns the reason for the operator. Blank rows are
    // skipped. Public for the dialog test.
    static QString collectEnvironment(const QList<QPair<QString, QString>>& rows,
                                      WaveformEnvironment* environment);

    // Starts an apply; returns false when it was refused before anything was
    // sent. The owner reports the outcome back through showApplyResult().
    using ApplyHandler = std::function<bool(const QString& name, const WaveformEnvironment&)>;
    void setApplyHandler(ApplyHandler handler) { m_applyHandler = std::move(handler); }
    void showApplyResult(const WaveformEnvApplyResult& result);

private:
    struct Row {
        QWidget* widget{nullptr};
        QLineEdit* key{nullptr};
        QLineEdit* value{nullptr};
    };

    void loadFromRadio();
    void addRow(const QString& key, const QString& value);
    void removeRow(QWidget* rowWidget);
    QList<QPair<QString, QString>> rowValues() const;
    void updateAvailability();
    void setMessage(const QString& text, const QString& tone);
    void onApply();
    void onRadioEnvironmentChanged();

    RadioModel* m_model{nullptr};
    ApplyHandler m_applyHandler;
    QString m_name;
    QList<Row> m_rows;
    QMap<QString, QString> m_loaded;
    bool m_applying{false};
    bool m_containerPresent{true};
    QVBoxLayout* m_rowsLayout{nullptr};
    QWidget* m_rowsContainer{nullptr};
    QLabel* m_emptyLabel{nullptr};
    QLabel* m_message{nullptr};
    QCheckBox* m_showValues{nullptr};
    QPushButton* m_addButton{nullptr};
    QPushButton* m_revertButton{nullptr};
    QPushButton* m_applyButton{nullptr};
};

} // namespace AetherSDR
