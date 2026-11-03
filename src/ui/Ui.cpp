#include "Ui.hpp"

namespace wp::ui {

void attach(PlayLayer* pl) {
    hudAttach(pl);
    markersAttach(pl);
}

void frame(PlayLayer* pl) {
    hudFrame(pl);
    markersFrame(pl);
}

void detach(PlayLayer*) {
    hudDetach();
    markersDetach();
}

}  // namespace wp::ui
