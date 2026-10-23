// Attempt recorder.
//
// Writes one CSV row per input, assist action and death to attempts.csv in the
// mod's save folder, so a session can be read back later to see where the
// player dies and why the assist did or did not help there.

#include "State.hpp"

#include <Geode/modify/PlayLayer.hpp>

#include <fstream>

namespace assist {

namespace {

std::ofstream g_file;
Watch g_watch[2];
int g_attempt = 0;
std::string g_level;

std::string csvField(std::string s) {
    for (auto& ch : s) {
        if (ch == ',' || ch == '\n' || ch == '\r') ch = ' ';
    }
    return s;
}

bool open() {
    if (g_file.is_open()) return true;
    auto path = Mod::get()->getSaveDir() / "attempts.csv";
    bool fresh = !std::filesystem::exists(path);
    g_file.open(path, std::ios::app);
    if (!g_file.is_open()) return false;
    if (fresh) g_file << "level,attempt,step,x,y,mode,event,detail\n";
    return true;
}

}

Watch& watch(int i) { return g_watch[i == 1 ? 1 : 0]; }

void record(PlayerObject* p, char const* event, std::string const& detail) {
    if (!cfg().record || !p || !open()) return;
    auto pl = PlayLayer::get();
    int i = (pl && p == pl->m_player2) ? 1 : 0;
    g_file << g_level << ',' << g_attempt << ',' << watch(i).step << ','
           << fmt::format("{:.1f},{:.1f}", p->getPositionX(), p->getPositionY()) << ','
           << modeName(modeOf(p)) << (i == 1 ? "-p2" : "") << ','
           << event << ',' << csvField(detail) << '\n';
}

void recordFlush() {
    if (g_file.is_open()) g_file.flush();
}

}

using namespace assist;

class $modify(RecorderPlayLayer, PlayLayer) {
    void startAttempt() {
        g_attempt++;
        for (auto& w : g_watch) {
            int step = w.step;
            w = Watch();
            w.step = step;
        }
        if (m_player1) record(m_player1, "attempt", "");
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        g_level = csvField(m_level ? std::string(m_level->m_levelName) : std::string("?"));
        g_attempt = 0;
        startAttempt();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        startAttempt();
    }

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        bool real = !simActive() && p && (p == m_player1 || p == m_player2) &&
                    obj != m_anticheatSpike && !p->m_isDead;
        if (real && cfg().record) {
            auto const& w = watch(p == m_player2 ? 1 : 0);
            int s = w.step;
            std::string why;
            if (s - w.clickStep <= 120) {
                why = fmt::format("auto-clicked {} steps before", s - w.clickStep);
            } else if (s - w.dangerStep <= 60) {
                why = w.danger.empty() ? "saw it coming but did not act (button held or cooldown)"
                                       : "saw it coming: " + w.danger;
            } else if (s - w.blindStep <= 60) {
                why = "could not see it: orb or pad or portal ahead";
            } else {
                why = "did not see it coming (moving object or prediction miss)";
            }
            record(p, "death", fmt::format("{}; hit object {}", why, obj ? obj->m_objectID : 0));
            recordFlush();
        }
        PlayLayer::destroyPlayer(p, obj);
    }

    void levelComplete() {
        if (!simActive() && m_player1) {
            record(m_player1, "complete", "");
            recordFlush();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        recordFlush();
        PlayLayer::onQuit();
    }
};
