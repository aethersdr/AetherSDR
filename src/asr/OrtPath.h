#pragma once

// ONNX Runtime's Session/model-path APIs take a `const ORTCHAR_T*`, which is
// `wchar_t` on Windows and `char` elsewhere. toOrtPath() yields a
// correctly-typed, null-terminatable path from a UTF-8 std::string.
//
// Deliberately Qt-free: the ASR ORT wrappers (SileroVad, SpeakerEmbedder) are
// compiled standalone by their unit tests with only the ORT include path, so
// pulling in <QString> for the widening would break those builds.

#include <string>

#ifdef _WIN32
#include <windows.h> // MultiByteToWideChar (Windows-only path widening)
#endif

namespace AetherSDR::asr_detail {

#ifdef _WIN32
inline std::wstring toOrtPath(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          w.data(), n);
    return w;
}
#else
// Non-Windows: ORTCHAR_T is char; hand back the UTF-8 string unchanged. Returns a
// reference (no copy); the caller uses .c_str() within the same full-expression.
inline const std::string& toOrtPath(const std::string& utf8)
{
    return utf8;
}
#endif

} // namespace AetherSDR::asr_detail
