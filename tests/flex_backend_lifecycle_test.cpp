// Regression guard for aetherd RFC step 2.2b: FlexBackend owns the
// RadioConnection + PanadapterStream and their two worker threads. It locks in
// the #502 creation order (PanadapterStream thread first, RadioConnection
// thread second) and the teardown ordering (BlockingQueued disconnect/stop →
// deleteLater → thread quit/wait) that 2.2b relocated out of RadioModel — so a
// future refactor of that ctor/dtor pair has an automated tripwire, not just a
// one-time manual check. (#4058 review, @NF0T)

#include "core/backends/flex/FlexBackend.h"
#include "core/RadioConnection.h"
#include "core/PanadapterStream.h"

#include <QCoreApplication>
#include <QThread>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Construct/destruct repeatedly: a broken #502 teardown tends to hang
    // (thread wait) or crash on the 2nd+ cycle rather than the 1st.
    for (int i = 0; i < 3; ++i) {
        auto backend = std::make_unique<FlexBackend>();

        RadioConnection* conn = backend->connection();
        PanadapterStream* pan = backend->panStream();
        check(conn != nullptr, "connection() is null after construction");
        check(pan != nullptr, "panStream() is null after construction");

        // The wire objects must live on their own worker threads (moved off the
        // main thread), and on two distinct threads.
        if (conn && pan) {
            check(conn->thread() != QThread::currentThread(),
                  "RadioConnection is on the main thread");
            check(pan->thread() != QThread::currentThread(),
                  "PanadapterStream is on the main thread");
            check(conn->thread() != pan->thread(),
                  "connection and panStream share a worker thread");
        }

        check(!backend->isConnected(), "fresh backend reports connected");

        // ~FlexBackend runs the #502 teardown; must not hang or crash.
        backend.reset();
    }

    if (g_failures == 0) {
        std::printf("flex_backend_lifecycle_test: OK (3 ctor/dtor cycles)\n");
    }
    return g_failures == 0 ? 0 : 1;
}
