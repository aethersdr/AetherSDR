#pragma once

#include "core/ThemeManager.h"

#include <QColor>
#include <QList>
#include <QPointer>
#include <QVector>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace AetherSDR {

class CompactColorPicker;
class GradientStrip;

// Inline token editor used by ThemeEditorDialog.  Shows every control
// group at all times (single vertical layout, no stacked-widget swap)
// and enables/disables each group based on the current token's
// namespace + value shape:
//
//   * color.* (scalar)   — CompactColorPicker bound to the scalar buffer
//   * color.* (gradient) — GradientStrip + stop list + angle; the
//     CompactColorPicker is bound to the currently-selected stop
//   * font.family.*      — Font toolbar (family + size + style buttons)
//   * font.size.* / sizing.* — pixel-value spin box
//
// All edits write to local buffers only.  ThemeManager isn't touched
// until the operator clicks OK; Cancel discards back to the last
// committed value.  This keeps drag interactions (SV crosshair,
// gradient stop drag) cheap — only the picker's own widgets repaint
// at mouse-event rate, never the rest of the app.
class TokenEditorWidget : public QWidget {
    Q_OBJECT

public:
    explicit TokenEditorWidget(QWidget* parent = nullptr);

    void setToken(const QString& key);
    const QString& currentToken() const { return m_currentToken; }

    // Container scope this editor is editing under.  Empty = root.
    // Setting a non-empty path routes commits through the scope-aware
    // ThemeManager setters and reads through `valueAt(path, token)`
    // so the buffer reflects the scope's resolved value (including
    // inherited values from ancestors).
    void setActiveContainerPath(const QString& path);
    QString activeContainerPath() const { return m_activeContainerPath; }

    // Owned by this widget but placed by the parent dialog (typically
    // into the Inspect row).  Stays in sync with the current token +
    // dirty state via setToken() / markDirty().
    QLabel*      headerLabel() const { return m_header; }
    QPushButton* resetButton() const { return m_resetBtn; }

signals:
    void tokenChanged(const QString& key);

    // Emitted when the operator hits OK while the active theme is one
    // of the built-in bundled themes ("Default Dark" / "Default Light")
    // — the parent dialog runs the Save As flow on the user's behalf,
    // then invokes completeDeferredCommit() to apply the buffered
    // edits to the new user copy.
    void requestSaveAsBeforeCommit();

public:
    // Apply the pending buffered edit captured before the parent
    // dialog ran Save As.  Called after the active theme has been
    // switched to a non-built-in copy.  No-op if no edit is pending.
    void completeDeferredCommit();

private slots:
    void onColorPickerChanged(const QColor& c);
    void onColorTypeToggled();
    void onGradientStopMoved(int index, qreal newAt);
    void onGradientStopActivated(int index);
    void onGradientRequestNewStop(qreal at);
    void onGradientRequestDeleteStop(int index);
    void onGradientStopListSelectionChanged();
    void onGradientAngleChanged(int deg);
    void onGradientAddStop();
    void onGradientDeleteStop();
    void onFontFamilyChanged(const QString& family);
    void onFontSizeChanged(int v);
    void onFontSizeBumpUp();
    void onFontSizeBumpDown();
    void onNumericValueChanged(int v);
    void onResetClicked();
    void onOkClicked();
    void onCancelClicked();

private:
    enum EditTarget {
        TargetNone,         // no token selected — everything disabled
        TargetScalarColor,  // picker → m_bufferColor
        TargetGradient,     // picker → m_gradientBuf.stops[selected].color
        TargetFontFamily,   // font toolbar → m_bufferFontFamily
        TargetNumeric,      // numeric spin → m_bufferNumeric
    };

    void buildHeaderRow(QVBoxLayout* root);
    void buildTypeRow(QVBoxLayout* root);
    QWidget* buildColorGroup();
    QWidget* buildGradientGroup();
    void buildFontGroup(QVBoxLayout* root);
    void buildNumericGroup(QVBoxLayout* root);
    void buildButtonRow(QVBoxLayout* root);

    void applyEnableState();
    void loadCurrentTokenIntoBuffers();

    void selectGradientStop(int index);
    int  selectedGradientStopIndex() const;
    void rebuildGradientStopList();
    void syncGradientStripAndList();
    void sortGradientStopsByAt();

    void refreshResetButton();
    void markDirty();
    void commitBufferToThemeManager();

