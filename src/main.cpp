#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <random>
#include <algorithm>
#include <chrono>
#include <cstdint>

using namespace geode::prelude;

// ============================================================
// Settings
// ============================================================

struct Settings {
    bool enabled = false;

    int updatesPerFrame = 30;     // fast-forward factor
    int innerBudgetMs = 8;        // time budget per rendered frame

    int maxFrames = 300000;

    int mutationWindow = 240;
    int prefixLockMargin = 30;

    int mutationOps = 40;
    int tapLength = 2;

    float seedTapRate = 0.002f;

    bool verifyAfterWin = true;
    bool logStatus = true;

    void load() {
        auto* m = Mod::get();
        enabled = m->getSettingValue<bool>("enabled");

        updatesPerFrame = (int)m->getSettingValue<int64_t>("updates-per-frame");
        innerBudgetMs = (int)m->getSettingValue<int64_t>("inner-time-budget-ms");

        maxFrames = (int)m->getSettingValue<int64_t>("max-frames");

        mutationWindow = (int)m->getSettingValue<int64_t>("mutation-window");
        prefixLockMargin = (int)m->getSettingValue<int64_t>("prefix-lock-margin");

        mutationOps = (int)m->getSettingValue<int64_t>("mutation-ops");
        tapLength = (int)m->getSettingValue<int64_t>("tap-length");

        seedTapRate = (float)m->getSettingValue<double>("seed-tap-rate");

        verifyAfterWin = m->getSettingValue<bool>("verify-after-win");
        logStatus = m->getSettingValue<bool>("log-status");

        updatesPerFrame = std::clamp(updatesPerFrame, 1, 300);
        innerBudgetMs = std::clamp(innerBudgetMs, 1, 30);

        maxFrames = std::clamp(maxFrames, 10000, 1000000);

        mutationWindow = std::clamp(mutationWindow, 30, 5000);
        prefixLockMargin = std::clamp(prefixLockMargin, 0, 2000);

        mutationOps = std::clamp(mutationOps, 1, 500);
        tapLength = std::clamp(tapLength, 1, 20);

        seedTapRate = std::clamp(seedTapRate, 0.0001f, 0.05f);
    }
};

static Settings g_set;

// ============================================================
// Deterministic fast-forward trainer state
// ============================================================

enum class RunMode {
    Idle,
    Training,
    Verify
};

static RunMode g_runMode = RunMode::Idle;

static bool g_isHolding = false;
static bool g_prevDead = false;

static int  g_frameInAttempt = 0;
static int  g_generation = 0;

static float g_bestX = 0.f;
static int   g_bestDeathFrame = 0;
static bool  g_hasWin = false;

// request reset safely (don’t reset mid-inner-loop)
static bool g_requestReset = false;

// for status logging
static int g_logCounter = 0;

// RNG
static std::mt19937 g_rng{ std::random_device{}() };

// ============================================================
// Genome: hold state per frame (0/1)
// ============================================================

struct Genome {
    std::vector<uint8_t> hold;
    float fitnessX = 0.f;
    int deathFrame = 0;
    bool completed = false;
};

static Genome g_best;
static Genome g_cur;

// ============================================================
// Helpers
// ============================================================

static void applyHold(GJBaseGameLayer* gl, bool hold) {
    if (!gl) return;
    if (hold == g_isHolding) return;
    gl->handleButton(hold, 1, true);
    g_isHolding = hold;
}

static void ensureSize(Genome& g, int n) {
    if ((int)g.hold.size() < n) g.hold.resize((size_t)n, 0);
    if ((int)g.hold.size() > n) g.hold.resize((size_t)n);
}

static bool holdAt(const Genome& g, int f) {
    if (f < 0) return false;
    if ((size_t)f >= g.hold.size()) return false;
    return g.hold[(size_t)f] != 0;
}

static void seedGenome(Genome& g) {
    g.hold.assign((size_t)g_set.maxFrames, 0);

    std::uniform_real_distribution<float> u01(0.f, 1.f);

    for (int i = 0; i < g_set.maxFrames; i++) {
        if (u01(g_rng) < g_set.seedTapRate) {
            for (int k = 0; k < g_set.tapLength && (i + k) < g_set.maxFrames; k++) {
                g.hold[(size_t)(i + k)] = 1;
            }
            i += g_set.tapLength;
        }
    }
}

static void copyBestToCur() {
    g_cur = g_best;
    ensureSize(g_cur, g_set.maxFrames);
}

static void mutateFromBest(Genome& out) {
    out = g_best;
    ensureSize(out, g_set.maxFrames);

    int center = (g_bestDeathFrame > 0) ? g_bestDeathFrame : (g_set.maxFrames / 4);
    int start = std::max(0, center - g_set.mutationWindow);
    int end   = std::min(g_set.maxFrames - 1, center + g_set.mutationWindow);

    // PREFIX LOCK: do not change anything before lockedPrefixEnd
    int lockedPrefixEnd = std::max(0, (g_bestDeathFrame - g_set.mutationWindow) - g_set.prefixLockMargin);
    start = std::max(start, lockedPrefixEnd);

    if (start >= end) {
        // nothing to mutate safely, fallback small window at end
        start = std::max(0, center - 30);
        end = std::min(g_set.maxFrames - 1, center + 30);
        start = std::max(start, lockedPrefixEnd);
    }

    std::uniform_int_distribution<int> pick(start, end);
    std::uniform_int_distribution<int> opPick(0, 1);

    for (int op = 0; op < g_set.mutationOps; op++) {
        int f = pick(g_rng);
        int which = opPick(g_rng);

        if (which == 0) {
            // insert tap
            for (int k = 0; k < g_set.tapLength && f + k < g_set.maxFrames; k++) {
                out.hold[(size_t)(f + k)] = 1;
            }
        } else {
            // remove tap
            for (int k = 0; k < g_set.tapLength && f + k < g_set.maxFrames; k++) {
                out.hold[(size_t)(f + k)] = 0;
            }
        }
    }
}

