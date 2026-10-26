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
    bool rawFly = true;        // ship, UFO, wave and swing inputs go straight to the game

    bool predictive = true;
    int maxDelay = 5;          // physics steps an input may be held back
    int lookGround = 30;       // steps simulated after the input, cube/ball/robot/spider
    int lookFly = 12;          // steps simulated after the input, ship/ufo/wave/swing
    bool assistReleases = true;
    bool predictMode[ModeCount] = {true, true, true, true, true, true, true, true};

    bool earlyBuffer = true;
    int earlyWindow = 6;       // physics steps

    bool autoSave = true;
    int saveWindow = 10;       // steps before a death the mod may click for you
    int saveHold = 6;          // steps the auto-click holds the button
    bool saveFly = false;      // auto-click in ship, UFO, wave and swing
    int planAhead = 240;       // ground modes: steps an auto-click must leave a way through

    bool showPath = false;
    bool debug = false;
    bool record = true;

    bool learn = true;
    int learnEarly = 12;       // steps before a learned window an early click is held back
    bool learnBlock = true;    // drop clicks at spots where clicking always killed the player
};

Settings& cfg();
void loadSettings();

Mode modeOf(PlayerObject* p);
bool isGroundMode(PlayerObject* p);
// Ship, UFO, wave and swing with raw input on: the assist leaves these inputs alone.
bool isRawMode(PlayerObject* p);
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

// ---- attempt recorder ------------------------------------------------------

// What the assist last saw for one player, used to explain deaths.
struct Watch {
    int step = 0;
    int dangerStep = -100000;  // last step the idle simulation saw a death coming
    int blindStep = -100000;   // last step the simulation hit something it cannot model
    int clickStep = -100000;   // last auto-click
    std::string danger;        // why the last seen danger got no auto-click
};

Watch& watch(int i);
void record(PlayerObject* p, char const* event, std::string const& detail = "");
void recordFlush();

// ---- learned click windows -------------------------------------------------

struct LearnWindow {
    float lo;   // x of the earliest click that got past this spot
    float hi;   // x of the latest one
    int hold;   // typical hold length of those clicks, in steps
    int n;      // how many good clicks it is based on
    int yBucket;  // height band the clicks were made at
};

void learnLoadLevel(std::string const& level);
void learnFeed(char const* event, PlayerObject* p, int step, std::string const& detail);
// First window that overlaps [x - after, x + before] around the player.
LearnWindow const* learnWindowNear(float x, float y, float before, float after);
int learnWindowCount();
// A click here has always killed the player, and not clicking here has not.
bool learnIsBadClick(float x, float y);

// ---- stats and overlay -----------------------------------------------------

struct Stats {
    int delayed = 0;
    int buffered = 0;
    int saved = 0;
    int learned = 0;
    int blocked = 0;
    std::string last;
};

Stats& stats();
void overlayRefresh();
void overlayShowPath(std::vector<cocos2d::CCPoint> const& path, bool died);
void overlayClearPath();

}
