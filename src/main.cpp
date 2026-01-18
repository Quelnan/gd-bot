#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <random>
#include <algorithm>
#include <cstdint>
#include <cmath>

using namespace geode::prelude;

// ============================================================
// Helpers: safe access to scheduler timescale
// ============================================================

static float getTimeScale() {
    auto* dir = cocos2d::CCDirector::sharedDirector();
    if (!dir) return 1.f;
    auto* sch = dir->getScheduler();
    if (!sch) return 1.f;
    return sch->getTimeScale();
}

static void setTimeScale(float s) {
    auto* dir = cocos2d::CCDirector::sharedDirector();
    if (!dir) return;
    auto* sch = dir->getScheduler();
    if (!sch) return;
    sch->setTimeScale(s);
}

// ============================================================
// Bot runtime state
// ============================================================

enum class RunState {
    Idle,
    Training,   // fast attempts, mutate
    Verify      // normal speed playback of best
};

static RunState g_state = RunState::Idle;

static bool  g_enabled = false;
static bool  g_isHolding = false;

static int   g_frameInAttempt = 0;
static int   g_resetCountdown = -1;

static float g_bestX = 0.f;
static int   g_bestDeathFrame = 0;
static bool  g_foundCompletion = false;

// remember & restore original timescale (avoid leaving game sped up after quit/crash)
static float g_savedTimeScale = 1.f;
static bool  g_savedTimeScaleValid = false;

// ============================================================
// Genome: per-frame hold input
// ============================================================

struct Genome {
    std::vector<uint8_t> hold; // 0/1
    float fitnessX = 0.f;
    int deathFrame = 0;
    bool completed = false;
};

// Best known and current candidate
static Genome g_best;
static Genome g_cur;

// RNG
static std::mt19937 g_rng{ std::random_device{}() };

// ============================================================
// Settings
// ============================================================

struct Settings {
    bool enabled = false;
    int mode = 1;

    float trainTimeScale = 3.0f;
    int restartDelay = 10;

    int maxFrames = 300000;

    int mutationWindow = 240;
    float mutationRate = 0.02f;

    float seedTapRate = 0.003f;
    int tapLen = 2;

    bool logStatus = true;

    void load() {
        auto* m = Mod::get();
        enabled = m->getSettingValue<bool>("enabled");
        mode = (int)m->getSettingValue<int64_t>("mode");

        trainTimeScale = (float)m->getSettingValue<double>("train-timescale");
        restartDelay = (int)m->getSettingValue<int64_t>("restart-delay-frames");

        maxFrames = (int)m->getSettingValue<int64_t>("max-frames");

        mutationWindow = (int)m->getSettingValue<int64_t>("mutation-window");
        mutationRate = (float)m->getSettingValue<double>("mutation-rate");

        seedTapRate = (float)m->getSettingValue<double>("seed-tap-rate");
        tapLen = (int)m->getSettingValue<int64_t>("tap-length-frames");

        logStatus = m->getSettingValue<bool>("log-status");

        trainTimeScale = std::clamp(trainTimeScale, 1.0f, 10.0f);
        restartDelay = std::clamp(restartDelay, 0, 120);

        maxFrames = std::clamp(maxFrames, 10000, 1000000);

        mutationWindow = std::clamp(mutationWindow, 30, 2000);
        mutationRate = std::clamp(mutationRate, 0.0f, 0.5f);

        seedTapRate = std::clamp(seedTapRate, 0.0005f, 0.05f);
        tapLen = std::clamp(tapLen, 1, 20);

        mode = std::clamp(mode, 0, 1);
    }
};

static Settings g_set;

// ============================================================
// Input application (only toggle when changed)
// ============================================================

static void applyHold(GJBaseGameLayer* gl, bool hold) {
    if (!gl) return;
    if (hold == g_isHolding) return;
    gl->handleButton(hold, 1, true);
    g_isHolding = hold;
}

// ============================================================
// Genome generation / mutation
// ============================================================

