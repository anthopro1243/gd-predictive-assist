// Learned click windows.
//
// Geometry Dash is deterministic: in a ground mode the player passes the same
// x positions every attempt, so a click that got past an obstacle once works
// again at the same x. From the recorded attempts (attempts.csv plus the
// current session) this collects the x positions of clicks that the player
// survived well past, and groups them into windows. Spots where clicks died
// at least half as often as they worked split windows apart. The assist then holds
// early clicks back to the start of a window and clicks for the player at the
// end of one if they have not clicked yet.
//
// This does not depend on the simulation, so it works on tight timings where
// the simulation is a step off.

#include "State.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace assist {

namespace {

constexpr float SURVIVE_DIST = 120.f;  // a click counts as good if the player got this far past it
constexpr float CLUSTER_GAP = 8.f;     // good clicks further apart than this are separate windows
constexpr int MIN_GOOD = 3;            // good clicks needed before a window is trusted
constexpr float MIN_X = 5.f;           // ignore the click held from the restart

struct Press {
    float x;
    int step;
    int hold = 0;  // 0 = release not seen
};

struct Attempt {
    std::vector<Press> presses;
    bool open = false;
};

std::string g_level;
Attempt g_cur;
struct Spot {
    int good = 0;
    int bad = 0;
    std::vector<int> holds;
};
std::map<int, Spot> g_spots;  // keyed by x * 10, rounded
int g_goodTotal = 0;
std::vector<LearnWindow> g_windows;

bool groundModeName(std::string const& m) {
    return m == "cube" || m == "ball" || m == "robot" || m == "spider";
}

void rebuild() {
    g_windows.clear();
    std::vector<std::pair<float, Spot const*>> run;

    auto flush = [&] {
        int n = 0;
        std::vector<int> holds;
        for (auto const& [x, spot] : run) {
            n += spot->good;
            holds.insert(holds.end(), spot->holds.begin(), spot->holds.end());
        }
        if (n >= MIN_GOOD) {
            int hold = 6;
            if (!holds.empty()) {
                std::nth_element(holds.begin(), holds.begin() + holds.size() / 2, holds.end());
                hold = std::clamp(holds[holds.size() / 2], 1, 40);
            }
            g_windows.push_back({run.front().first, run.back().first, hold, n});
        }
        run.clear();
    };

    for (auto const& [key, spot] : g_spots) {
        float x = key / 10.f;
        bool ok = spot.good >= 1 && spot.good >= 2 * spot.bad;
        if (!ok) {
            flush();
            continue;
        }
        if (!run.empty() && x - run.back().first > CLUSTER_GAP) flush();
        run.push_back({x, &spot});
    }
    flush();
}

void finish(float endX) {
    if (!g_cur.open) return;
    for (auto const& p : g_cur.presses) {
        auto& spot = g_spots[static_cast<int>(std::lround(p.x * 10.f))];
        if (endX > p.x + SURVIVE_DIST) {
            spot.good++;
            g_goodTotal++;
            if (p.hold > 0) spot.holds.push_back(p.hold);
        } else {
            spot.bad++;
        }
    }
    g_cur = Attempt();
}

// Shared by the live feed and the CSV loader so both read rows the same way.
void feed(std::string const& event, float x, std::string const& mode, int step, std::string const& detail) {
    if (event == "attempt") {
        g_cur = Attempt();
        g_cur.open = true;
        return;
    }
    if (!g_cur.open) return;
    if (event == "press" || event == "press-fired" || event == "auto-click" || event == "learned-click") {
        // Held-back presses are logged again as press-fired where they land.
        if (detail.find("dropped") != std::string::npos || detail.find("delay") != std::string::npos) return;
        if (!groundModeName(mode) || x < MIN_X) return;
        g_cur.presses.push_back({x, step});
    } else if (event == "release") {
        for (auto it = g_cur.presses.rbegin(); it != g_cur.presses.rend(); ++it) {
            if (it->hold == 0) {
                it->hold = std::max(1, step - it->step);
                break;
            }
        }
    } else if (event == "death") {
        finish(x);
        rebuild();
    } else if (event == "complete") {
        finish(1e9f);
        rebuild();
    }
}

}

void learnLoadLevel(std::string const& level) {
    g_level = level;
    g_cur = Attempt();
    g_spots.clear();
    g_goodTotal = 0;

    std::ifstream in(Mod::get()->getSaveDir() / "attempts.csv");
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() < 7 || f[0] != level) continue;
        // Rows from before the player-2 key fix have no position.
        if (f[5].find("-p2") != std::string::npos) continue;
        feed(f[6], std::strtof(f[3].c_str(), nullptr), f[5], std::atoi(f[2].c_str()),
             f.size() > 7 ? f[7] : std::string());
    }
    // The file's last attempt may be unfinished; start clean.
    g_cur = Attempt();
    rebuild();
    log::info("[Assist] learned {} click windows for {} from {} good clicks", g_windows.size(), level, g_goodTotal);
}

void learnFeed(char const* event, PlayerObject* p, int step, std::string const& detail) {
    feed(event, p->getPositionX(), modeName(modeOf(p)), step, detail);
}

LearnWindow const* learnWindowNear(float x, float before, float after) {
    for (auto const& w : g_windows) {
        if (w.hi + after < x) continue;
        if (w.lo - before > x) return nullptr;
        return &w;
    }
    return nullptr;
}

}
