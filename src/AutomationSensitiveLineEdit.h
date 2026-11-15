#pragma once

// Desktop-only automation helper shared by AutomationServer (a legacy core/
// source built into aetherdesktop_support) and GUI tests. This sits in src/
// so the engine has no src/core/ QtWidgets header and no core -> gui include.

#include <QLineEdit>
#include <QPointer>
#include <QVector>
#include <utility>

namespace AetherSDR {

// Preserve redaction when a user temporarily reveals a secret in the UI.
inline QString automationLineEditValue(const QLineEdit* edit)
{
    if (edit->property("aetherSensitiveValue").toBool()
        || edit->echoMode() != QLineEdit::Normal) {
        return edit->text().isEmpty() ? QString() : QStringLiteral("<hidden>");
    }
    return edit->text();
}

// QWidget::grab paints child widgets synchronously. Hide any revealed code
// during the capture, then restore the operator's Show setting.
class AutomationSensitiveGrabMask {
public:
    explicit AutomationSensitiveGrabMask(QWidget* root)
    {
        if (!root) {
            return;
        }
        auto mask = [this](QLineEdit* edit) {
            if (!edit || edit->echoMode() == QLineEdit::Password
                || edit->echoMode() == QLineEdit::NoEcho) {
                return;
            }
            if (!edit->property("aetherSensitiveValue").toBool()
                && edit->echoMode() == QLineEdit::Normal) {
                return;
            }
            m_original.append({edit, edit->echoMode()});
            edit->setEchoMode(QLineEdit::Password);
        };
        mask(qobject_cast<QLineEdit*>(root));
        for (QLineEdit* edit : root->findChildren<QLineEdit*>()) {
            mask(edit);
        }
    }

    ~AutomationSensitiveGrabMask()
    {
        for (const auto& [edit, mode] : m_original) {
            if (edit) {
                edit->setEchoMode(mode);
            }
        }
    }

    AutomationSensitiveGrabMask(const AutomationSensitiveGrabMask&) = delete;
    AutomationSensitiveGrabMask& operator=(const AutomationSensitiveGrabMask&) = delete;

private:
    QVector<std::pair<QPointer<QLineEdit>, QLineEdit::EchoMode>> m_original;
};

} // namespace AetherSDR
