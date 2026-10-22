# KAT500 clean-room wire fixtures

`utility-1.23.11.9.KAT500Config` is the untouched output of Elecraft KAT500
Utility 1.23.11.9 on 2026-10-13, connected only to the task-owned virtual
COM8/COM108 pair. Identity SN12345 and all EEPROM contents were synthetic.
It contains both complete regions and the Utility's original CRC32 ECCFA104.
The file has no firmware or executable code.

`configuration-readback.json` contains the 103 documented configuration GET
responses captured by the parent task from firmware 02.16 on 2026-10-13.
Endpoint and device serial metadata are excluded. No setter or RF operation
produced this fixture.

Protocol provenance is the official KAT500 Serial Command Reference dated
2023-09-06 and original runtime wire captures, retained in the task evidence
under `Documents/AetherSDR/elecraft-rea/kat500-investigation` and
`Documents/AetherSDR/kat500-validation`. No proprietary code was decompiled.
The model test injects byte transport and time; it opens no socket or port.

Native-file negative fixture CRC values and the varied encoder CRC were
independently calculated with Python `zlib.crc32(concatenated_lines) ^ 0xffffffff`.
A missing first MEM2 record has CRC C006FDBE; a duplicated first MEM2 record has
CRC 92719607; the independently specified varied pattern has CRC 69488867.
