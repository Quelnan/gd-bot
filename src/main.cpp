#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdint>

using namespace geode::prelude;

// ============================================================
// Settings
// ============================================================

struct Settings {
    bool enabled = false;

    int updatesPerFrame = 120;
    int innerBudgetMs = 10;

    int maxFrames = 400000;

    int tapLen = 2;
    int maxShift = 40;
    int insertOffset = 10;

    int window = 800;
    int prefixLock = 120;

    bool verifyAfterWin = true;
    bool logStatus = true;

    void load() {
        auto* m = Mod::get();
        enabled = m->getSettingValue<bool>("enabled");

        updatesPerFrame = (int)m->getSettingValue<int64_t>("updates-per-frame");
        innerBudgetMs = (int)m->getSettingValue<int64_t>("inner-budget-ms");

        maxFrames = (int)m->getSettingValue<int64_t>("max-frames");

        tapLen = (int)m->getSettingValue<int64_t>("tap-length");
        maxShift = (int)m->getSettingValue<int64_t>("max-shift");
        insertOffset = (int)m->getSettingValue<int64_t>("insert-offset");

        window = (int)m->getSettingValue<int64_t>("window");
        prefixLock = (int)m->getSettingValue<int64_t>("prefix-lock");

        verifyAfterWin = m->getSettingValue<bool>("verify-after-win");
        logStatus = m->getSettingValue<bool>("log-status");

        updatesPerFrame = std::clamp(updatesPerFrame, 1, 2000);
        innerBudgetMs = std::clamp(innerBudgetMs, 1, 50);

        maxFrames = std::clamp(maxFrames, 20000, 1500000);

        tapLen = std::clamp(tapLen, 1, 20);
        maxShift = std::clamp(maxShift, 1, 600);
        insertOffset = std::clamp(insertOffset, 1, 300);

        window = std::clamp(window, 60, 8000);
        prefixLock = std::clamp(prefixLock, 0, 8000);
    }
};

static Settings g_set;

// ============================================================
// Engine-derived mode (for “tap default, hold when needed”)
// ============================================================

enum class Mode : uint8_t { Cube=0, Ship=1, Ball=2, UFO=3, Wave=4, Robot=5, Spider=6, Swing=7 };

static Mode readMode(PlayerObject* p) {
    if (!p) return Mode::Cube;
    if (p->m_isShip)   return Mode::Ship;
    if (p->m_isBall)   return Mode::Ball;
    if (p->m_isBird)   return Mode::UFO;
    if (p->m_isDart)   return Mode::Wave;
    if (p->m_isRobot)  return Mode::Robot;
    if (p->m_isSpider) return Mode::Spider;
    if (p->m_isSwing)  return Mode::Swing;
    return Mode::Cube;
}

// ============================================================
// Segment plan: default = release, segments = hold in [start, end)
// ============================================================

struct Segment {
    int start = 0;
    int end = 0; // exclusive
};

static void normalizeSegments(std::vector<Segment>& segs, int maxFrames) {
    for (auto& s : segs) {
        s.start = std::clamp(s.start, 0, maxFrames);
        s.end   = std::clamp(s.end,   0, maxFrames);
        if (s.end < s.start) std::swap(s.start, s.end);
    }
    segs.erase(std::remove_if(segs.begin(), segs.end(), [](const Segment& s){
        return s.end <= s.start;
    }), segs.end());

    std::sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b){
        return a.start < b.start;
    });

    // merge overlaps
    std::vector<Segment> out;
    out.reserve(segs.size());
    for (auto const& s : segs) {
        if (out.empty() || s.start > out.back().end) out.push_back(s);
        else out.back().end = std::max(out.back().end, s.end);
    }
    segs.swap(out);
}

// O(1) scanning using segIdx
static bool holdAtFrame(const std::vector<Segment>& segs, int frame, size_t& segIdx) {
    while (segIdx < segs.size() && frame >= segs[segIdx].end) segIdx++;
    if (segIdx >= segs.size()) return false;
    return frame >= segs[segIdx].start && frame < segs[segIdx].end;
}

// ============================================================
// Backtracking cursor (deterministic enumeration)
// ============================================================

struct Cursor {
    int backtrackDepth = 0;
    int shiftIndex = 0;
    int durIndex = 0;
    bool tryDelete = false;

    void reset() {
        backtrackDepth = 0;
        shiftIndex = 0;
        durIndex = 0;
        tryDelete = false;
    }
};

static Cursor g_cursor;

