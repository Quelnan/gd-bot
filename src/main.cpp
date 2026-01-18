#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <set>
#include <map>
#include <deque>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using namespace geode::prelude;

// ============================================================
// Global state
// ============================================================

static bool g_botEnabled = false;
static bool g_levelAnalyzed = false;
static bool g_isHolding = false;
static int  g_frameCounter = 0;

static float g_playerX = 0.f;
static float g_playerY = 0.f;
static float g_playerYVel = 0.f;
static bool  g_playerOnGround = false;
static bool  g_isUpsideDown = false;
static bool  g_isMini = false;
static float g_xSpeed = 311.58f / 240.f;

enum class BotGameMode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };
static BotGameMode g_gameMode = BotGameMode::Cube;

// ============================================================
// Settings
// ============================================================

struct BotSettings {
    int simFrames = 180;
    int stepFrames = 3;
    int beamWidth = 64;
    int timeBudgetMs = 4;
    int replanEvery = 6;
    int tapFrames = 2;

    float hitboxScale = 0.9f;
    float hazardHitboxScale = 1.0f;
    float safeMargin = 2.0f;
    float nearScanRange = 600.0f;

    bool debugOverlay = true;
    bool debugDraw = false;

    void load() {
        auto* m = Mod::get();
        simFrames = (int)m->getSettingValue<int64_t>("sim-frames");
        stepFrames = (int)m->getSettingValue<int64_t>("step-frames");
        beamWidth = (int)m->getSettingValue<int64_t>("beam-width");
        timeBudgetMs = (int)m->getSettingValue<int64_t>("time-budget-ms");
        replanEvery = (int)m->getSettingValue<int64_t>("replan-every");
        tapFrames = (int)m->getSettingValue<int64_t>("hold-duration");

        hitboxScale = (float)m->getSettingValue<double>("hitbox-scale");
        hazardHitboxScale = (float)m->getSettingValue<double>("hazard-hitbox-scale");
        safeMargin = (float)m->getSettingValue<double>("safe-margin");
        nearScanRange = (float)m->getSettingValue<double>("near-scan-range");

        debugOverlay = m->getSettingValue<bool>("debug-overlay");
        debugDraw = m->getSettingValue<bool>("debug-draw");

        simFrames = std::clamp(simFrames, 30, 720);
        stepFrames = std::clamp(stepFrames, 1, 10);
        beamWidth = std::clamp(beamWidth, 8, 256);
        timeBudgetMs = std::clamp(timeBudgetMs, 1, 12);
        replanEvery = std::clamp(replanEvery, 1, 30);
        tapFrames = std::clamp(tapFrames, 1, 10);
    }
};

static BotSettings g_settings;

// ============================================================
// IDs (extend SOLID_IDS as you encounter custom blocks)
// ============================================================

static const std::set<int> HAZARD_IDS = {
    8, 9, 39, 61, 103, 392,
    243,244,245,246,247,248,249,
    363,364,365,366,367,368,
    446,447,
    678,679,680,
    1705,1706,1707,1708,1709,1710,
    1711,1712,1713,1714,1715,1716,1717,1718,
    88,89,98,397,398,399,740,741,742
};

// IMPORTANT: This is incomplete. Add more IDs as needed.
// This list is intentionally conservative to avoid treating decoration as solid.
static const std::set<int> SOLID_IDS = []{
    std::set<int> s;
    // Many common solids are in 1..34 (classic block set).
    for (int id = 1; id <= 34; ++id) s.insert(id);
    // Common extra blocks (you can extend)
    for (int id = 40; id <= 48; ++id) s.insert(id);
    return s;
}();

static const std::map<int, int> ORB_IDS = {
    {36, 0}, {84, 1}, {141, 2}, {1022, 3}, {1330, 4}, {1333, 5}, {1704, 6}, {1751, 7}
};

static const std::map<int, int> PAD_IDS = {
    {35, 0}, {67, 1}, {140, 2}, {1332, 3}, {452, 4}
};

static const std::map<int, BotGameMode> PORTAL_IDS = {
    {12, BotGameMode::Cube}, {13, BotGameMode::Ship}, {47, BotGameMode::Ball},
    {111, BotGameMode::UFO}, {660, BotGameMode::Wave}, {745, BotGameMode::Robot},
    {1331, BotGameMode::Spider}, {1933, BotGameMode::Swing}
};

// ============================================================
// Physics constants
// ============================================================

namespace Phys {
    constexpr float GROUND = 105.0f;
    constexpr float CEILING = 2085.0f;

    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP = 11.180032f;
    constexpr float JUMP_MINI = 9.4f;

