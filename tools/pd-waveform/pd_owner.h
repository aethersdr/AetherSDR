#pragma once
// Optional inherited read-only lifetime pipe. No payload is consumed.
// -1 means standalone operation; -2 means invalid launcher input.
int pd_owner_open(const char* descriptor);
int pd_owner_closed(int descriptor);
int pd_owner_unprivileged(void);
