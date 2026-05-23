#pragma once

#include <QString>
#include <array>

namespace MasterSDR {

// Semver-style version comparison supporting X.Y.Z and X.Y.Z.P formats.
// Strips non-numeric suffixes (e.g., "0.4.7a" → 0.4.7.0).
class VersionNumber {
public:
    VersionNumber() = default;

    static VersionNumber parse(const QString& str) {
        VersionNumber v;
        // Strip leading 'v' if present
        QString s = str.startsWith('v') ? str.mid(1) : str;
        const auto parts = s.split('.');
        for (int i = 0; i < 4 && i < parts.size(); ++i) {
            // Strip non-numeric suffix (e.g., "7a" → 7)
            QString p = parts[i];
            int end = 0;
            while (end < p.size() && p[end].isDigit()) ++end;
            v.m_seg[i] = p.left(end).toInt();
        }
        return v;
    }

    bool isNull() const { return m_seg[0] == 0 && m_seg[1] == 0 && m_seg[2] == 0 && m_seg[3] == 0; }
    QString toString() const {
        if (m_seg[3] != 0)
            return QString("%1.%2.%3.%4").arg(m_seg[0]).arg(m_seg[1]).arg(m_seg[2]).arg(m_seg[3]);
        return QString("%1.%2.%3").arg(m_seg[0]).arg(m_seg[1]).arg(m_seg[2]);
    }

    friend bool operator==(const VersionNumber& a, const VersionNumber& b) { return a.m_seg == b.m_seg; }
    friend bool operator!=(const VersionNumber& a, const VersionNumber& b) { return !(a == b); }
    friend bool operator<(const VersionNumber& a, const VersionNumber& b) { return a.m_seg < b.m_seg; }
    friend bool operator>(const VersionNumber& a, const VersionNumber& b) { return b < a; }
    friend bool operator<=(const VersionNumber& a, const VersionNumber& b) { return !(b < a); }
    friend bool operator>=(const VersionNumber& a, const VersionNumber& b) { return !(a < b); }

private:
    std::array<int, 4> m_seg{0, 0, 0, 0};
};

} // namespace MasterSDR