    constexpr float SHIP_ACCEL = 0.8f;
    constexpr float SHIP_MAX = 8.0f;

    constexpr float UFO_BOOST = 7.0f;
    constexpr float BALL_VEL = 6.0f;

    constexpr float MAX_VEL = 15.0f;

    constexpr float PLAYER_SIZE = 30.0f;
    constexpr float MINI_SCALE = 0.6f;
    constexpr float WAVE_SCALE = 0.5f;

    constexpr float SPEED_SLOW = 251.16f;
    constexpr float SPEED_NORMAL = 311.58f;
    constexpr float SPEED_FAST = 387.42f;
    constexpr float SPEED_FASTER = 468.0f;
    constexpr float SPEED_FASTEST = 576.0f;
}

// ============================================================
// Level objects
// ============================================================

struct LevelObj {
    int id = 0;
    float x = 0, y = 0;
    float w = 0, h = 0;

    bool isHazard = false;
    bool isSolid = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;

    int subType = 0; // orb/pad
    BotGameMode portalMode = BotGameMode::Cube;

    bool gravityPortal = false;
    bool gravityUp = false;

    bool sizePortal = false;
    bool sizeMini = false;

    bool speedPortal = false;
    float speedVal = 1.0f;
};

static std::vector<LevelObj>   g_objects;
static std::vector<LevelObj*>  g_hazards;
static std::vector<LevelObj*>  g_solids;
static std::vector<LevelObj*>  g_orbs;
static std::vector<LevelObj*>  g_pads;
static std::vector<LevelObj*>  g_portals;

// ============================================================
// Utility
// ============================================================

static inline bool isPlatformMode(BotGameMode m) {
    return m == BotGameMode::Cube || m == BotGameMode::Ball || m == BotGameMode::Robot || m == BotGameMode::Spider;
}

static float getPlayerSizeFor(bool mini, BotGameMode mode) {
    float sz = Phys::PLAYER_SIZE;
    if (mini) sz *= Phys::MINI_SCALE;
    if (mode == BotGameMode::Wave) sz *= Phys::WAVE_SCALE;
    return sz * g_settings.hitboxScale;
}

static bool aabbOverlap(float px, float py, float psz, const LevelObj& o, float extraMargin = 0.f) {
    float ph = psz / 2.f + g_settings.safeMargin + extraMargin;
    float ow = (o.w * g_settings.hazardHitboxScale) / 2.f;
    float oh = (o.h * g_settings.hazardHitboxScale) / 2.f;

    return !(px + ph < o.x - ow || px - ph > o.x + ow || py + ph < o.y - oh || py - ph > o.y + oh);
}

// Find range by X in sorted pointer vectors
template <class Vec>
static std::pair<typename Vec::const_iterator, typename Vec::const_iterator>
rangeByX(const Vec& v, float minX, float maxX) {
    auto lb = std::lower_bound(v.begin(), v.end(), minX, [](auto* obj, float x) {
        return obj->x < x;
    });
    auto ub = std::upper_bound(v.begin(), v.end(), maxX, [](float x, auto* obj) {
        return x < obj->x;
    });
    return { lb, ub };
}

// ============================================================
// Level analyzer
// ============================================================

