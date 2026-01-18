#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <deque>
#include <algorithm>
#include <chrono>
#include <cstdint>

using namespace geode::prelude;

// ============================================================
// Settings
// ============================================================

struct Settings {
    bool enabled = false;

    int updatesPerFrame = 60;
    int innerBudgetMs = 8;

    int maxFrames = 300000;

    int tapLen = 2;
    int maxShift = 30;
    int insertOffset = 10;

    int window = 600;
    int prefixLock = 60;

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

        window = (int)m->getSettingValue<int64_t>("mutation-window");
        prefixLock = (int)m->getSettingValue<int64_t>("prefix-lock");

        verifyAfterWin = m->getSettingValue<bool>("verify-after-win");
        logStatus = m->getSettingValue<bool>("log-status");

        updatesPerFrame = std::clamp(updatesPerFrame, 1, 2000);
        innerBudgetMs = std::clamp(innerBudgetMs, 1, 40);

        maxFrames = std::clamp(maxFrames, 20000, 1500000);

        tapLen = std::clamp(tapLen, 1, 20);
        maxShift = std::clamp(maxShift, 1, 600);
        insertOffset = std::clamp(insertOffset, 1, 300);

        window = std::clamp(window, 60, 6000);
        prefixLock = std::clamp(prefixLock, 0, 6000);
    }
};

static Settings g_set;

// ============================================================
// Run state
// ============================================================

enum class RunMode {
    Idle,
    Training,
    Verify
};

static RunMode g_mode = RunMode::Idle;

static bool g_isHolding = false;
static bool g_prevDead = false;

// Apply jump hold state to the real engine (only sends input when it changes)
static void applyHold(GJBaseGameLayer* layer, bool hold) {
    if (!layer) return;
    if (hold == g_isHolding) return;
    layer->handleButton(hold, 1, true);
    g_isHolding = hold;
}

static int g_frame = 0;
static int g_bestDeathFrame = 0;
static float g_bestX = 0.f;
static bool g_hasWin = false;

static bool g_requestReset = false;

static int g_logCounter = 0;

// ============================================================
// Input representation: HOLD segments
// Default state = RELEASE. Any segment makes hold=true for [start, end).
// This supports both "tap" (short) and "hold" (long).
// ============================================================

struct Segment {
    int start = 0;
    int end = 0; // exclusive
};

static void normalizeSegments(std::vector<Segment>& segs, int maxFrames) {
    // clamp and remove invalid
    for (auto& s : segs) {
        s.start = std::clamp(s.start, 0, maxFrames);
        s.end = std::clamp(s.end, 0, maxFrames);
        if (s.end < s.start) std::swap(s.start, s.end);
    }
    segs.erase(std::remove_if(segs.begin(), segs.end(), [](const Segment& s) {
        return s.end <= s.start;
    }), segs.end());

    std::sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) {
        return a.start < b.start;
    });

    // merge overlaps
    std::vector<Segment> merged;
    merged.reserve(segs.size());
    for (auto const& s : segs) {
        if (merged.empty() || s.start > merged.back().end) {
            merged.push_back(s);
        } else {
            merged.back().end = std::max(merged.back().end, s.end);
        }
    }
    segs.swap(merged);
}

static bool holdAtFrame(const std::vector<Segment>& segs, int frame, size_t& segIdx) {
    // advance segIdx while frame >= end
    while (segIdx < segs.size() && frame >= segs[segIdx].end) segIdx++;
    if (segIdx >= segs.size()) return false;
    return frame >= segs[segIdx].start && frame < segs[segIdx].end;
}

// ============================================================
// “Analyzed attempts”: record mode per frame for best attempt (optional)
// For now we only store it; you can later use it to restrict holds to ship/wave.
// ============================================================

static std::vector<uint8_t> g_attemptModeTL;
static std::vector<uint8_t> g_bestModeTL;

