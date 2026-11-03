#pragma once
#include <cstddef>

namespace wp::ui {
struct LogoBytes {
    const unsigned char* data;
    size_t size;
};
LogoBytes logoBytes();   // the PNG bytes of the Windowpane logo
}  // namespace wp::ui