static void analyzeLevel(PlayLayer* pl) {
    g_objects.clear();
    g_hazards.clear();
    g_solids.clear();
    g_orbs.clear();
    g_pads.clear();
    g_portals.clear();
    g_levelAnalyzed = false;

    if (!pl || !pl->m_objects) return;

    int hazardCount = 0, solidCount = 0, orbCount = 0, padCount = 0, portalCount = 0;

    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;

        int id = obj->m_objectID;

        LevelObj lo;
        lo.id = id;
        lo.x = obj->getPositionX();
        lo.y = obj->getPositionY();

        // Use contentSize*scale as collision approximation
        auto sz = obj->getContentSize();
        float sc = obj->getScale();
        lo.w = std::max(5.f, sz.width * sc * 0.9f);
        lo.h = std::max(5.f, sz.height * sc * 0.9f);

        bool important = false;

        if (HAZARD_IDS.count(id)) {
            lo.isHazard = true;
            important = true;
            hazardCount++;
        }
        else if (SOLID_IDS.count(id)) {
            lo.isSolid = true;
            important = true;
            solidCount++;
        }
        else if (auto it = ORB_IDS.find(id); it != ORB_IDS.end()) {
            lo.isOrb = true;
            lo.subType = it->second;
            important = true;
            orbCount++;
        }
        else if (auto it = PAD_IDS.find(id); it != PAD_IDS.end()) {
            lo.isPad = true;
            lo.subType = it->second;
            important = true;
            padCount++;
        }
        else if (auto it = PORTAL_IDS.find(id); it != PORTAL_IDS.end()) {
            lo.isPortal = true;
            lo.portalMode = it->second;
            important = true;
            portalCount++;
        }
        else if (id == 10) { lo.isPortal = true; lo.gravityPortal = true; lo.gravityUp = false; important = true; portalCount++; }
        else if (id == 11) { lo.isPortal = true; lo.gravityPortal = true; lo.gravityUp = true;  important = true; portalCount++; }
        else if (id == 99) { lo.isPortal = true; lo.sizePortal = true; lo.sizeMini = false;   important = true; portalCount++; }
        else if (id == 101){ lo.isPortal = true; lo.sizePortal = true; lo.sizeMini = true;    important = true; portalCount++; }
        else if (id == 200){ lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 0.7f;   important = true; portalCount++; }
        else if (id == 201){ lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 0.9f;   important = true; portalCount++; }
        else if (id == 202){ lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.0f;   important = true; portalCount++; }
        else if (id == 203){ lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.1f;   important = true; portalCount++; }
        else if (id == 1334){lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.3f;   important = true; portalCount++; }

        if (important) g_objects.push_back(lo);
    }

    std::sort(g_objects.begin(), g_objects.end(), [](const LevelObj& a, const LevelObj& b) {
        return a.x < b.x;
    });

    for (auto& o : g_objects) {
        if (o.isHazard) g_hazards.push_back(&o);
        if (o.isSolid)  g_solids.push_back(&o);
        if (o.isOrb)    g_orbs.push_back(&o);
        if (o.isPad)    g_pads.push_back(&o);
        if (o.isPortal) g_portals.push_back(&o);
    }

    log::info("Bot: {} hazards, {} solids, {} orbs, {} pads, {} portals",
        hazardCount, solidCount, orbCount, padCount, portalCount);

    g_levelAnalyzed = true;
}

// ============================================================
// Death checks (hazards + bounds)
// ============================================================

static bool willDieHazards(float x, float y, bool mini, BotGameMode mode) {
    float psz = getPlayerSizeFor(mini, mode);

    float minX = x - 80.f;
    float maxX = x + g_settings.nearScanRange;

    auto [b, e] = rangeByX(g_hazards, minX, maxX);
    for (auto it = b; it != e; ++it) {
        if (aabbOverlap(x, y, psz, **it)) return true;
    }

    // bounds
    if (y < Phys::GROUND - 80.f || y > Phys::CEILING + 80.f) return true;
    return false;
}

// Platform solid collision resolution:
// - landing on top is allowed (sets onGround)
// - side/bottom collision kills
static bool resolveSolidsPlatform(
    float& x, float& y, float& yVel, bool& onGround,
    bool upsideDown, bool mini, BotGameMode mode,
    float prevX, float prevY
) {
    float psz = getPlayerSizeFor(mini, mode);
    float pHalf = psz / 2.f;

    float minX = x - 120.f;
    float maxX = x + 200.f;

    auto [b, e] = rangeByX(g_solids, minX, maxX);
    for (auto it = b; it != e; ++it) {
        const LevelObj& s = **it;
        if (!aabbOverlap(x, y, psz, s)) continue;

        float ow = (s.w * g_settings.hazardHitboxScale) / 2.f;
        float oh = (s.h * g_settings.hazardHitboxScale) / 2.f;

        float solidLeft   = s.x - ow;
        float solidRight  = s.x + ow;
        float solidBottom = s.y - oh;
        float solidTop    = s.y + oh;

        float prevBottom = prevY - pHalf;
        float prevTop    = prevY + pHalf;
        float curBottom  = y - pHalf;
        float curTop     = y + pHalf;

        const float eps = 2.0f;

        if (!upsideDown) {
            // land on top
            if (prevBottom >= solidTop - eps && curBottom < solidTop) {
                y = solidTop + pHalf;
                yVel = 0.f;
                onGround = true;
                continue;
            }
            // hit underside => die
            if (prevTop <= solidBottom + eps && curTop > solidBottom) {
                return false;
            }
        } else {
            // upside-down land on underside
            if (prevTop <= solidBottom + eps && curTop > solidBottom) {
                y = solidBottom - pHalf;
                yVel = 0.f;
                onGround = true;
                continue;
            }
            // hit "top" in upside-down => die
            if (prevBottom >= solidTop - eps && curBottom < solidTop) {
                return false;
            }
        }

        // side collision => die
        return false;
    }

    return true;
}

