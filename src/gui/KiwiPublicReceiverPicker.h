#pragma once

#include <QDateTime>
#include <QVector>

#include "PersistentDialog.h"
#include "core/KiwiPublicDirectory.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace AetherSDR {

// "Browse public KiwiSDRs" picker. Populates from AetherSDR's directory mirror
// (cdn.aethersdr.com/kiwi.json), and lists ONLY receivers whose operator allows
// the external API (ext_api > 0). Web-only receivers (ext_api == 0) are
// filtered out entirely and never shown — AetherSDR honors that policy by not
// offering them, and receivers that publish no policy at all are not assumed to
// permit one. See docs/kiwisdr-public-directory.md.
class KiwiPublicReceiverPicker : public PersistentDialog {
    Q_OBJECT
public:
    explicit KiwiPublicReceiverPicker(QWidget* parent = nullptr);

    // Valid after the dialog is accepted.
    QString selectedEndpoint() const { return m_selectedEndpoint; }  // host[:port]
    QString selectedName() const { return m_selectedName; }          // suggested name

private:
    void startFetch();
    void onReady(const QVector<KiwiPublicReceiver>& receivers, const QDateTime& fetchedAt);
    void applyFilter();
    void acceptCurrentRow();

    KiwiPublicDirectory* m_dir{nullptr};
    QVector<KiwiPublicReceiver> m_apiReceivers;  // already filtered to ext_api>0
    int m_hiddenWebOnly{0};
    int m_hiddenUnknown{0};  // dropped because their API policy wasn't published
    int m_hiddenFlagged{0};  // the origin itself marks these entries as bad
    bool m_fromCache{false};  // current list came from the session cache
    QDateTime m_fetchedAt;   // when the mirror last pulled the origin (UTC)

    QLineEdit*    m_search{nullptr};
    QTableWidget* m_table{nullptr};
    QLabel*       m_status{nullptr};
    QPushButton*  m_refresh{nullptr};
    QPushButton*  m_ok{nullptr};

    QString m_selectedEndpoint;
    QString m_selectedName;
};

} // namespace AetherSDR
