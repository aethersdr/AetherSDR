#pragma once

#include <QFrame>
#include <QVector>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QEvent;
class QObject;
class QVBoxLayout;

namespace AetherSDR {

// The vertical stage column shared by AetherRX and AetherTX.
//
// One row per stage: a drag grip, the tab that selects its page, and an enable
// checkbox. The bar owns the look and the gestures and knows nothing about
// audio — the host says which stages exist and answers the questions the bar
// asks (is this stage enabled, what is the chain order), so the same column
// serves a receive chain and a transmit one without either growing a copy.
//
// Rows that are not chain stages — noise reduction ahead of the chain, the
// output meter at the end of it — carry no grip and, if the host says so, no
// checkbox, and hold their declared position while the rest reorder around
// them.
class StageTabBar : public QFrame {
    Q_OBJECT

public:
    // What the host must answer for the bar to do its job. All are called on
    // the GUI thread, and may be called often — the enable poll runs at a few
    // hertz while the window is visible.
    struct Host {
        // Is `id` part of the reorderable chain? False pins the row where it
        // was declared and gives it no grip.
        std::function<bool(int)>                   isChainStage;
        // Current enable state, and how to change it. A null setter means the
        // row shows no checkbox at all.
        std::function<bool(int)>                   stageEnabled;
        std::function<void(int, bool)>             setStageEnabled;
        // The chain order as ids, and how to commit a new one.
        std::function<QVector<int>()>              chainOrder;
        std::function<void(const QVector<int>&)>   commitChainOrder;
    };

    // `objectPrefix` namespaces every row's object and accessible names —
    // "aetherRx" gives aetherRxTabGate, aetherRxEnableGate, aetherRxGripGate.
    // Both windows carry tabs called Gate, EQ, Tube and so on, so without a
    // prefix the automation bridge resolves a name to whichever window it
    // finds first and refuses it as hidden.
    explicit StageTabBar(const QString& objectPrefix, QWidget* parent = nullptr);

    void setHost(Host host);

    // Declare a stage. `id` is the host's own enum value; the bar only ever
    // hands it back. Order of calls is the initial top-to-bottom order.
    void addStage(int id, const QString& label, bool wantsCheckbox = true);

    // Park a widget at the foot of the column, above the footer button. The
    // bar takes no interest in what it is — the transmit window puts its MIC
    // and TX indicators here.
    void addFooterWidget(QWidget* w);

    // Add the footer button under the stretch — Settings, in both windows.
    void addFooterButton(const QString& label, const QString& objectName,
                         const QString& tooltip);

    // The drag payload type this column emits and accepts. Private to the
    // window: see the note in StageTabBar.cpp. Exposed so a test can cross
    // the two bars' real types rather than a copy of the formula, which
    // would pass whether or not they are actually distinct.
    QString dragMimeType() const { return m_mime; }

    void setCurrentStage(int id);
    int  currentStage() const;

    // Pull every checkbox from the host, and re-lay the rows out if the host's
    // chain order has changed behind the bar's back (the chain strip can
    // reorder too). Cheap enough to call on a timer; no-ops when nothing moved.
    void refreshFromHost();

signals:
    void stageSelected(int id);
    void footerButtonClicked();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void beginFooter();
    void dropStageAt(int movedId, int y);
    void relayoutRows();

    QString       m_prefix;
    // Drag payload type, private to this window -- see stageMimeFor().
    QString       m_mime;
    bool          m_footerStarted{false};
    Host          m_host;
    QButtonGroup* m_group{nullptr};
    QVBoxLayout*  m_rows{nullptr};
    QWidget*      m_footerRow{nullptr};

    struct Row {
        int        id{-1};
        QWidget*   row{nullptr};
        QCheckBox* check{nullptr};
        QWidget*   indent{nullptr};   // stands in for an absent grip
    };
    QVector<Row> m_stages;
    QVector<int> m_laidOut;
    bool         m_syncing{false};
};

} // namespace AetherSDR
