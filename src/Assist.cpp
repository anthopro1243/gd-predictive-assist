// Input handling.
//
// 1. Predictive delay: when a click or release arrives, simulate it on the
//    next physics step. If that dies within the look-ahead but holding it back
//    1..N steps survives, hold it back by the smallest such delay.
//
// 2. Early-click buffer: a press in the air (cube, ball, robot, spider) that
//    is released before landing is replayed when the player touches the ground
//    or an orb within the window, keeping the original tap length. This covers
//    orbs, which the simulation does not model.

#include "State.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>

#include <chrono>
#include <deque>

using namespace assist;

namespace {

struct Queued {
    int fireStep;
    bool down;
};

struct EarlyState {
    bool held = false;
    int pressStep = 0;

    bool pending = false;
    int pendingStart = 0;
    int tapLen = 0;          // 0 = still held when last seen

    bool rescueHeld = false; // we pressed for the player and owe a release
    int rescueReleaseAt = 0;
};

constexpr int JUMP_BUTTON = 1;

}

class $modify(AssistLayer, GJBaseGameLayer) {
    struct Fields {
        int step = 0;
        int epoch = -1;
        std::deque<Queued> queue[2];
        EarlyState early[2];
        int rateCount = 0;
        std::chrono::steady_clock::time_point rateStart;
    };

    // ---- helpers -----------------------------------------------------------

