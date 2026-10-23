// Input handling.
//
// 1. Predictive delay: when a click or release arrives, simulate it on the
//    next physics step. If that dies within the look-ahead but holding it back
//    1..N steps survives, hold it back by the smallest such delay.
//
// 2. Auto-click save: every physics step while the button is up, simulate
//    doing nothing. If that dies within the save window but a click right now
//    survives, click for the player. This is what helps with late clicks.
//
// 3. Early-click buffer: a press in the air (cube, ball, robot, spider) that
//    is released before landing is replayed when the player touches the ground
//    or an orb within the window, keeping the original tap length. This covers
//    orbs, which the simulation does not model.

#include "State.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>

#include <chrono>
#include <cmath>
#include <deque>

using namespace assist;

namespace {

struct Queued {
    int fireStep;
    bool down;
    bool p1Key;  // the flag the game sent, passed back unchanged
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
constexpr int SAVE_JUMP_STEPS = 110;    // a full cube jump at 240 TPS is about 100 steps
constexpr int SAVE_SWALLOW_SLACK = 8;
constexpr int LEARN_SWALLOW_STEPS = 12;     // a click this soon after a learned click is the same click, late
constexpr float LEARN_SAVE_SHADOW = 80.f;   // no simulation auto-click this close before a learned window   // steps past the avoided death a late click is still dropped

// Plan-ahead search for ground-mode auto-clicks.
constexpr int PLAN_DEPTH = 2;           // follow-up clicks tried after the auto-click
constexpr int PLAN_STRIDE = 6;          // steps between follow-up click times tried
constexpr int PLAN_BUDGET_US = 1500;    // search time per physics step before giving up
constexpr int PLAN_LAST_CHANCE = 2;     // this close to death, click even into a dead end

enum class Plan { Viable, DeadEnd, OutOfTime };

using PlanClock = std::chrono::steady_clock;

// Is there a way to survive `horizon` steps that starts with `events`, adding
// up to `depth` more clicks? Follow-up clicks are tried latest first, since a
// click just before the death is the usual way past it.
Plan findPlan(PlayLayer* pl, std::vector<PlayerObject*> const& sims, std::vector<SimEvent>& events,
              int horizon, int depth, int hold, PlanClock::time_point deadline) {
    if (PlanClock::now() > deadline) return Plan::OutOfTime;

    int diedAt = -1;
    for (auto sp : sims) {
        auto r = simulate(pl, sp, events, horizon, false);
        if (r.died) diedAt = diedAt < 0 ? r.ticks : std::min(diedAt, r.ticks);
    }
    if (diedAt < 0) return Plan::Viable;
    if (depth == 0) return Plan::DeadEnd;

    int earliest = events.empty() ? 0 : events.back().tick + 1;
    for (int t = diedAt - 1; t >= earliest; t -= PLAN_STRIDE) {
        events.push_back({t, true});
        events.push_back({t + hold, false});
        auto res = findPlan(pl, sims, events, horizon, depth - 1, hold, deadline);
        events.pop_back();
        events.pop_back();
        if (res != Plan::DeadEnd) return res;
    }
    return Plan::DeadEnd;
}

}

class $modify(AssistLayer, GJBaseGameLayer) {
    struct Fields {
        int step = 0;
        int epoch = -1;
        std::deque<Queued> queue[2];
        EarlyState early[2];
        int saveCooldown[2] = {0, 0};
        int swallowUntil[2] = {0, 0};      // after a save, the player's own late click is dropped
        bool swallowRelease[2] = {false, false};
        // Learned windows (player 1 only).
        float lastX = 0.f;
        float dx = 0.f;                    // x moved per physics step
        float lastPressX = -1e9f;          // where the last press reached the game
        float learnedDoneHi = -1e9f;       // window already clicked for this attempt
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
            f->saveCooldown[i] = 0;
            f->swallowUntil[i] = 0;
            f->swallowRelease[i] = false;
        }
        f->lastPressX = -1e9f;
        f->learnedDoneHi = -1e9f;
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
    void deliver(int i, bool down, int tick, bool p1Key) {
        auto f = m_fields.operator->();

        int first = i, last = i;
        if (i == 0 && sharedDual()) last = 1;

        for (int j = first; j <= last; j++) {
            auto p = playerAt(j);
            if (!p) continue;
            auto& s = f->early[j];

            if (down) {
                if (j == 0) f->lastPressX = p->getPositionX();
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

        GJBaseGameLayer::handleButton(down, JUMP_BUTTON, p1Key);
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
        // Outside two-player mode the player-2 keys (Up arrow, W) also drive
        // player 1, so only two-player mode splits inputs by player.
        int i = (isPlayer1 || !m_levelSettings->m_twoPlayerMode) ? 0 : 1;
        auto p = playerAt(i);
        if (!p || p->m_isDead || pl->m_hasCompletedLevel) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int nextStep = f->step + 1;
        auto& q = f->queue[i];

        // Right after an auto-click in a ground mode, the player's own click
        // for the same obstacle is late by definition. Delivering it would
        // buffer a second jump, so drop it and its release.
        if (isGroundMode(p)) {
            if (down && nextStep <= f->swallowUntil[i]) {
                f->swallowUntil[i] = 0;
                f->swallowRelease[i] = true;
                if (cfg().debug) log::info("[Assist] P{} late click after auto-click dropped", i + 1);
                record(p, "press", "dropped: late click right after an auto-click");
                return;
            }
            if (!down && f->swallowRelease[i]) {
                f->swallowRelease[i] = false;
                return;
            }
        }

        // Keep order: a new input never fires before one already waiting, and
        // a release lands at least one step after its press.
        int minDelay = 0;
        if (!q.empty()) {
            auto const& lastQ = q.back();
            minDelay = std::max(0, lastQ.fireStep - nextStep + (lastQ.down != down ? 1 : 0));
        }

        int delay = minDelay;
        bool learned = false;
        // A learned window near this press decides its timing: hold an early
        // press back to where clicks have worked before. Inside a window the
        // press goes through as is.
        if (down && i == 0 && cfg().learn && isGroundMode(p) && f->dx > 0.f) {
            float x = p->getPositionX();
            if (auto w = learnWindowNear(x, cfg().learnEarly * f->dx, 0.f)) {
                learned = true;
                if (x < w->lo) {
                    int d = static_cast<int>(std::ceil((w->lo - x) / f->dx));
                    delay = std::max(minDelay, d);
                }
            }
        }
        if (!learned && shouldPredict(p, down) && simReady(pl)) {
            delay = chooseDelay(pl, i, down, minDelay);
        }

        record(p, down ? "press" : "release",
               delay > minDelay ? fmt::format("{}delayed {} steps", learned ? "learned " : "", delay - minDelay)
                                : std::string());

        if (delay > minDelay) {
            stats().delayed++;
            stats().last = fmt::format("+{} {}", delay - minDelay, down ? "click" : "release");
            overlayRefresh();
        }

        if (delay == 0 && q.empty()) {
            deliver(i, down, nextStep, isPlayer1);
            return;
        }
        q.push_back({nextStep + delay, down, isPlayer1});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.operator->();
        f->step++;
        watch(0).step = f->step;
        watch(1).step = f->step;

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
            if (m_player1) {
                float x = m_player1->getPositionX();
                float d = x - f->lastX;
                if (d > 0.01f && d < 30.f) f->dx = d;
                f->lastX = x;
            }
            // Delayed inputs due this step go in before the game processes
            // its input queue.
            for (int i = 0; i < 2; i++) {
                auto& q = f->queue[i];
                while (!q.empty() && q.front().fireStep <= f->step) {
                    auto item = q.front();
                    q.pop_front();
                    deliver(i, item.down, f->step, item.p1Key);
                    if (item.down) record(playerAt(i), "press-fired", "");
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (active) {
            stepEarly(0);
            stepEarly(1);
            stepLearned();
            stepSave(0);
            stepSave(1);
        }
    }

    // ---- learned windows -----------------------------------------------------

    // Clicks for player 1 at the end of a learned window if they have not
    // clicked since just before it.
    void stepLearned() {
        auto const& c = cfg();
        auto f = m_fields.operator->();
        auto p = m_player1;
        auto pl = PlayLayer::get();
        if (!c.enabled || !c.learn || !p || p->m_isDead || f->dx <= 0.f) return;
        if (!pl || pl->m_hasCompletedLevel || !m_levelSettings || m_levelSettings->m_platformerMode) return;
        if (!isGroundMode(p)) return;

        float x = p->getPositionX();
        auto w = learnWindowNear(x, 0.f, 0.f);
        if (!w || w->hi <= f->learnedDoneHi) return;
        // Last step inside the window: the next one passes its end.
        if (x + f->dx <= w->hi) return;

        f->learnedDoneHi = w->hi;
        if (f->lastPressX >= w->lo - c.learnEarly * f->dx) return;  // the player already clicked for it
        if (!f->queue[0].empty()) return;                              // a held-back click is about to land
        auto const& s = f->early[0];
        if (s.held || s.rescueHeld || holdingJump(p)) return;

        std::vector<PlayerObject*> targets = {p};
        if (sharedDual() && m_player2) targets.push_back(m_player2);
        int step = f->step;
        for (auto sp : targets) {
            int k = sp == m_player1 ? 0 : 1;
            sp->pushButton(PlayerButton::Jump);
            f->early[k].rescueHeld = true;
            f->early[k].rescueReleaseAt = step + w->hold;
        }
        f->lastPressX = x;
        f->saveCooldown[0] = step + w->hold + 2;
        f->swallowUntil[0] = step + LEARN_SWALLOW_STEPS;

        watch(0).clickStep = step;
        record(p, "learned-click", fmt::format("window {:.1f}-{:.1f} from {} good clicks", w->lo, w->hi, w->n));
        stats().learned++;
        stats().last = fmt::format("learned click at {:.0f}", x);
        overlayRefresh();
    }

    // ---- auto-click save -----------------------------------------------------

    bool holdingJump(PlayerObject* p) {
        auto it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        return it != p->m_holdingButtons.end() && it->second;
    }

    void stepSave(int j) {
        auto const& c = cfg();
        if (!c.enabled || !c.autoSave || c.saveWindow <= 0) return;
        if (!m_levelSettings || m_levelSettings->m_platformerMode) return;
        // In shared dual mode player 1's button drives both; handle it once.
        if (j == 1 && (!m_gameState.m_isDualMode || sharedDual())) return;

        auto f = m_fields.operator->();
        auto pl = PlayLayer::get();
        auto p = playerAt(j);
        if (!pl || !p || p->m_isDead || pl->m_hasCompletedLevel || !simReady(pl)) return;
        if (!c.predictMode[modeOf(p)]) return;
        if (f->step < f->saveCooldown[j]) return;
        // Where learned windows exist, they handle the timing; the simulation
        // is often a step off on tight clicks.
        if (j == 0 && c.learn && learnWindowNear(p->getPositionX(), LEARN_SAVE_SHADOW, 0.f)) return;

        std::vector<PlayerObject*> sims = {p};
        if (j == 0 && sharedDual() && m_player2) sims.push_back(m_player2);

        // Never fight the player: only step in while the button is up and
        // nothing of ours is waiting.
        char const* blocked = nullptr;
        if (!f->queue[0].empty() || !f->queue[1].empty()) blocked = "your input is queued";
        for (auto sp : sims) {
            int k = sp == m_player1 ? 0 : 1;
            auto const& s = f->early[k];
            if (s.held) blocked = "you are holding the button";
            else if (holdingJump(sp)) blocked = "the game thinks the button is held";
            else if (s.pending || s.rescueHeld) blocked = "an earlier assist input is active";
        }
        // Only the recorder needs to know about dangers while blocked.
        if (blocked && !c.record) return;

        int deathIn = -1;
        for (auto sp : sims) {
            auto idle = simulate(pl, sp, {}, c.saveWindow, false);
            if (idle.died) deathIn = deathIn < 0 ? idle.ticks : std::min(deathIn, idle.ticks);
            if (idle.unknown) watch(j).blindStep = f->step;
        }
        if (deathIn < 0) return;
        watch(j).dangerStep = f->step;
        // Logged every step while in danger, so a recorded session shows the
        // whole lead-up to a death.
        auto noSave = [&](char const* why, std::string const& extra = "") {
            watch(j).danger = why;
            record(p, "no-save", fmt::format("{} (death in {} steps){}", why, deathIn, extra));
        };
        if (blocked) {
            noSave(blocked);
            return;
        }

        int hold = std::max(1, c.saveHold);
        // Ground modes: the click has to survive the whole jump, not just its
        // start, or it jumps over one spike and lands on the next.
        int look = isGroundMode(p) ? std::max(c.lookGround, SAVE_JUMP_STEPS) : c.lookFly;
        look = std::max(look, hold + 1);
        std::vector<SimEvent> events = {{0, true}, {hold, false}};
        SimResult shown;
        for (auto sp : sims) {
            auto r = simulate(pl, sp, events, look, c.showPath && sp == p);
            if (sp == p) shown = r;
            if (r.died) {
                if (c.debug) {
                    log::info("[Assist] P{} {}: death in {} steps, clicking now doesn't survive either",
                              j + 1, modeName(modeOf(p)), deathIn);
                }
                // Try again next step: clicking a little later may survive.
                noSave("no click right now survives",
                       fmt::format("; clicking now dies after {} steps", r.ticks));
                return;
            }
        }
        // Wait for the last step a click still survives, so a player who is
        // about to click gets to do it themselves.
        if (deathIn > 1) {
            std::vector<SimEvent> later = {{1, true}, {1 + hold, false}};
            bool laterOk = true;
            for (auto sp : sims) {
                if (simulate(pl, sp, later, look + 1, false).died) laterOk = false;
            }
            if (laterOk) {
                noSave("waiting for the last safe moment");
                return;
            }
        }
        // Ground modes: clearing this obstacle is not enough if the jump lands
        // somewhere with no way out. Look further ahead, allowing follow-up
        // clicks, and hold off while this click only leads to a dead end; a
        // later step may find a click that does not. If the search runs out of
        // time, the basic check above stands.
        if (isGroundMode(p) && c.planAhead > look) {
            auto plan = events;
            auto deadline = PlanClock::now() + std::chrono::microseconds(PLAN_BUDGET_US);
            auto res = findPlan(pl, sims, plan, c.planAhead, PLAN_DEPTH, hold, deadline);
            if (res == Plan::DeadEnd && deathIn > PLAN_LAST_CHANCE) {
                if (c.debug) {
                    log::info("[Assist] P{} {}: death in {} steps, clicking now leads to a dead end, waiting",
                              j + 1, modeName(modeOf(p)), deathIn);
                }
                noSave("clicking now leads to a dead end");
                return;
            }
            if (c.debug && res == Plan::OutOfTime) {
                log::info("[Assist] P{} {}: plan-ahead ran out of time, using the short check", j + 1, modeName(modeOf(p)));
            }
        }

        if (c.showPath) overlayShowPath(shown.path, false);

        int step = f->step;
        for (auto sp : sims) {
            int k = sp == m_player1 ? 0 : 1;
            sp->pushButton(PlayerButton::Jump);
            f->early[k].rescueHeld = true;
            f->early[k].rescueReleaseAt = step + hold;
        }
        f->saveCooldown[j] = step + hold + 2;
        if (isGroundMode(p)) f->swallowUntil[j] = step + deathIn + SAVE_SWALLOW_SLACK;

        watch(j).clickStep = step;
        watch(j).danger.clear();
        record(p, "auto-click", fmt::format("death was {} steps away", deathIn));

        stats().saved++;
        stats().last = fmt::format("auto-click ({} steps from death)", deathIn);
        overlayRefresh();
        if (c.debug) {
            log::info("[Assist] P{} {}: auto-click, death was {} steps away, held {} steps",
                      j + 1, modeName(modeOf(p)), deathIn, hold);
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
        record(p, "buffered", fmt::format("{} after {} steps early", onRing ? "orb" : "landing", waited));
        stats().last = fmt::format("{} buffered ({} early)", onRing ? "orb" : "landing", waited);
        overlayRefresh();
        if (cfg().debug) {
            log::info("[Assist] P{} {} early click replayed on {} after {} steps, held {} steps",
                      j + 1, modeName(modeOf(p)), onRing ? "orb" : "ground", waited, std::max(1, s.tapLen));
        }
    }
};
