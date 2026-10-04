#pragma once

namespace AetherSDR::FakePeripheralAuthStore {

// Controls only the dialog test's in-memory store implementation.
void setNextClearResult(bool ok);
void setBackendAvailable(bool available);
void deferClear(bool defer);
// Models the vault read failing before an endpoint-scoped clear: unavailable
// (no backend) clears the session value; denied fails the clear.
enum class ReadFailure { None, Unavailable, Denied };
void setNextReadFailure(ReadFailure failure);
void finishClear();

}