static void startTraining() {
    g_runMode = RunMode::Training;
    g_generation = 0;

    g_best = Genome{};
    g_cur = Genome{};
    g_bestX = 0.f;
    g_bestDeathFrame = 0;
    g_hasWin = false;

    seedGenome(g_cur);
}

static void startVerify() {
    g_runMode = RunMode::Verify;
    copyBestToCur();
}

// Called when an attempt ends (death or completion)
static void finishAttempt(float xReached, int deathFrame, bool completed) {
    g_cur.fitnessX = xReached;
    g_cur.deathFrame = deathFrame;
    g_cur.completed = completed;

    if (completed) {
        g_best = g_cur;
        g_bestX = xReached;
        g_bestDeathFrame = deathFrame;
        g_hasWin = true;
        return;
    }

    if (xReached > g_bestX) {
        g_best = g_cur;
        g_bestX = xReached;
        g_bestDeathFrame = deathFrame;
    }
}

// ============================================================
// Hooks
// ============================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bool ok = PlayLayer::init(level, useReplay, dontCreateObjects);
        if (!ok) return false;

        g_set.load();
        g_runMode = RunMode::Idle;

        g_prevDead = false;
        g_isHolding = false;
        g_frameInAttempt = 0;
        g_requestReset = false;
        g_logCounter = 0;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // clear hold safely
        if (auto* gj = GJBaseGameLayer::get()) {
            gj->handleButton(false, 1, true);
        }
        g_isHolding = false;
        g_prevDead = false;
        g_frameInAttempt = 0;
        g_requestReset = false;

        g_set.load();

        // Prepare next attempt genome:
        if (g_runMode == RunMode::Training) {
            if (g_best.hold.empty()) {
                seedGenome(g_cur);
            } else {
                mutateFromBest(g_cur);
            }
            g_generation++;
        } else if (g_runMode == RunMode::Verify) {
            copyBestToCur();
        }
    }

    void onQuit() {
        // stop controlling input on quit
        if (auto* gj = GJBaseGameLayer::get()) {
            gj->handleButton(false, 1, true);
        }
        g_isHolding = false;
        g_runMode = RunMode::Idle;
        PlayLayer::onQuit();
    }
};

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        // If bot is disabled, run normal behavior
        g_set.load();
        if (!g_set.enabled || !PlayLayer::get()) {
            GJBaseGameLayer::update(dt);
            return;
        }

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // If paused, run normal update once and do nothing
        if (pl->m_isPaused) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // Start training when entering a level
        if (g_runMode == RunMode::Idle) {
            startTraining();
            // start first attempt fresh
            g_requestReset = true;
        }

        // Fast-forward loop (deterministic): call ORIGINAL update multiple times with same dt
        auto t0 = std::chrono::high_resolution_clock::now();

        int stepsThisFrame = (g_runMode == RunMode::Training) ? g_set.updatesPerFrame : 1;

        for (int s = 0; s < stepsThisFrame; s++) {
            // If we requested a reset, do it safely outside this loop
            if (g_requestReset) break;

            // stop if completed
            if (pl->m_hasCompletedLevel) break;

            // stop if dead (wait for reset)
            if (m_player1->m_isDead) break;

            // Apply genome input for this internal frame
            bool desiredHold = holdAt(g_cur, g_frameInAttempt);
            applyHold(this, desiredHold);

            // Run one real engine tick
            GJBaseGameLayer::update(dt);

            g_frameInAttempt++;

            // failsafe max frames
            if (g_frameInAttempt >= g_set.maxFrames) {
                // treat as failure
                float x = m_player1 ? m_player1->getPositionX() : 0.f;
                finishAttempt(x, g_frameInAttempt, false);
                applyHold(this, false);
                g_requestReset = true;
                break;
            }

            // time budget
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            if (ms >= g_set.innerBudgetMs) break;
        }

        // Handle completion / death / reset requests
        bool deadNow = m_player1->m_isDead;
        bool completedNow = pl->m_hasCompletedLevel;

        float curX = m_player1 ? m_player1->getPositionX() : 0.f;

        if (!g_prevDead && deadNow) {
            finishAttempt(curX, g_frameInAttempt, false);
            applyHold(this, false);
            g_requestReset = true;
        }
        g_prevDead = deadNow;

        if (completedNow) {
            finishAttempt(curX, g_frameInAttempt, true);
            applyHold(this, false);

            if (g_runMode == RunMode::Training && g_set.verifyAfterWin) {
                // switch to verify and replay best at normal speed
                startVerify();
                g_requestReset = true;
            } else {
                // stop controlling after success
                Mod::get()->setSettingValue("enabled", false);
            }
        }

        // status logs
        if (g_set.logStatus) {
            g_logCounter++;
            if (g_logCounter % 240 == 0) {
                log::info("Mode={} gen={} frame={} x={:.1f} bestX={:.1f} bestDeathFrame={}",
                    (int)g_runMode, g_generation, g_frameInAttempt, curX, g_bestX, g_bestDeathFrame);
            }
        }

        // Perform reset safely once per outer frame
        if (g_requestReset) {
            g_requestReset = false;
            pl->resetLevel();
        }
    }
};

// Pause menu: toggle + settings button
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

// F8 toggles enabled
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
    log::info("GD AutoBot Trainer (Deterministic) loaded. F8 toggles enabled.");
}