// shift order: 0, -1, +1, -2, +2, ...
static std::vector<int> makeShiftList(int maxShift) {
    std::vector<int> s;
    s.reserve((size_t)maxShift * 2 + 1);
    s.push_back(0);
    for (int d = 1; d <= maxShift; d++) {
        s.push_back(-d);
        s.push_back(+d);
    }
    return s;
}

// duration list based on the mode where we are trying to solve:
// - cube/ball/ufo/spider: taps only (hold is dangerous due to orbs / edge-trigger behavior)
// - robot: allow longer (hold affects jump height)
// - ship/wave/swing: allow much longer holds
static std::vector<int> makeDurationList(Mode m) {
    int t = g_set.tapLen;
    switch (m) {
        case Mode::Ship:
        case Mode::Wave:
        case Mode::Swing:
            return { t, t*4, t*16, t*64, t*256 };
        case Mode::Robot:
            return { t, t*2, t*4, t*8, t*16 };
        default:
            return { t };
    }
}

// pick “last decision” segment near a center frame
static int pickSegmentToModify(const std::vector<Segment>& bestSegs, int centerFrame, int lockFrame, int window, int backtrackDepth) {
    if (bestSegs.empty()) return -1;

    int startMin = std::max(lockFrame, centerFrame - window);
    int startMax = centerFrame;

    std::vector<int> eligible;
    eligible.reserve(bestSegs.size());

    for (int i = 0; i < (int)bestSegs.size(); i++) {
        int st = bestSegs[i].start;
        if (st >= startMin && st < startMax) eligible.push_back(i);
    }
    if (eligible.empty()) return -1;

    int idx = (int)eligible.size() - 1 - backtrackDepth;
    if (idx < 0) return -1;
    return eligible[idx];
}

// Produce next candidate plan from best plan, deterministic backtracking.
static bool nextCandidate(
    std::vector<Segment>& out,
    const std::vector<Segment>& best,
    int centerFrame,
    int lockFrame,
    Mode centerMode
) {
    auto shifts = makeShiftList(g_set.maxShift);
    auto durs   = makeDurationList(centerMode);

    int segIndex = pickSegmentToModify(best, centerFrame, lockFrame, g_set.window, g_cursor.backtrackDepth);

    // If no segment near center, insert a new one near frontier
    if (segIndex < 0) {
        if (g_cursor.durIndex >= (int)durs.size()) {
            g_cursor.durIndex = 0;
            g_cursor.shiftIndex++;
        }
        if (g_cursor.shiftIndex >= (int)shifts.size()) {
            g_cursor.shiftIndex = 0;
            g_cursor.durIndex = 0;
            return false;
        }

        int baseStart = std::max(lockFrame, centerFrame - g_set.insertOffset);
        int start = std::clamp(baseStart + shifts[g_cursor.shiftIndex], lockFrame, g_set.maxFrames - 1);
        int dur = durs[g_cursor.durIndex];
        g_cursor.durIndex++;

        out = best;
        out.push_back({ start, start + dur });
        normalizeSegments(out, g_set.maxFrames);
        return true;
    }

    // deletion phase (after exhausting shifts/durations)
    if (g_cursor.tryDelete) {
        out = best;
        out.erase(out.begin() + segIndex);
        normalizeSegments(out, g_set.maxFrames);

        g_cursor.tryDelete = false;
        g_cursor.shiftIndex = 0;
        g_cursor.durIndex = 0;
        g_cursor.backtrackDepth++;
        return true;
    }

    // enumerate duration/shift combos
    if (g_cursor.durIndex >= (int)durs.size()) {
        g_cursor.durIndex = 0;
        g_cursor.shiftIndex++;
    }
    if (g_cursor.shiftIndex >= (int)shifts.size()) {
        g_cursor.shiftIndex = 0;
        g_cursor.durIndex = 0;
        g_cursor.tryDelete = true;
        return nextCandidate(out, best, centerFrame, lockFrame, centerMode);
    }

    int shift = shifts[g_cursor.shiftIndex];
    int dur = durs[g_cursor.durIndex];
    g_cursor.durIndex++;

    out = best;
    Segment s = out[segIndex];

    int newStart = s.start + shift;
    if (newStart < lockFrame) newStart = lockFrame;
    int newEnd = newStart + std::max(1, dur);

    out[segIndex] = { newStart, newEnd };
    normalizeSegments(out, g_set.maxFrames);
    return true;
}

// ============================================================
// Bot runtime state
// ============================================================

enum class RunMode { Idle, Training, Verify };

