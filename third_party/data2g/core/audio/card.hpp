// The sound card backend this build uses (CMake DATA2G_AUDIO_BACKEND):
// audio::card is audio::qt or audio::ma, which have the same interface.
#pragma once

#ifdef DATA2G_AUDIO_MINIAUDIO
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
