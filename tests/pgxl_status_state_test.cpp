#include "core/backends/AmpDelta.h"
#include "models/AmpModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AmpModel model;
    bool operateAtPresence = false;
    QObject::connect(&model, &AmpModel::presenceChanged, &model,
                     [&model, &operateAtPresence](bool present) {
        if (present) {
            operateAtPresence = model.operate();
        }
    });
    AmpDelta initial;
    initial.handle = QStringLiteral("0x1000");
    initial.detectedModel = QStringLiteral("PowerGeniusXL");
    initial.operate = true;
    model.applyChanges(initial);
    check(model.present(), "initial amplifier delta establishes presence");
    check(operateAtPresence, "first presence observer sees the decoded operate state");

    if (failures == 0) {
        std::printf("pgxl_status_state_test: all checks passed\n");
        return 0;
    }
    return 1;
}