static RunMode g_run = RunMode::Idle;

static bool g_isHolding = false;
static bool g_prevDead = false;

static int g_frame = 0;
static size_t g_segIdx = 0;

static bool g_requestReset = false;

static float g_bestX = 0.f;
static int   g_bestFrame = 0;          // frontier frame of bestX
static int   g_lastDeathFrame = 0;     // last death frame of last attempt
static Mode  g_lastDeathMode = Mode::Cube;

static std::vector<Segment> g_bestSegs;
static std::vector<Segment> g_curSegs;

static bool g_hasWin = false;

static int g_logCounter = 0;

// Apply hold to engine (only when changed)
static void applyHold(GJBaseGameLayer* gl, bool hold) {
    if (!gl) return;
    if (hold == g_isHolding) return;
    gl->handleButton(hold, 1, true);
    g_isHolding = hold;
}

// ============================================================
// Attempt end handling
// ============================================================

static void commitBestProgress(float xNow) {
    // update best continuously but not too frequently (avoid heavy copying)
    // For these levels, 10 units is a good compromise.
    if (xNow > g_bestX + 10.f) {
        g_bestX = xNow;
        g_bestFrame = g_frame;
        g_bestSegs = g_curSegs;
        // Note: we do NOT reset cursor here; we only reset cursor when best improves at attempt end
        // because we still might die and need to backtrack.
    }
}

static void finishAttemptOnDeath(float xNow) {
    // update last death info
    g_lastDeathFrame = g_frame;

    // If this death reached further, promote it to best (already mostly committed, but ensure)
    if (xNow > g_bestX) {
        g_bestX = xNow;
        g_bestFrame = g_frame;
        g_bestSegs = g_curSegs;
        g_cursor.reset(); // new frontier, restart local search
    }
}

static void finishAttemptOnWin(float xNow) {
    g_hasWin = true;
    if (xNow > g_bestX) {
        g_bestX = xNow;
        g_bestFrame = g_frame;
        g_bestSegs = g_curSegs;
    }
}

// ============================================================
// Training start / verify start
// ============================================================

static void beginTraining() {
    g_run = RunMode::Training;

    g_bestX = 0.f;
    g_bestFrame = 0;
    g_lastDeathFrame = 0;
    g_lastDeathMode = Mode::Cube;

    g_bestSegs.clear();
    g_curSegs.clear();

    g_cursor.reset();

    g_hasWin = false;
    g_frame = 0;
    g_segIdx = 0;
    g_requestReset = true;
}

static void beginVerify() {
    g_run = RunMode::Verify;
    g_curSegs = g_bestSegs;
    normalizeSegments(g_curSegs, g_set.maxFrames);
    g_cursor.reset();
    g_frame = 0;
    g_segIdx = 0;
    g_requestReset = true;
}