// mode encoding 0..7
static uint8_t readMode(PlayerObject* p) {
    if (!p) return 0;
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

// ============================================================
// Candidate generator: backtracking systematic edits
// ============================================================

struct SearchCursor {
    int backtrackDepth = 0;   // 0 = last segment near death, 1 = previous, etc
    int durIndex = 0;
    int shiftIndex = 0;
    bool tryDelete = false;
    bool inserting = false;

    void reset() {
        backtrackDepth = 0;
        durIndex = 0;
        shiftIndex = 0;
        tryDelete = false;
        inserting = false;
    }
};

static SearchCursor g_cursor;

static std::vector<int> makeShiftList(int maxShift) {
    std::vector<int> shifts;
    shifts.reserve((size_t)(maxShift * 2 + 1));
    shifts.push_back(0);
    for (int d = 1; d <= maxShift; d++) {
        shifts.push_back(-d);
        shifts.push_back(d);
    }
    return shifts;
}

static std::vector<int> makeDurationList(int tapLen) {
    // tap first, then progressively longer holds
    // (still deterministic, “hold when needed”)
    return { tapLen, tapLen * 2, tapLen * 4, tapLen * 8, tapLen * 16 };
}

// Find candidate segment index to modify (last segment starting before bestDeathFrame within window),
// then apply backtrackDepth.
static int pickSegmentToModify(const std::vector<Segment>& bestSegs, int bestDeathFrame, int lockFrame, int window, int backtrackDepth) {
    if (bestSegs.empty()) return -1;

    int startMin = std::max(lockFrame, bestDeathFrame - window);
    int startMax = bestDeathFrame;

    // gather eligible indices
    std::vector<int> eligible;
    for (int i = 0; i < (int)bestSegs.size(); i++) {
        int st = bestSegs[i].start;
        if (st >= startMin && st < startMax) eligible.push_back(i);
    }
    if (eligible.empty()) return -1;

    int idx = (int)eligible.size() - 1 - backtrackDepth;
    if (idx < 0) return -1;
    return eligible[idx];
}

// Produce next candidate segments deterministically.
// Returns false if no more candidates (should widen parameters or restart search).
static bool nextCandidate(std::vector<Segment>& outSegs, const std::vector<Segment>& bestSegs, int bestDeathFrame) {
    int lockFrame = std::max(0, (bestDeathFrame - g_set.window) - g_set.prefixLock);

    auto shifts = makeShiftList(g_set.maxShift);
    auto durs   = makeDurationList(g_set.tapLen);

    // which segment to modify?
    int segIndex = pickSegmentToModify(bestSegs, bestDeathFrame, lockFrame, g_set.window, g_cursor.backtrackDepth);

    // If none, insert a new tap near death (deterministic)
    if (segIndex < 0) {
        g_cursor.inserting = true;

        if (g_cursor.durIndex >= (int)durs.size()) {
            g_cursor.durIndex = 0;
            g_cursor.shiftIndex++;
        }
        if (g_cursor.shiftIndex >= (int)shifts.size()) {
            // exhausted insertion variants; backtrackDepth increases (but none exist)
            g_cursor.shiftIndex = 0;
            g_cursor.durIndex = 0;
            return false;
        }

        int baseStart = std::max(0, bestDeathFrame - g_set.insertOffset);
        int start = baseStart + shifts[g_cursor.shiftIndex];
        int dur = durs[g_cursor.durIndex];
        g_cursor.durIndex++;

        Segment inserted{ start, start + dur };

        outSegs = bestSegs;
        outSegs.push_back(inserted);
        normalizeSegments(outSegs, g_set.maxFrames);
        return true;
    }

    // Modify an existing segment
    g_cursor.inserting = false;

    // Delete after exhausting (shift,dur) combos
    if (g_cursor.tryDelete) {
        outSegs = bestSegs;
        outSegs.erase(outSegs.begin() + segIndex);
        normalizeSegments(outSegs, g_set.maxFrames);

        // advance cursor to next backtrack target
        g_cursor.tryDelete = false;
        g_cursor.shiftIndex = 0;
        g_cursor.durIndex = 0;
        g_cursor.backtrackDepth++;

        return true;
    }

    // enumerate shift/duration combos
    if (g_cursor.durIndex >= (int)durs.size()) {
        g_cursor.durIndex = 0;
        g_cursor.shiftIndex++;
    }
    if (g_cursor.shiftIndex >= (int)shifts.size()) {
        // finished all variants -> next, try delete
        g_cursor.shiftIndex = 0;
        g_cursor.durIndex = 0;
        g_cursor.tryDelete = true;
        return nextCandidate(outSegs, bestSegs, bestDeathFrame);
    }

    int shift = shifts[g_cursor.shiftIndex];
    int dur = durs[g_cursor.durIndex];
    g_cursor.durIndex++;

    outSegs = bestSegs;

    Segment s = outSegs[segIndex];
    int len = std::max(1, dur);
    int newStart = s.start + shift;
    int newEnd = newStart + len;

    // enforce lock
    if (newStart < lockFrame) newStart = lockFrame;
    newEnd = newStart + len;

    outSegs[segIndex] = { newStart, newEnd };
    normalizeSegments(outSegs, g_set.maxFrames);
    return true;
}

// ============================================================
// Best + current plan
// ============================================================

static std::vector<Segment> g_bestSegs;
static std::vector<Segment> g_curSegs;

// segment scanning state
static size_t g_segIdx = 0;

// ============================================================
// Attempt lifecycle
// ============================================================

static void beginTraining() {
    g_mode = RunMode::Training;
    g_bestSegs.clear();
    g_bestModeTL.clear();

    g_bestX = 0.f;
    g_bestDeathFrame = 0;
    g_hasWin = false;

    g_curSegs.clear();
    g_cursor.reset();
}

static void beginVerify() {
    g_mode = RunMode::Verify;
    g_curSegs = g_bestSegs;
    normalizeSegments(g_curSegs, g_set.maxFrames);
}

// Called at end of an attempt
static void finishAttempt(float xReached, int deathFrame, bool completed) {
    if (completed) {
        g_bestX = xReached;
        g_bestDeathFrame = deathFrame;
        g_bestSegs = g_curSegs;
        g_bestModeTL = g_attemptModeTL;
        g_hasWin = true;
        return;
    }

    if (xReached > g_bestX) {
        g_bestX = xReached;
        g_bestDeathFrame = deathFrame;
        g_bestSegs = g_curSegs;
        g_bestModeTL = g_attemptModeTL;

        // reset backtracking when best improves
        g_cursor.reset();
    }
}

// ============================================================
// Hooks
// ============================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_set.load();
        g_mode = RunMode::Idle;

        g_isHolding = false;
        g_prevDead = false;
        g_requestReset = false;

        g_frame = 0;
        g_segIdx = 0;

        g_attemptModeTL.clear();
        g_attemptModeTL.shrink_to_fit(); // keep memory reasonable

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // release button
        if (auto* gj = GJBaseGameLayer::get()) {
            gj->handleButton(false, 1, true);
        }
        g_isHolding = false;

        g_prevDead = false;
        g_requestReset = false;
        g_frame = 0;
        g_segIdx = 0;

        g_attemptModeTL.clear();
        g_attemptModeTL.reserve(20000);

        g_set.load();

        if (g_mode == RunMode::Training) {
            // Build next candidate based on best
            if (g_bestDeathFrame <= 0) {
                // If no best yet, we’ll learn it from the first death
                g_curSegs.clear();
            } else {
                std::vector<Segment> cand;
                bool ok = nextCandidate(cand, g_bestSegs, g_bestDeathFrame);
                if (!ok) {
                    // If exhausted, widen by backtracking more
                    g_cursor.reset();
                    g_cursor.backtrackDepth++;
                    // last resort: insert near death
                    nextCandidate(cand, g_bestSegs, g_bestDeathFrame);
                }
                g_curSegs = cand;
            }
            normalizeSegments(g_curSegs, g_set.maxFrames);
        } else if (g_mode == RunMode::Verify) {
            g_curSegs = g_bestSegs;
            normalizeSegments(g_curSegs, g_set.maxFrames);
        }
    }

    void onQuit() {
        // release input on quit
        if (auto* gj = GJBaseGameLayer::get()) {
            gj->handleButton(false, 1, true);
        }
        g_isHolding = false;
        g_mode = RunMode::Idle;
        PlayLayer::onQuit();
    }
};

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        g_set.load();

        // If disabled: normal update only
        if (!g_set.enabled) {
            if (g_isHolding) applyHold(this, false);
            GJBaseGameLayer::update(dt);
            return;
        }

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) {
            GJBaseGameLayer::update(dt);
            return;
        }

        if (pl->m_isPaused) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // start training when entering gameplay
        if (g_mode == RunMode::Idle) {
            beginTraining();
            g_requestReset = true; // restart cleanly with candidate logic
        }

        // fast-forward deterministically: multiple ORIGINAL updates per render frame
        int targetSteps = (g_mode == RunMode::Training) ? g_set.updatesPerFrame : 1;

        auto t0 = std::chrono::high_resolution_clock::now();

        for (int step = 0; step < targetSteps; step++) {
            if (g_requestReset) break;
            if (pl->m_hasCompletedLevel) break;
            if (m_player1->m_isDead) break;

            // apply input for this internal frame
            bool desiredHold = holdAtFrame(g_curSegs, g_frame, g_segIdx);
            applyHold(this, desiredHold);

            // record current mode for “analyzed attempts”
            g_attemptModeTL.push_back(readMode(m_player1));

            // execute real engine tick (real hitboxes)
            GJBaseGameLayer::update(dt);

            g_frame++;

            // failsafe
            if (g_frame >= g_set.maxFrames) {
                float x = m_player1->getPositionX();
                finishAttempt(x, g_frame, false);
                applyHold(this, false);
                g_requestReset = true;
                break;
            }

            // time budget
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            if (ms >= g_set.innerBudgetMs) break;
        }

        // detect death edge
        bool deadNow = m_player1->m_isDead;
        bool completeNow = pl->m_hasCompletedLevel;
        float xNow = m_player1->getPositionX();

        if (!g_prevDead && deadNow) {
            finishAttempt(xNow, g_frame, false);
            applyHold(this, false);
            g_requestReset = true;
        }
        g_prevDead = deadNow;

        if (completeNow) {
            finishAttempt(xNow, g_frame, true);
            applyHold(this, false);

            if (g_mode == RunMode::Training && g_set.verifyAfterWin) {
                beginVerify();
                g_requestReset = true;
            } else {
                // stop after win if no verify
                Mod::get()->setSettingValue("enabled", false);
            }
        }

        // status log
        if (g_set.logStatus) {
            g_logCounter++;
            if (g_logCounter % 240 == 0) {
                log::info("Mode={} frame={} x={:.1f} bestX={:.1f} bestDeathFrame={} segsBest={} segsCur={}",
                    (int)g_mode, g_frame, xNow, g_bestX, g_bestDeathFrame,
                    g_bestSegs.size(), g_curSegs.size());
            }
        }

        // reset safely once per render frame
        if (g_requestReset) {
            g_requestReset = false;
            pl->resetLevel();
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
    log::info("GD AutoBot Backtracker loaded. F8 toggles enabled. Use pause gear for settings.");
}

