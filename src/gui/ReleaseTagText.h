#pragma once

#include <QMessageBox>
#include <QString>

namespace AetherSDR {

// A GitHub release tag is network text, and VersionNumber::parse reads only
// the leading digits of each part, so a tag that passes the newer-version
// test can still carry markup. Each prompt neutralises the tag itself.

// Rich text by design (<p>, <b>), so both versions are HTML-escaped.
inline QString outdatedVersionPromptHtml(const QString& current, const QString& latestTag)
{
    return QStringLiteral(
               "<p>You are running <b>v%1</b> but <b>v%2</b> is available.</p>"
               "<p>Your issue may already be fixed in the latest release. "
               "Please update before filing a bug report.</p>"
               "<p>Continue anyway?</p>")
        .arg(current.toHtmlEscaped(), latestTag.toHtmlEscaped());
}

// Neither line carries markup, so the box is plain text and shows the tag
// verbatim. Escaping would not do: under Qt::AutoText an escaped tag without
// "&lt;" stays plain and shows its entities literally.
inline void setUpdateAvailableText(QMessageBox& box, const QString& current,
                                   const QString& latestTag)
{
    box.setTextFormat(Qt::PlainText);
    box.setText(QStringLiteral("AetherSDR v%1 is available.").arg(latestTag));
    box.setInformativeText(QStringLiteral("You are running v%1.").arg(current));
}

} // namespace AetherSDR