static bool willDieOnSolidsNonPlatform(float x, float y, bool mini, BotGameMode mode) {
    float psz = getPlayerSizeFor(mini, mode);

    float minX = x - 120.f;
    float maxX = x + 200.f;

    auto [b, e] = rangeByX(g_solids, minX, maxX);
    for (auto it = b; it != e; ++it) {
        if (aabbOverlap(x, y, psz, **it)) return true;
    }
    return false;
}

// ============================================================
// Orbs/Pads/Portals detection (AABB)
// ============================================================

static LevelObj* findOrb(float x, float y, bool mini, BotGameMode mode) {
    float psz = getPlayerSizeFor(mini, mode) * 1.5f;
    float minX = x - 80.f;
    float maxX = x + 80.f;
    auto [b, e] = rangeByX(g_orbs, minX, maxX);
    for (auto it = b; it != e; ++it) {
        if (aabbOverlap(x, y, psz, **it, 0.f)) return *it;
    }
    return nullptr;
}

static LevelObj* findPad(float x, float y, bool mini, BotGameMode mode) {
    float psz = getPlayerSizeFor(mini, mode);
    float minX = x - 80.f;
    float maxX = x + 80.f;
    auto [b, e] = rangeByX(g_pads, minX, maxX);
    for (auto it = b; it != e; ++it) {
        if (aabbOverlap(x, y, psz, **it, 0.f)) return *it;
    }
    return nullptr;
}

static LevelObj* findPortal(float x, float y, bool mini, BotGameMode mode) {
    float psz = getPlayerSizeFor(mini, mode) * 2.0f;
    float minX = x - 100.f;
    float maxX = x + 100.f;
    auto [b, e] = rangeByX(g_portals, minX, maxX);
    for (auto it = b; it != e; ++it) {
        if (aabbOverlap(x, y, psz, **it, 0.f)) return *it;
    }
    return nullptr;
}

// ============================================================
// Simulation state
// ============================================================

struct SimState {
    float x = 0, y = 0;
    float yVel = 0;
    float xSpeed = 0;
    float prevX = 0, prevY = 0;

    bool onGround = false;
    bool canJump = true;
    bool upsideDown = false;
    bool mini = false;

    BotGameMode mode = BotGameMode::Cube;

    float orbCD = 0.f;
    int lastOrb = -1;

    bool dead = false;
};

// Apply interactions in sim
static void simInteract(SimState& s, bool hold) {
    // portals
    if (auto* p = findPortal(s.x, s.y, s.mini, s.mode)) {
        if (p->gravityPortal) {
            s.upsideDown = p->gravityUp;
        } else if (p->sizePortal) {
            s.mini = p->sizeMini;
        } else if (p->speedPortal) {
            // set xSpeed from portal speed
            if (p->speedVal <= 0.8f) s.xSpeed = Phys::SPEED_SLOW / 240.f;
            else if (p->speedVal <= 0.95f) s.xSpeed = Phys::SPEED_NORMAL / 240.f;
            else if (p->speedVal <= 1.05f) s.xSpeed = Phys::SPEED_FAST / 240.f;
            else if (p->speedVal <= 1.15f) s.xSpeed = Phys::SPEED_FASTER / 240.f;
            else s.xSpeed = Phys::SPEED_FASTEST / 240.f;
        } else if (p->isPortal) {
            s.mode = p->portalMode;
            s.yVel *= 0.5f;
        }
    }

    // pads
    if (auto* pad = findPad(s.x, s.y, s.mini, s.mode)) {
        float b = 0.f;
        switch (pad->subType) {
            case 0: b = 12.f; break;
            case 1: b = 16.f; break;
            case 2: b = 20.f; break;
            case 3: b = -12.f; break;
            case 4: s.upsideDown = !s.upsideDown; b = 0.f; break;
        }
        if (b != 0.f) {
            s.yVel = s.upsideDown ? -b : b;
            s.onGround = false;
        }
    }

    // orbs (only when holding)
    if (hold && s.orbCD <= 0.f) {
        if (auto* orb = findOrb(s.x, s.y, s.mini, s.mode)) {
            if (orb->id != s.lastOrb) {
                float b = 0.f;
                bool flip = false;
                switch (orb->subType) {
                    case 0: b = 11.2f; break;
                    case 1: b = 14.f; break;
                    case 2: b = 18.f; break;
                    case 3: flip = true; b = 8.f; break;
                    case 4: flip = true; b = 11.2f; break;
                    case 6:
                    case 7: b = 15.f; break;
                    default: break;
                }

                if (flip) s.upsideDown = !s.upsideDown;
                if (b != 0.f) {
                    s.yVel = s.upsideDown ? -b : b;
                    s.onGround = false;
                }

                s.orbCD = 0.1f;
                s.lastOrb = orb->id;
            }
        }
    }
}

