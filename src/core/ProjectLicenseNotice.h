// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AetherSDR — ProjectLicenseNotice
//
// Copyright (C) 2025-2026 Jeremy Fielder (KK7GWY) and AetherSDR contributors.
//
// This file is part of AetherSDR.
//
// AetherSDR is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// AetherSDR is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public
// License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with AetherSDR. If not, see <https://www.gnu.org/licenses/>.
//
// ─────────────────────────────────────────────────────────────────────────
//
// This file is intentionally licensed under AGPL v3 to invoke the
// combined-work mechanism of GPL v3 section 13. Other files in this
// repository may retain their original GPL v3 license for historical
// contributions; the combined work is governed by AGPL v3's network-use
// clause (section 13). See LICENSE for the full notice.

#pragma once

#include <QString>

namespace AetherSDR {

// Returns a short, human-readable summary of AetherSDR's license posture
// for display in the About dialog, --version output, or startup log.
// The string includes:
//   * Project name and version
//   * AGPL v3 (with GPL v3 combined-work) license declaration
//   * Pointer to the LICENSE file for full terms
//   * No-warranty notice
//   * No-affiliation notice with respect to FlexRadio Systems
//
// The returned string is plain text suitable for log output, terminal
// stdout, or as the body of a QMessageBox.  It does not include HTML or
// rich text formatting.
QString projectLicenseNoticeText();

// Returns the same content as projectLicenseNoticeText() but with light
// HTML formatting (paragraph breaks, hyperlinks). Suitable for use in a
// QLabel or QTextBrowser.
QString projectLicenseNoticeHtml();

// Returns just the one-line license declaration suitable for a startup
// log entry. Example:
//   "AetherSDR v26.5.2 — AGPL v3 (combined with GPL v3, see LICENSE)"
QString projectLicenseOneLine();

} // namespace AetherSDR
