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

#include "ProjectLicenseNotice.h"

#ifndef AETHERSDR_VERSION
#define AETHERSDR_VERSION "unknown"
#endif

namespace AetherSDR {

QString projectLicenseNoticeText()
{
    return QStringLiteral(
        "AetherSDR %1\n"
        "Copyright (C) 2025-2026 Jeremy Fielder (KK7GWY) and AetherSDR contributors.\n"
        "\n"
        "AetherSDR is free software: you can redistribute it and/or modify it under\n"
        "the terms of the GNU Affero General Public License (AGPL) version 3 or any\n"
        "later version. Some files retain their original GNU General Public License\n"
        "version 3 (GPL v3) license; the combined work is governed by AGPL v3 under\n"
        "GPL v3 section 13.\n"
        "\n"
        "This program is distributed WITHOUT ANY WARRANTY; without even the implied\n"
        "warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
        "\n"
        "See the file LICENSE in the program's source distribution for the full\n"
        "license text and terms.\n"
        "\n"
        "AetherSDR is an independent project and is not affiliated with or endorsed\n"
        "by FlexRadio Systems. SmartSDR, FlexRadio, FLEX-6000, FLEX-8000, Aurora,\n"
        "SmartLink, and multiFLEX are trademarks of FlexRadio Systems.\n"
    ).arg(QStringLiteral(AETHERSDR_VERSION));
}

QString projectLicenseNoticeHtml()
{
    return QStringLiteral(
        "<p><b>AetherSDR %1</b><br/>"
        "Copyright (C) 2025-2026 Jeremy Fielder (KK7GWY) and AetherSDR contributors.</p>"
        "<p>AetherSDR is free software: you can redistribute it and/or modify it under "
        "the terms of the "
        "<a href=\"https://www.gnu.org/licenses/agpl-3.0.html\">GNU Affero General Public License (AGPL) version 3</a> "
        "or any later version. Some files retain their original "
        "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">GNU General Public License version 3 (GPL v3)</a> "
        "license; the combined work is governed by AGPL v3 under GPL v3 section 13.</p>"
        "<p>This program is distributed <b>WITHOUT ANY WARRANTY</b>; without even the implied "
        "warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.</p>"
        "<p>See the file <code>LICENSE</code> in the program's source distribution for the full "
        "license text and terms.</p>"
        "<p><i>AetherSDR is an independent project and is not affiliated with or endorsed "
        "by FlexRadio Systems. SmartSDR, FlexRadio, FLEX-6000, FLEX-8000, Aurora, "
        "SmartLink, and multiFLEX are trademarks of FlexRadio Systems.</i></p>"
    ).arg(QStringLiteral(AETHERSDR_VERSION));
}

QString projectLicenseOneLine()
{
    return QStringLiteral("AetherSDR %1 — AGPL v3 (combined with GPL v3 per LICENSE)")
        .arg(QStringLiteral(AETHERSDR_VERSION));
}

} // namespace AetherSDR