static void simFrame(SimState& s, bool hold) {
    if (s.dead) return;

    s.prevX = s.x;
    s.prevY = s.y;

    if (s.orbCD > 0.f) s.orbCD -= 1.f / 240.f;

    float g = Phys::GRAVITY * (s.mini ? 0.8f : 1.0f);
    if (s.upsideDown) g = -g;

    float groundY = s.upsideDown ? Phys::CEILING : Phys::GROUND;

    switch (s.mode) {
        case BotGameMode::Cube:
        case BotGameMode::Robot:
        case BotGameMode::Spider: {
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_VEL, Phys::MAX_VEL);
            s.y += s.yVel;

            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0.f;
                s.onGround = true;
                // allow jump again once released
            } else {
                s.onGround = false;
            }

            if (hold && s.onGround && s.canJump) {
                float j = s.mini ? Phys::JUMP_MINI : Phys::JUMP;
                s.yVel = s.upsideDown ? -j : j;
                s.onGround = false;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
        } break;

        case BotGameMode::Ship: {
            float acc = s.mini ? 0.6f : Phys::SHIP_ACCEL;
            float maxV = s.mini ? 6.f : Phys::SHIP_MAX;
            s.yVel += hold ? (s.upsideDown ? -acc : acc) : (s.upsideDown ? acc : -acc);
            s.yVel = std::clamp(s.yVel, -maxV, maxV);
            s.y += s.yVel;
            s.onGround = false;
        } break;

        case BotGameMode::Ball: {
            s.yVel -= g * 0.6f;
            s.yVel = std::clamp(s.yVel, -12.f, 12.f);
            s.y += s.yVel;

            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0.f;
                s.onGround = true;
            } else {
                s.onGround = false;
            }

            if (hold && s.onGround && s.canJump) {
                s.upsideDown = !s.upsideDown;
                s.yVel = s.upsideDown ? -Phys::BALL_VEL : Phys::BALL_VEL;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
        } break;

        case BotGameMode::UFO: {
            s.yVel -= g * 0.5f;
            s.yVel = std::clamp(s.yVel, -8.f, 8.f);
            s.y += s.yVel;

            if (hold && s.canJump) {
                float b = s.mini ? 5.5f : Phys::UFO_BOOST;
                s.yVel = s.upsideDown ? -b : b;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
        } break;

        case BotGameMode::Wave: {
            float ws = s.xSpeed * (s.mini ? 0.7f : 1.0f);
            s.y += hold ? (s.upsideDown ? -ws : ws) : (s.upsideDown ? ws : -ws);
            s.onGround = false;
        } break;

        case BotGameMode::Swing: {
            float sg = (hold ? -g : g) * 0.56f;
            s.yVel += sg;
            s.yVel = std::clamp(s.yVel, -8.f, 8.f);
            s.y += s.yVel;
            s.onGround = false;
        } break;
    }

    s.x += s.xSpeed;
    s.y = std::clamp(s.y, Phys::GROUND - 100.f, Phys::CEILING + 100.f);

    // interactions then collisions
    simInteract(s, hold);

    // hazards kill always
    if (willDieHazards(s.x, s.y, s.mini, s.mode)) {
        s.dead = true;
        return;
    }

    // solids
    if (isPlatformMode(s.mode)) {
        if (!resolveSolidsPlatform(s.x, s.y, s.yVel, s.onGround, s.upsideDown, s.mini, s.mode, s.prevX, s.prevY)) {
            s.dead = true;
            return;
        }
    } else {
        if (willDieOnSolidsNonPlatform(s.x, s.y, s.mini, s.mode)) {
            s.dead = true;
            return;
        }
    }
}

// ============================================================
// Planner (Beam Search)
// ============================================================

enum class ActionType : uint8_t { Hold, Release, Tap };

struct BeamNode {
    SimState s;
    float score = -1e30f;
    std::vector<ActionType> steps;
};

