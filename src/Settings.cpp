#include "State.hpp"

namespace assist {

static Settings g_settings;
static Stats g_stats;
static int g_resetEpoch = 0;

static constexpr char const* PREDICT_KEYS[ModeCount] = {
    "predict-cube", "predict-ship", "predict-ball", "predict-ufo",
    "predict-wave", "predict-robot", "predict-spider", "predict-swing",
};

Settings& cfg() { return g_settings; }
Stats& stats() { return g_stats; }
int resetEpoch() { return g_resetEpoch; }
void bumpResetEpoch() { g_resetEpoch++; }

static int getInt(char const* key) {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key));
}

static bool getBool(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}

void loadSettings() {
    auto& s = g_settings;
    s.enabled = getBool("enabled");
    s.showLabel = getBool("show-label");
    s.predictive = getBool("predictive");
    s.maxDelay = getInt("max-delay");
    s.lookGround = getInt("lookahead-ground");
    s.lookFly = getInt("lookahead-fly");
    s.assistReleases = getBool("assist-releases");
    for (int i = 0; i < ModeCount; i++) s.predictMode[i] = getBool(PREDICT_KEYS[i]);
    s.earlyBuffer = getBool("early-buffer");
    s.earlyWindow = getInt("early-window");
    s.autoSave = getBool("auto-save");
    s.saveWindow = getInt("save-window");
    s.saveHold = getInt("save-hold");
    s.planAhead = getInt("plan-ahead");
    s.showPath = getBool("show-path");
    s.debug = getBool("debug");
    s.record = getBool("record");
}

Mode modeOf(PlayerObject* p) {
    if (p->m_isShip) return Ship;
    if (p->m_isBird) return Ufo;
    if (p->m_isDart) return Wave;
    if (p->m_isSwing) return Swing;
    if (p->m_isBall) return Ball;
    if (p->m_isRobot) return Robot;
    if (p->m_isSpider) return Spider;
    return Cube;
}

bool isGroundMode(PlayerObject* p) {
    switch (modeOf(p)) {
        case Cube: case Ball: case Robot: case Spider: return true;
        default: return false;
    }
}

char const* modeName(Mode m) {
    static constexpr char const* NAMES[ModeCount] = {
        "cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing",
    };
    return NAMES[m];
}

int ringsTouching(PlayerObject* p) {
    return p->m_touchingRings ? static_cast<int>(p->m_touchingRings->count()) : 0;
}

}

$on_mod(Loaded) {
    using namespace assist;
    loadSettings();

    for (auto key : {"enabled", "show-label", "predictive", "assist-releases",
                     "predict-cube", "predict-ship", "predict-ball", "predict-ufo",
                     "predict-wave", "predict-robot", "predict-spider", "predict-swing",
                     "early-buffer", "auto-save", "show-path", "debug", "record"}) {
        listenForSettingChanges<bool>(key, [](bool) { loadSettings(); overlayRefresh(); });
    }
    for (auto key : {"max-delay", "lookahead-ground", "lookahead-fly", "early-window",
                     "save-window", "save-hold", "plan-ahead"}) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { loadSettings(); });
    }

    log::info("[Assist] loaded");
}
