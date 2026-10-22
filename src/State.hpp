#pragma once

#include <Geode/Geode.hpp>

#include <string>
#include <vector>

using namespace geode::prelude;

namespace assist {

// ---- settings --------------------------------------------------------------

enum Mode { Cube = 0, Ship, Ball, Ufo, Wave, Robot, Spider, Swing, ModeCount };

struct Settings {
    bool enabled = true;
    bool showLabel = true;

    bool predictive = true;
    int maxDelay = 3;          // physics steps an input may be held back
    int lookGround = 30;       // steps simulated after the input, cube/ball/robot/spider
    int lookFly = 12;          // steps simulated after the input, ship/ufo/wave/swing
    bool assistReleases = true;
    bool predictMode[ModeCount] = {true, true, true, true, true, true, true, true};

    bool earlyBuffer = true;
    int earlyWindow = 4;       // physics steps

    bool showPath = false;
    bool debug = false;
};

Settings& cfg();
void loadSettings();

Mode modeOf(PlayerObject* p);
bool isGroundMode(PlayerObject* p);
char const* modeName(Mode m);
int ringsTouching(PlayerObject* p);

// ---- reset tracking --------------------------------------------------------

// Raised on death, restart and checkpoint load. Assist state is cleared on the
// next input or physics step when this changes.
int resetEpoch();
void bumpResetEpoch();

// ---- simulation ------------------------------------------------------------

struct SimEvent {
    int tick;   // 0 = the next physics step
    bool down;  // press or release
};

struct SimResult {
    bool died = false;
    bool unknown = false;  // hit something the simulation does not model (orb, pad, most portals)
    int ticks = 0;         // steps actually simulated
    std::vector<cocos2d::CCPoint> path;
};

bool simActive();
bool simReady(PlayLayer* pl);
SimResult simulate(PlayLayer* pl, PlayerObject* real, std::vector<SimEvent> const& events, int totalTicks, bool recordPath);

// ---- stats and overlay -----------------------------------------------------

struct Stats {
    int delayed = 0;
    int buffered = 0;
    std::string last;
};

Stats& stats();
void overlayRefresh();
void overlayShowPath(std::vector<cocos2d::CCPoint> const& path, bool died);
void overlayClearPath();

}