static float approxDistanceToDanger(const SimState& s) {
    // compute min distance to nearest hazard/solid AABB in a window
    float psz = getPlayerSizeFor(s.mini, s.mode);
    float minDist = 1e9f;

    auto scanSet = [&](const std::vector<LevelObj*>& vec, bool onlyForward) {
        float minX = s.x - 120.f;
        float maxX = s.x + g_settings.nearScanRange;
        if (onlyForward) minX = s.x - 20.f;

        auto [b, e] = rangeByX(vec, minX, maxX);
        for (auto it = b; it != e; ++it) {
            const LevelObj& o = **it;
            float ow = (o.w * g_settings.hazardHitboxScale) / 2.f;
            float oh = (o.h * g_settings.hazardHitboxScale) / 2.f;
            float ph = psz / 2.f;

            // AABB separation distance (0 if overlapping)
            float dx = std::max(0.f, std::abs(s.x - o.x) - (ph + ow));
            float dy = std::max(0.f, std::abs(s.y - o.y) - (ph + oh));
            float d = std::sqrt(dx*dx + dy*dy);
            if (d < minDist) minDist = d;
        }
    };

    scanSet(g_hazards, true);
    scanSet(g_solids, true);

    if (minDist > 1e8f) minDist = 999.f;
    return minDist;
}

static float scoreState(const SimState& s) {
    // progress heavily weighted
    float progress = s.x;

    // safety: map distance to 0..1
    float d = approxDistanceToDanger(s);
    float safe01 = std::clamp(d / 60.f, 0.f, 1.f);

    // keep y reasonable (very rough)
    float targetY = 300.f;
    float yPenalty = std::abs(s.y - targetY) * 0.002f;

    return progress * 10.f + safe01 * 800.f - yPenalty * 200.f;
}

class BeamPlanner {
public:
    void reset() {
        m_planSteps.clear();
        m_stepFrame = 0;
        m_sincePlan = 0;
    }

    bool getHold(const SimState& current, bool currentHold, bool& outHold) {
        if (m_planSteps.empty() || (++m_sincePlan >= g_settings.replanEvery)) {
            m_sincePlan = 0;
            plan(current, currentHold);
        }

        if (m_planSteps.empty()) {
            outHold = false;
            return false;
        }

        // Expand one step into per-frame output
        ActionType act = m_planSteps.front();
        outHold = holdForAction(act, m_stepFrame);

        m_stepFrame++;
        if (m_stepFrame >= g_settings.stepFrames) {
            m_stepFrame = 0;
            m_planSteps.pop_front();
        }

        return true;
    }

private:
    std::deque<ActionType> m_planSteps;
    int m_stepFrame = 0;
    int m_sincePlan = 0;

    static bool holdForAction(ActionType a, int stepFrameIndex) {
        switch (a) {
            case ActionType::Hold:    return true;
            case ActionType::Release: return false;
            case ActionType::Tap:     return stepFrameIndex < g_settings.tapFrames;
        }
        return false;
    }

    static std::vector<ActionType> actionSetForMode(BotGameMode mode) {
        // Tap is important for UFO/Ball; wave/ship still benefit sometimes.
        if (mode == BotGameMode::UFO || mode == BotGameMode::Ball) {
            return { ActionType::Release, ActionType::Tap, ActionType::Hold };
        }
        return { ActionType::Release, ActionType::Hold, ActionType::Tap };
    }

    void plan(const SimState& start, bool startHold) {
        m_planSteps.clear();
        m_stepFrame = 0;

        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<BeamNode> beam;
        beam.reserve((size_t)g_settings.beamWidth);

        BeamNode root;
        root.s = start;
        root.score = scoreState(start);
        beam.push_back(std::move(root));

        std::vector<BeamNode> next;
        next.reserve((size_t)g_settings.beamWidth * 3);

        int steps = std::max(1, g_settings.simFrames / std::max(1, g_settings.stepFrames));
        auto actions = actionSetForMode(start.mode);

        for (int step = 0; step < steps; ++step) {
            next.clear();

            for (auto const& n : beam) {
                for (auto act : actions) {
                    BeamNode c;
                    c.s = n.s;
                    c.steps = n.steps;
                    c.steps.push_back(act);

                    // simulate stepFrames with this action
                    for (int k = 0; k < g_settings.stepFrames; ++k) {
                        bool hold = holdForAction(act, k);
                        simFrame(c.s, hold);
                        if (c.s.dead) break;
                    }
                    if (c.s.dead) continue;

                    c.score = scoreState(c.s);
                    next.push_back(std::move(c));
                }
            }

            if (next.empty()) break;

            std::nth_element(
                next.begin(),
                next.begin() + std::min((int)next.size() - 1, g_settings.beamWidth - 1),
                next.end(),
                [](const BeamNode& a, const BeamNode& b) { return a.score > b.score; }
            );

            std::sort(next.begin(), next.end(), [](const BeamNode& a, const BeamNode& b) {
                return a.score > b.score;
            });

            if ((int)next.size() > g_settings.beamWidth) next.resize((size_t)g_settings.beamWidth);
            beam = next;

            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            if (ms >= g_settings.timeBudgetMs) break;
        }

        if (!beam.empty()) {
            auto const& best = beam.front();
            for (auto a : best.steps) m_planSteps.push_back(a);
        }
    }
};