// ============================================================
// Hooks
// ============================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_set.load();

        g_run = RunMode::Idle;
        g_isHolding = false;
        g_prevDead = false;

        g_bestSegs.clear();
        g_curSegs.clear();
        g_cursor.reset();

        g_bestX = 0.f;
        g_bestFrame = 0;
        g_lastDeathFrame = 0;
        g_lastDeathMode = Mode::Cube;
        g_hasWin = false;

        g_frame = 0;
        g_segIdx = 0;

        g_requestReset = false;
        g_logCounter = 0;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // release button
        if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
        g_isHolding = false;

        g_prevDead = false;
        g_requestReset = false;

        g_frame = 0;
        g_segIdx = 0;

        g_set.load();

        if (g_run == RunMode::Training) {
            // Decide center of search: prioritize extending frontier
            int center = std::max(g_bestFrame, g_lastDeathFrame);
            if (center <= 0) center = 60; // early default

            int lockFrame = std::max(0, (g_bestFrame - g_set.window) - g_set.prefixLock);

            // Determine “center mode” from last death mode (tap default, hold when needed)
            Mode centerMode = g_lastDeathMode;

            std::vector<Segment> cand;
            bool ok = nextCandidate(cand, g_bestSegs, center, lockFrame, centerMode);
            if (!ok) {
                // escalate backtracking depth if exhausted
                g_cursor.reset();
                g_cursor.backtrackDepth++;
                nextCandidate(cand, g_bestSegs, center, lockFrame, centerMode);
            }

            g_curSegs = cand;
            normalizeSegments(g_curSegs, g_set.maxFrames);
        }
        else if (g_run == RunMode::Verify) {
            g_curSegs = g_bestSegs;
            normalizeSegments(g_curSegs, g_set.maxFrames);
        }
    }

    void onQuit() {
        if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
        g_isHolding = false;

        g_run = RunMode::Idle;
        PlayLayer::onQuit();
    }
};

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        g_set.load();

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // If disabled, do normal update and stop holding.
        if (!g_set.enabled) {
            if (g_isHolding) applyHold(this, false);
            GJBaseGameLayer::update(dt);
            return;
        }

        // Pause -> do normal update once
        if (pl->m_isPaused) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // Start training when entering gameplay
        if (g_run == RunMode::Idle) {
            beginTraining();
        }

        // Handle scheduled reset outside the inner loop
        if (g_requestReset) {
            g_requestReset = false;
            pl->resetLevel();
            return;
        }

        // Deterministic dt: use the game’s animation interval (fixed)
        float fixedDT = cocos2d::CCDirector::sharedDirector()->getAnimationInterval();

        // Fast-forward factor
        int stepsTarget = (g_run == RunMode::Training) ? g_set.updatesPerFrame : 1;

        auto t0 = std::chrono::high_resolution_clock::now();

        for (int step = 0; step < stepsTarget; step++) {
            if (pl->m_hasCompletedLevel) break;
            if (m_player1->m_isDead) break;

            // Decide hold from segments
            bool desiredHold = holdAtFrame(g_curSegs, g_frame, g_segIdx);
            applyHold(this, desiredHold);

            // Run one real engine tick
            GJBaseGameLayer::update(fixedDT);

            g_frame++;

            // Update best continuously while alive (learning acceleration)
            float xNow = m_player1->getPositionX();
            commitBestProgress(xNow);

            // failsafe
            if (g_frame >= g_set.maxFrames) {
                applyHold(this, false);
                g_requestReset = true;
                break;
            }

            // time budget per rendered frame
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            if (ms >= g_set.innerBudgetMs) break;
        }

        // After inner loop, check death/completion
        bool deadNow = m_player1->m_isDead;
        bool completeNow = pl->m_hasCompletedLevel;
        float xNow = m_player1->getPositionX();

        if (!g_prevDead && deadNow) {
            g_lastDeathMode = readMode(m_player1);
            finishAttemptOnDeath(xNow);
            applyHold(this, false);
            g_requestReset = true;
        }
        g_prevDead = deadNow;

        if (completeNow) {
            finishAttemptOnWin(xNow);
            applyHold(this, false);

            if (g_run == RunMode::Training && g_set.verifyAfterWin) {
                beginVerify();
            } else {
                // stop once won if not verifying
                Mod::get()->setSettingValue("enabled", false);
            }
        }

        // periodic logs (only meaningful during gameplay)
        if (g_set.logStatus) {
            g_logCounter++;
            if (g_logCounter % 240 == 0) {
                log::info("Run={} frame={} x={:.1f} bestX={:.1f} bestFrame={} segsBest={} segsCur={} lastDeathFrame={}",
                    (int)g_run, g_frame, xNow, g_bestX, g_bestFrame,
                    g_bestSegs.size(), g_curSegs.size(), g_lastDeathFrame);
            }
        }
    }
};

// Pause menu: toggle + settings
class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto ws = CCDirector::sharedDirector()->getWinSize();
        auto* menu = CCMenu::create();
        menu->setPosition(ccp(0, 0));
        addChild(menu, 200);

        auto* onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* togg = CCMenuItemToggler::create(offSpr, onSpr, this, menu_selector(BotPauseLayer::onToggle));
        togg->setPosition(ccp(ws.width - 30.f, ws.height - 30.f));
        togg->toggle(Mod::get()->getSettingValue<bool>("enabled"));
        menu->addChild(togg);

        auto* gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        auto* gearBtn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(BotPauseLayer::onSettings));
        gearBtn->setPosition(ccp(ws.width - 30.f, ws.height - 75.f));
        gearBtn->setScale(0.7f);
        menu->addChild(gearBtn);
    }

    void onToggle(CCObject*) {
        bool v = !Mod::get()->getSettingValue<bool>("enabled");
        Mod::get()->setSettingValue("enabled", v);
    }

    void onSettings(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};

// F8 toggle
class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && key == KEY_F8) {
            bool v = !Mod::get()->getSettingValue<bool>("enabled");
            Mod::get()->setSettingValue("enabled", v);
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

$on_mod(Loaded) {
    g_set.load();
    log::info("GD AutoBot Backtracker (Engine) loaded. F8 toggles enabled. Pause gear opens settings.");
}
