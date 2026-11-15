// The sound card backend this build uses (CMake DATA2G_AUDIO_BACKEND, or the
// platform's on Android): audio::card is audio::qt, audio::ma or
// audio::android, which have the same interface.
#pragma once

#if defined(DATA2G_AUDIO_ANDROID)
#include "audio/android/androidaudio.hpp"
namespace data2g::audio {
namespace card = android;
}
#elif defined(DATA2G_AUDIO_MINIAUDIO)
#include "audio/miniaudio/maaudio.hpp"
namespace data2g::audio {
namespace card = ma;
}
#else
#include "audio/qt/qtaudio.hpp"
namespace data2g::audio {
namespace card = qt;
}
#endif