static void ensureGenomeSize(Genome& g, int minSize) {
    if ((int)g.hold.size() < minSize) g.hold.resize((size_t)minSize, 0);
}

static void seedRandomGenome(Genome& g, int maxFrames) {
    g.hold.assign((size_t)maxFrames, 0);

    std::uniform_real_distribution<float> u01(0.f, 1.f);

    // Random taps: for each frame, with probability seedTapRate, make a small tap
    for (int i = 0; i < maxFrames; i++) {
        if (u01(g_rng) < g_set.seedTapRate) {
            for (int k = 0; k < g_set.tapLen && i + k < maxFrames; k++) {
                g.hold[(size_t)(i + k)] = 1;
            }
            i += g_set.tapLen;
        }
    }
}

static void mutateFromBest(Genome& out, const Genome& best, int maxFrames) {
    out = best;
    ensureGenomeSize(out, maxFrames);

    // keep size exactly maxFrames
    if ((int)out.hold.size() > maxFrames) out.hold.resize((size_t)maxFrames);

    std::uniform_real_distribution<float> u01(0.f, 1.f);
    std::uniform_int_distribution<int> w(-g_set.mutationWindow, g_set.mutationWindow);

    // Focus mutation around death frame (if known)
    int center = best.deathFrame > 0 ? best.deathFrame : (maxFrames / 4);
    int start = std::max(0, center - g_set.mutationWindow);
    int end   = std::min(maxFrames - 1, center + g_set.mutationWindow);

    for (int i = start; i <= end; i++) {
        if (u01(g_rng) < g_set.mutationRate) {
            out.hold[(size_t)i] ^= 1;
        }
    }

    // Add a few random taps near center to explore alternatives
    for (int t = 0; t < 12; t++) {
        int idx = std::clamp(center + w(g_rng), 0, maxFrames - 1);
        for (int k = 0; k < g_set.tapLen && idx + k < maxFrames; k++) {
            out.hold[(size_t)(idx + k)] = 1;
        }
    }
}

static bool holdAtFrame(const Genome& g, int frame) {
    if (frame < 0) return false;
    if ((size_t)frame >= g.hold.size()) return false;
    return g.hold[(size_t)frame] != 0;
}

// ============================================================
// Training control
// ============================================================

static void beginTraining() {
    if (!g_savedTimeScaleValid) {
        g_savedTimeScale = getTimeScale();
        g_savedTimeScaleValid = true;
    }
    setTimeScale(g_set.trainTimeScale);
    g_state = RunState::Training;
    g_foundCompletion = false;
}

static void beginVerifyPlayback() {
    if (!g_savedTimeScaleValid) {
        g_savedTimeScale = getTimeScale();
        g_savedTimeScaleValid = true;
    }
    setTimeScale(1.0f);
    g_state = RunState::Verify;
}

static void stopAll() {
    setTimeScale(g_savedTimeScaleValid ? g_savedTimeScale : 1.0f);
    g_state = RunState::Idle;
    g_resetCountdown = -1;
    g_frameInAttempt = 0;
    g_isHolding = false;
    g_savedTimeScaleValid = false;
}

// Called after death to schedule reset
static void scheduleReset() {
    g_resetCountdown = g_set.restartDelay;
}

// ============================================================
// Evaluate attempt end (death or completion)
// ============================================================

