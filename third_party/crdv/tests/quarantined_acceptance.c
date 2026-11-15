/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

int main(void)
{
    puts("SKIP: hardware/public-evidence acceptance is quarantined");
    puts("C1 dynamic versus fixed UDP port on firmware 4.2.18");
    puts("C2 DFM slice settings on firmware 4.2.18");
    puts("C3 Flex polarity inversion and limiting");
    puts("C4 candidate AMBE silence codeword");
    puts("C6 no-parity startup alternative");
    puts("C7 230400 baud fallback");
    puts("C8 FTDI VID/PID inventory");
    puts("C9 drain and tail bounds");
    puts("C10 transmit pre-roll capacity and overflow policy");
    puts("C11 receive sync tolerance, sliding span, and loss threshold");
    puts("C12 health thresholds under platform load");
    puts("C13 firmware unkey timing");
    puts("C14 live four-stream response and return-stream behavior");
    puts("C15 interoperable Gaussian pulse parameters");
    puts("C16 serial cap, response deadline, and reopen interval");
    puts("C17 SmartSDR command deadline");
    return 77;
}
