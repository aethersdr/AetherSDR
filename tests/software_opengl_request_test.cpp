#include "gui/SoftwareOpenGlRequest.h"

#include <QByteArray>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) {
        ++g_failures;
    }
}

class EnvironmentRestore {
public:
    explicit EnvironmentRestore(const char* name)
        : m_name(name)
        , m_wasSet(qEnvironmentVariableIsSet(name))
        , m_value(qgetenv(name))
    {
    }

    ~EnvironmentRestore()
    {
        if (m_wasSet) {
            qputenv(m_name, m_value);
        } else {
            qunsetenv(m_name);
        }
    }

private:
    const char* m_name;
    bool m_wasSet;
    QByteArray m_value;
};

} // namespace

int main()
{
    EnvironmentRestore restoreNoGpu("AETHER_NO_GPU");
    EnvironmentRestore restoreQtOpenGl("QT_OPENGL");

    qunsetenv("AETHER_NO_GPU");
    qunsetenv("QT_OPENGL");
    check(!AetherSDR::softwareOpenGlRequested(),
          "unset environment keeps the default renderer");

    qputenv("AETHER_NO_GPU", "1");
    check(AetherSDR::softwareOpenGlRequested(),
          "AETHER_NO_GPU requests software OpenGL");

    qunsetenv("AETHER_NO_GPU");
    qputenv("QT_OPENGL", "software");
    check(AetherSDR::softwareOpenGlRequested(),
          "QT_OPENGL=software requests software OpenGL");

    qputenv("QT_OPENGL", "SoFtWaRe");
    check(AetherSDR::softwareOpenGlRequested(),
          "QT_OPENGL software comparison is case-insensitive");

    qputenv("QT_OPENGL", "desktop");
    check(!AetherSDR::softwareOpenGlRequested(),
          "other QT_OPENGL values keep the default renderer");

    return g_failures == 0 ? 0 : 1;
}