    // Select `sz` in the non-editable font-size combo, inserting a new
    // entry in sorted order if the preset list doesn't already contain
    // it (so A↑/A↓ bumps to off-preset values stay in sync).
    void syncFontSizeComboToBuffer(int sz);

    QLabel*       m_header{nullptr};
    QPushButton*  m_okBtn{nullptr};
    QPushButton*  m_cancelBtn{nullptr};
    QPushButton*  m_resetBtn{nullptr};

    // Type chooser — only meaningful for color.* tokens.
    QRadioButton* m_typeFlat{nullptr};
    QRadioButton* m_typeGradient{nullptr};
    QButtonGroup* m_typeGroup{nullptr};
    QLabel*       m_typeLabel{nullptr};

    // Color group — single CompactColorPicker that rebinds between the
    // scalar buffer (flat-color tokens) and the selected gradient stop
    // (gradient-color tokens).
    QLabel*             m_colorGroupLabel{nullptr};
    CompactColorPicker* m_colorPicker{nullptr};

    // Font group — family combo + size combo + style buttons (B/I/U/S),
    // size bump (A↑/A↓), clear-formatting placeholder.  Style buttons
    // are visual placeholders — AetherSDR has no font.weight / font.style
    // tokens yet, so they're disabled with a "future tokens" tooltip.
    QLabel*        m_fontGroupLabel{nullptr};
    QFontComboBox* m_fontCombo{nullptr};
    QComboBox*     m_fontSizeCombo{nullptr};
    QToolButton*   m_fontBoldBtn{nullptr};
    QToolButton*   m_fontItalicBtn{nullptr};
    QToolButton*   m_fontUnderlineBtn{nullptr};
    QToolButton*   m_fontStrikeBtn{nullptr};
    QToolButton*   m_fontSizeUpBtn{nullptr};
    QToolButton*   m_fontSizeDownBtn{nullptr};
    QToolButton*   m_fontClearBtn{nullptr};

    // Gradient group — strip + stop list + inline +/- buttons next to
    // the label + angle spinbox.  The "selected stop colour" is
    // edited via the shared color picker above (no second embedded
    // picker — saves vertical space).
    QLabel*        m_gradGroupLabel{nullptr};
    GradientStrip* m_gradStrip{nullptr};
    QListWidget*   m_gradStopList{nullptr};
    QToolButton*   m_gradAddBtn{nullptr};
    QToolButton*   m_gradDelBtn{nullptr};
    QSpinBox*      m_gradAngleSpin{nullptr};

    // Recent-colors grid — 2 rows × 10 swatches sitting in the
    // gradient column below the stop list.  Per-theme: each theme
    // keeps its own list in AppSettings under the key
    // "ThemeEditor.RecentColors/<theme name>".  Loaded on
    // construction and re-loaded whenever the active theme changes
    // (theme switch via View menu, Save As, etc.).
    QVector<QToolButton*> m_recentButtons;
    QList<QColor>         m_recentColors;
    QString               m_recentColorsTheme;  // theme last loaded into m_recentColors
    void buildRecentColorsGrid(QVBoxLayout* lay, QWidget* group);
    void loadRecentColors();
    void saveRecentColors() const;
    void pushRecentColor(const QColor& c);
    void refreshRecentButtons();
    void applyRecentColor(int idx);
    void reloadRecentColorsIfThemeChanged();
    static QString recentColorsKeyFor(const QString& themeName);

    QString    m_currentToken;
    QString    m_activeContainerPath;  // empty == root scope
    EditTarget m_target{TargetNone};

    // Working copies — never written to ThemeManager until OK.
    QColor        m_bufferColor;
    ThemeGradient m_gradientBuf;
    QString       m_bufferFontFamily;
    int           m_bufferFontSize{12};
    int           m_bufferNumeric{0};

    // Snapshot of the buffer used to survive the list-rebuild that
    // happens when Save As switches the active theme out from under
    // an in-progress edit on a built-in theme.
    struct DeferredEdit {
        QString       token;
        EditTarget    target {TargetNone};
        QColor        color;
        ThemeGradient gradient;
        QString       fontFamily;
        int           numeric {0};
        int           fontSize {12};
        bool          pending {false};
    };
    DeferredEdit m_deferredEdit;
    bool          m_dirty{false};
    bool          m_settingControlsFromToken{false};
};

} // namespace AetherSDR