static void onAttemptEnded(float xReached, int deathFrame, bool completed) {
    g_cur.fitnessX = xReached;
    g_cur.deathFrame = deathFrame;
    g_cur.completed = completed;

    // keep best
    if (completed) {
        g_best = g_cur;
        g_bestX = xReached;
        g_bestDeathFrame = deathFrame;
        g_foundCompletion = true;
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
        g_enabled = g_set.enabled;

        // Reset run state for each entry
        g_state = RunState::Idle;
        g_best = Genome{};
        g_cur = Genome{};
        g_bestX = 0.f;
        g_bestDeathFrame = 0;
        g_foundCompletion = false;

        g_frameInAttempt = 0;
        g_resetCountdown = -1;
        g_isHolding = false;
        g_prevDead = false;

        // Save timescale now (safe restore on quit)
        g_savedTimeScale = getTimeScale();
        g_savedTimeScaleValid = true;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // release input
        if (auto* gj = GJBaseGameLayer::get()) {
            gj->handleButton(false, 1, true);
        }
        g_isHolding = false;

        g_frameInAttempt = 0;
        g_resetCountdown = -1;

        // prepare next candidate genome when attempt restarts
        g_set.load();
        if (g_state == RunState::Training) {
            if (g_best.hold.empty()) {
                seedRandomGenome(g_cur, g_set.maxFrames);
            } else {
                mutateFromBest(g_cur, g_best, g_set.maxFrames);
            }
        }
        else if (g_state == RunState::Verify) {
            g_cur = g_best;
            ensureGenomeSize(g_cur, g_set.maxFrames);
        }
    }

    void onQuit() {
        // restore timescale always
        stopAll();
        PlayLayer::onQuit();
    }
};

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);

        g_set.load();
        g_enabled = g_set.enabled;

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) return;

        // Only operate in actual runs
        if (pl->m_isPaused) return;

        // If disabled: ensure timescale restored, release input
        if (!g_enabled || g_set.mode == 0) {
            if (g_state != RunState::Idle) stopAll();
            if (g_isHolding) applyHold(this, false);
            return;
        }

        // If just entered playing state, start training
        if (g_state == RunState::Idle) {
            beginTraining();
            // prepare first genome
            seedRandomGenome(g_cur, g_set.maxFrames);
        }

        // Read live engine progress (engine hitboxes determine death, so no need to simulate hitboxes)
        float x = m_player1->getPositionX();
        bool dead = m_player1->m_isDead;
        bool complete = pl->m_hasCompletedLevel;

        // Periodic status
        if (g_set.logStatus && (g_frameCounter++ % 240 == 0)) {
            log::info("TrainState={} x={:.1f} bestX={:.1f} frame={} timeScale={:.2f}",
                (int)g_state, x, g_bestX, g_frameInAttempt, getTimeScale());
        }

        // Handle completion
        if (complete) {
            onAttemptEnded(x, g_frameInAttempt, true);

            if (g_state == RunState::Training) {
                // switch to verify playback at normal speed
                beginVerifyPlayback();
                // replay best
                g_cur = g_best;
                g_frameInAttempt = 0;
                applyHold(this, false);
                // restart level to replay cleanly
                scheduleReset();
            } else if (g_state == RunState::Verify) {
                // success: stop speedhack, keep enabled but stop changing input
                setTimeScale(1.0f);
                // You can leave it in Verify (it already won) or set Idle
                g_state = RunState::Idle;
            }
            return;
        }

        // Handle death: evaluate attempt, schedule reset
        if (dead) {
            onAttemptEnded(x, g_frameInAttempt, false);
            applyHold(this, false);

            // schedule reset (don’t reset instantly inside death frame)
            if (g_resetCountdown < 0) scheduleReset();
        }

        // Countdown reset
        if (g_resetCountdown >= 0) {
            g_resetCountdown--;
            if (g_resetCountdown == 0) {
                // reset attempt
                pl->resetLevel();
            }
            return; // don't apply input while waiting to reset
        }

        // Apply genome input for this frame
        bool desiredHold = holdAtFrame(g_cur, g_frameInAttempt);
        applyHold(this, desiredHold);

        g_frameInAttempt++;

        // Safety: stop runaway attempts
        if (g_frameInAttempt >= g_set.maxFrames) {
            onAttemptEnded(x, g_frameInAttempt, false);
            applyHold(this, false);
            scheduleReset();
        }
    }
};

// Pause menu UI: toggle + settings button
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
    log::info("GD AutoBot Trainer loaded. F8 toggles enabled.");
}