    bool isPlayLayer() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == this;
    }

    bool sharedDual() {
        return m_gameState.m_isDualMode && m_levelSettings && !m_levelSettings->m_twoPlayerMode;
    }

    PlayerObject* playerAt(int i) {
        return i == 0 ? m_player1 : m_player2;
    }

    void syncReset() {
        auto f = m_fields.operator->();
        if (f->epoch == resetEpoch()) return;
        f->epoch = resetEpoch();

        bool owedRelease = false;
        for (int i = 0; i < 2; i++) {
            if (!f->queue[i].empty() || f->early[i].rescueHeld) owedRelease = true;
            f->queue[i].clear();
            f->early[i] = EarlyState();
        }
        // A held-back release may never have reached the game. Releasing a
        // button that is not held is harmless.
        if (owedRelease) {
            if (m_player1) m_player1->releaseButton(PlayerButton::Jump);
            if (m_player2) m_player2->releaseButton(PlayerButton::Jump);
        }
        stats() = Stats();
        overlayClearPath();
        overlayRefresh();
    }

    // ---- delivery ----------------------------------------------------------

    // Hands an input to the game and updates early-buffer bookkeeping.
    // `tick` is the physics step the game will apply it on.
    void deliver(int i, bool down, int tick) {
        auto f = m_fields.operator->();

        int first = i, last = i;
        if (i == 0 && sharedDual()) last = 1;

        for (int j = first; j <= last; j++) {
            auto p = playerAt(j);
            if (!p) continue;
            auto& s = f->early[j];

            if (down) {
                s.held = true;
                s.pressStep = tick;
                bool early = cfg().earlyBuffer && cfg().earlyWindow > 0 &&
                             isGroundMode(p) && !p->m_isOnGround && ringsTouching(p) == 0;
                s.pending = early;
                s.pendingStart = tick;
                s.tapLen = 0;
            } else {
                s.held = false;
                if (s.pending && s.tapLen == 0) s.tapLen = std::max(1, tick - s.pressStep);
            }
        }

        GJBaseGameLayer::handleButton(down, JUMP_BUTTON, i == 0);
    }

    // ---- predictive delay --------------------------------------------------

    bool shouldPredict(PlayerObject* p, bool down) {
        auto const& c = cfg();
        if (!c.predictive || c.maxDelay <= 0) return false;
        if (!down && !c.assistReleases) return false;
        if (!c.predictMode[modeOf(p)]) return false;
        return true;
    }

    // Returns the delay (in steps from the next step) to apply the input at.
    int chooseDelay(PlayLayer* pl, int i, bool down, int minDelay) {
        auto f = m_fields.operator->();
        auto p = playerAt(i);
        int nextStep = f->step + 1;

        // Inputs already waiting in the queue are part of every simulation.
        std::vector<SimEvent> base;
        for (auto const& q : f->queue[i]) base.push_back({q.fireStep - nextStep, q.down});

        std::vector<PlayerObject*> sims = {p};
        if (i == 0 && sharedDual() && m_player2) sims.push_back(m_player2);

        int look = isGroundMode(p) ? cfg().lookGround : cfg().lookFly;
        bool wantPath = cfg().showPath;

        auto test = [&](int d, SimResult* out) {
            auto events = base;
            events.push_back({d, down});
            bool ok = true;
            for (auto sp : sims) {
                auto r = simulate(pl, sp, events, d + look, wantPath && sp == p);
                if (sp == p && out) *out = r;
                if (r.died) ok = false;
            }
            return ok;
        };

        SimResult now;
        if (test(minDelay, &now)) {
            return minDelay;
        }

        int maxD = std::max(minDelay, cfg().maxDelay);
        for (int d = minDelay + 1; d <= maxD; d++) {
            SimResult r;
            if (test(d, &r)) {
                if (wantPath) overlayShowPath(r.path, false);
                if (cfg().debug) {
                    log::info("[Assist] P{} {} {} delayed +{} steps ({} would die after {} steps)",
                              i + 1, modeName(modeOf(p)), down ? "press" : "release", d,
                              down ? "pressing now" : "releasing now", now.ticks);
                }
                return d;
            }
        }

        if (wantPath) overlayShowPath(now.path, true);
        if (cfg().debug) {
            log::info("[Assist] P{} {} {}: no delay up to {} survives, sent as normal",
                      i + 1, modeName(modeOf(p)), down ? "press" : "release", maxD);
        }
        return minDelay;
    }

    // ---- hooks -------------------------------------------------------------

    void handleButton(bool down, int button, bool isPlayer1) {
        if (simActive() || button != JUMP_BUTTON || !cfg().enabled || !isPlayLayer() ||
            !m_levelSettings || m_levelSettings->m_platformerMode) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        syncReset();
        auto f = m_fields.operator->();
        auto pl = PlayLayer::get();
        int i = isPlayer1 ? 0 : 1;
        auto p = playerAt(i);
        if (!p || p->m_isDead || pl->m_hasCompletedLevel) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int nextStep = f->step + 1;
        auto& q = f->queue[i];

        // Keep order: a new input never fires before one already waiting, and
        // a release lands at least one step after its press.
        int minDelay = 0;
        if (!q.empty()) {
            auto const& lastQ = q.back();
            minDelay = std::max(0, lastQ.fireStep - nextStep + (lastQ.down != down ? 1 : 0));
        }

        int delay = minDelay;
        if (shouldPredict(p, down) && simReady(pl)) {
            delay = chooseDelay(pl, i, down, minDelay);
        }

        if (delay > minDelay) {
            stats().delayed++;
            stats().last = fmt::format("+{} {}", delay - minDelay, down ? "click" : "release");
            overlayRefresh();
        }

        if (delay == 0 && q.empty()) {
            deliver(i, down, nextStep);
            return;
        }
        q.push_back({nextStep + delay, down});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.operator->();
        f->step++;

        // Debug: report how many times per real second this runs, to confirm
        // one call per physics step (should match your TPS, 240 by default).
        if (cfg().debug) {
            using clock = std::chrono::steady_clock;
            auto now = clock::now();
            if (f->rateCount == 0) f->rateStart = now;
            f->rateCount++;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - f->rateStart).count();
            if (ms >= 2000) {
                log::info("[Assist] {:.0f} steps per second (halfTick={}, lastTick={})",
                          f->rateCount * 1000.0 / ms, isHalfTick, isLastTick);
                f->rateCount = 0;
            }
        }

        // Runs even when the mod is switched off, so inputs already held back
        // and releases already owed always reach the game.
        bool active = isPlayLayer() && !simActive();
        if (active) {
            syncReset();
            // Delayed inputs due this step go in before the game processes
            // its input queue.
            for (int i = 0; i < 2; i++) {
                auto& q = f->queue[i];
                while (!q.empty() && q.front().fireStep <= f->step) {
                    bool down = q.front().down;
                    q.pop_front();
                    deliver(i, down, f->step);
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (active) {
            stepEarly(0);
            stepEarly(1);
        }
    }

    // ---- early-click buffer --------------------------------------------------

    void stepEarly(int j) {
        auto f = m_fields.operator->();
        auto p = playerAt(j);
        if (!p) return;
        auto& s = f->early[j];
        int step = f->step;

        if (s.rescueHeld && step >= s.rescueReleaseAt) {
            s.rescueHeld = false;
            if (!s.held) p->releaseButton(PlayerButton::Jump);
        }

        if (!s.pending) return;

        int waited = step - s.pendingStart;
        if (!cfg().enabled || !cfg().earlyBuffer || waited > cfg().earlyWindow || !isGroundMode(p) || p->m_isDead) {
            s.pending = false;
            return;
        }

        bool onRing = ringsTouching(p) > 0;
        if (!p->m_isOnGround && !onRing) return;

        s.pending = false;
        if (s.held) return;  // still holding: vanilla already jumps on landing

        p->pushButton(PlayerButton::Jump);
        s.rescueHeld = true;
        s.rescueReleaseAt = step + std::max(1, s.tapLen);

        stats().buffered++;
        stats().last = fmt::format("{} buffered ({} early)", onRing ? "orb" : "landing", waited);
        overlayRefresh();
        if (cfg().debug) {
            log::info("[Assist] P{} {} early click replayed on {} after {} steps, held {} steps",
                      j + 1, modeName(modeOf(p)), onRing ? "orb" : "ground", waited, std::max(1, s.tapLen));
        }
    }
};