static BeamPlanner g_planner;

// ============================================================
// Debug HUD (screen-space, stable)
// ============================================================

class BotHUD : public CCNode {
public:
    CCLabelBMFont* l1 = nullptr;
    CCLabelBMFont* l2 = nullptr;

    static BotHUD* create() {
        auto* r = new BotHUD();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        auto ws = CCDirector::sharedDirector()->getWinSize();

        l1 = CCLabelBMFont::create("Bot: OFF", "bigFont.fnt");
        l1->setScale(0.4f);
        l1->setAnchorPoint(ccp(0, 1));
        l1->setPosition(ccp(10.f, ws.height - 10.f));
        addChild(l1);

        l2 = CCLabelBMFont::create("", "chatFont.fnt");
        l2->setScale(0.55f);
        l2->setAnchorPoint(ccp(0, 1));
        l2->setPosition(ccp(10.f, ws.height - 35.f));
        addChild(l2);

        scheduleUpdate();
        return true;
    }

    void update(float) override {
        g_settings.load();
        setVisible(g_settings.debugOverlay);

        l1->setString(g_botEnabled ? "Bot: ON" : "Bot: OFF");
        l1->setColor(g_botEnabled ? ccc3(0,255,0) : ccc3(255,100,100));

        const char* modes[] = {"Cube","Ship","Ball","UFO","Wave","Robot","Spider","Swing"};
        char buf[256];
        snprintf(buf, sizeof(buf), "X:%.0f  Y:%.0f  vY:%.2f  %s",
            g_playerX, g_playerY, g_playerYVel, modes[(int)g_gameMode]);
        l2->setString(buf);
    }
};

static BotHUD* g_hud = nullptr;

// ============================================================
// Debug Draw (world-space, on object layer)
// ============================================================

class BotWorldDebug : public CCNode {
public:
    CCDrawNode* dn = nullptr;

    static BotWorldDebug* create() {
        auto* r = new BotWorldDebug();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        dn = CCDrawNode::create();
        addChild(dn);
        scheduleUpdate();
        return true;
    }

    void update(float) override {
        g_settings.load();
        dn->clear();
        if (!g_settings.debugDraw || !g_levelAnalyzed) return;

        float psz = getPlayerSizeFor(g_isMini, g_gameMode);
        dn->drawRect(
            ccp(g_playerX - psz/2, g_playerY - psz/2),
            ccp(g_playerX + psz/2, g_playerY + psz/2),
            ccc4f(1,1,1,0.2f), 1.f, ccc4f(1,1,1,0.8f)
        );

        float minX = g_playerX - 200.f;
        float maxX = g_playerX + 800.f;

        auto drawBox = [&](const LevelObj& o, ccColor4F fill, ccColor4F stroke) {
            float ow = (o.w * g_settings.hazardHitboxScale) / 2.f;
            float oh = (o.h * g_settings.hazardHitboxScale) / 2.f;
            dn->drawRect(ccp(o.x-ow, o.y-oh), ccp(o.x+ow, o.y+oh), fill, 1.f, stroke);
        };

        auto [hb, he] = rangeByX(g_hazards, minX, maxX);
        for (auto it = hb; it != he; ++it) drawBox(**it, ccc4f(1,0,0,0.12f), ccc4f(1,0,0,0.45f));

        auto [sb, se] = rangeByX(g_solids, minX, maxX);
        for (auto it = sb; it != se; ++it) drawBox(**it, ccc4f(0.2f,0.6f,1,0.10f), ccc4f(0.2f,0.6f,1,0.35f));
    }
};

static BotWorldDebug* g_dbg = nullptr;

