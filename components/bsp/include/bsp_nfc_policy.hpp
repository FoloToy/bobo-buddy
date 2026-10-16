#pragma once
#include <string>
// Each placement triggers once. Three absent polls rearm the same UID.
class BspNfcPlacement {
    std::string active;
    unsigned absent = 0;

  public:
    bool observe(const std::string &uid) {
        if (uid.empty()) {
            if (++absent >= 3)
                active.clear();
            return false;
        }
        absent = 0;
        if (uid == active)
            return false;
        active = uid;
        return true;
    }
    void reset() {
        active.clear();
        absent = 0;
    }
};
