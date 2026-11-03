#pragma once
#include <cstddef>

namespace wp {

struct FrameSoundBytes {
    const unsigned char* data;
    size_t size;
};

// The bytes of frame-N.wav (N = 1..9), compiled into the mod.
FrameSoundBytes frameSoundBytes(int n);

}  // namespace wp