// ============================================================
// Hooks
// ============================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_settings.load();
        g_planner.reset();

        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;

        // Attach HUD to running scene (screen space, does not move)
        if (!g_hud) g_hud = BotHUD::create();
        if (g_hud->getParent()) g_hud->removeFromParentAndCleanup(false);
        CCDirector::sharedDirector()->getRunningScene()->addChild(g_hud, 999999);

        // Attach debug draw to object layer (world space)
        if (!g_dbg) g_dbg = BotWorldDebug::create();
        if (g_dbg->getParent()) g_dbg->removeFromParentAndCleanup(false);
        if (this->m_objectLayer) this->m_objectLayer->addChild(g_dbg, 999998);
        else this->addChild(g_dbg, 999998);

        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        analyzeLevel(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_planner.reset();

        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }

        // some levels rebuild objects after reset; ensure analyzed
        if (!g_levelAnalyzed) analyzeLevel(this);
    }

    void onQuit() {
        g_planner.reset();
        if (g_hud && g_hud->getParent()) g_hud->removeFromParentAndCleanup(false);
        if (g_dbg && g_dbg->getParent()) g_dbg->removeFromParentAndCleanup(false);
        PlayLayer::onQuit();
    }
};

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        g_frameCounter++;

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) return;
        if (!g_levelAnalyzed) return;
        if (!g_botEnabled) return;
        if (pl->m_isPaused || pl->m_hasCompletedLevel || m_player1->m_isDead) return;

        // read player state
        g_playerX = m_player1->getPositionX();
        g_playerY = m_player1->getPositionY();
        g_playerYVel = m_player1->m_yVelocity;
        g_playerOnGround = m_player1->m_isOnGround;
        g_isUpsideDown = m_player1->m_isUpsideDown;
        g_isMini = (m_player1->m_vehicleSize != 1.0f);

        if (m_player1->m_isShip) g_gameMode = BotGameMode::Ship;
        else if (m_player1->m_isBall) g_gameMode = BotGameMode::Ball;
        else if (m_player1->m_isBird) g_gameMode = BotGameMode::UFO;
        else if (m_player1->m_isDart) g_gameMode = BotGameMode::Wave;
        else if (m_player1->m_isRobot) g_gameMode = BotGameMode::Robot;
        else if (m_player1->m_isSpider) g_gameMode = BotGameMode::Spider;
        else if (m_player1->m_isSwing) g_gameMode = BotGameMode::Swing;
        else g_gameMode = BotGameMode::Cube;

        float spd = m_player1->m_playerSpeed;
        if (spd <= 0.8f) g_xSpeed = Phys::SPEED_SLOW / 240.f;
        else if (spd <= 0.95f) g_xSpeed = Phys::SPEED_NORMAL / 240.f;
        else if (spd <= 1.05f) g_xSpeed = Phys::SPEED_FAST / 240.f;
        else if (spd <= 1.15f) g_xSpeed = Phys::SPEED_FASTER / 240.f;
        else g_xSpeed = Phys::SPEED_FASTEST / 240.f;

        // reload settings occasionally
        if (g_frameCounter % 120 == 0) g_settings.load();

        // build sim start state
        SimState cur;
        cur.x = g_playerX;
        cur.y = g_playerY;
        cur.yVel = g_playerYVel;
        cur.xSpeed = g_xSpeed;
        cur.onGround = g_playerOnGround;
        // IMPORTANT: approximate canJump based on hold state
        cur.canJump = !g_isHolding;
        cur.upsideDown = g_isUpsideDown;
        cur.mini = g_isMini;
        cur.mode = g_gameMode;
        cur.dead = false;

        bool plannedHold = false;
        g_planner.getHold(cur, g_isHolding, plannedHold);

        if (plannedHold != g_isHolding) {
            this->handleButton(plannedHold, 1, true);
            g_isHolding = plannedHold;
        }
    }
};

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto ws = CCDirector::sharedDirector()->getWinSize();

        auto* menu = CCMenu::create();
        menu->setPosition(ccp(0, 0));
        this->addChild(menu, 200);

        // Bot toggle
        auto* onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* togg = CCMenuItemToggler::create(offSpr, onSpr, this, menu_selector(BotPauseLayer::onToggleBot));
        togg->setPosition(ccp(ws.width - 30.f, ws.height - 30.f));
        togg->toggle(g_botEnabled);
        menu->addChild(togg);

        // Settings gear
        auto* gearSpr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        auto* gearBtn = CCMenuItemSpriteExtra::create(gearSpr, this, menu_selector(BotPauseLayer::onOpenSettings));
        gearBtn->setPosition(ccp(ws.width - 30.f, ws.height - 75.f));
        gearBtn->setScale(0.7f);
        menu->addChild(gearBtn);
    }

    void onToggleBot(CCObject*) {
        g_botEnabled = !g_botEnabled;
        g_planner.reset();

        if (!g_botEnabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
    }

    void onOpenSettings(CCObject*) {
        // This is the API you said worked in your earlier version.
        openSettingsPopup(Mod::get());
    }
};

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && key == KEY_F8) {
            g_botEnabled = !g_botEnabled;
            g_planner.reset();

            if (!g_botEnabled && g_isHolding) {
                if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
                g_isHolding = false;
            }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

$on_mod(Loaded) {
    g_settings.load();
    log::info("GD AutoBot loaded (F8 toggle).");
}
