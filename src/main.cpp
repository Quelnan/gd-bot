#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ============================================================================
// GLOBAL STATE
// ============================================================================

static bool g_botEnabled = false;
static bool g_levelAnalyzed = false;
static bool g_isHolding = false;
static int g_frameCounter = 0;
static int g_totalClicks = 0;
static int g_attempts = 0;
static float g_bestProgress = 0.0f;

// Player state
static float g_playerX = 0;
static float g_playerY = 0;
static float g_playerYVel = 0;
static bool g_playerOnGround = false;
static bool g_isUpsideDown = false;
static bool g_isMini = false;
static float g_xSpeed = 311.58f / 240.0f;

// Gamemode
enum class BotGameMode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };
static BotGameMode g_gameMode = BotGameMode::Cube;

// ============================================================================
// SETTINGS
// ============================================================================

struct BotSettings {
    int simFrames = 60;
    int holdDuration = 12;
    float hitboxScale = 0.9f;
    float hazardHitboxScale = 1.0f;
    float safeMargin = 2.0f;
    float reactionDistance = 150.0f;
    bool debugOverlay = true;
    bool debugDraw = false;
    
    void load() {
        auto* mod = Mod::get();
        simFrames = static_cast<int>(mod->getSettingValue<int64_t>("sim-frames"));
        holdDuration = static_cast<int>(mod->getSettingValue<int64_t>("hold-duration"));
        hitboxScale = static_cast<float>(mod->getSettingValue<double>("hitbox-scale"));
        hazardHitboxScale = static_cast<float>(mod->getSettingValue<double>("hazard-hitbox-scale"));
        safeMargin = static_cast<float>(mod->getSettingValue<double>("safe-margin"));
        reactionDistance = static_cast<float>(mod->getSettingValue<double>("reaction-distance"));
        debugOverlay = mod->getSettingValue<bool>("debug-overlay");
        debugDraw = mod->getSettingValue<bool>("debug-draw");
    }
};

static BotSettings g_settings;

// ============================================================================
// HITBOX DATA
// ============================================================================

struct HitboxInfo {
    float w;
    float h;
    bool triangle;
};

static const std::map<int, HitboxInfo> HITBOX_DATA = {
    {8, {20, 20, true}}, {39, {20, 20, true}}, {103, {20, 20, true}},
    {392, {12, 12, true}}, {9, {24, 24, true}}, {61, {24, 24, true}},
    {243, {20, 20, true}}, {244, {20, 20, true}}, {245, {20, 20, true}},
    {246, {20, 20, true}}, {247, {20, 20, true}}, {248, {20, 20, true}}, {249, {20, 20, true}},
    {446, {20, 20, true}}, {447, {20, 20, true}},
    {678, {24, 24, true}}, {679, {24, 24, true}}, {680, {24, 24, true}},
    {1705, {20, 20, true}}, {1706, {20, 20, true}}, {1707, {20, 20, true}},
    {1708, {20, 20, true}}, {1709, {20, 20, true}}, {1710, {20, 20, true}},
    {1711, {20, 20, true}}, {1712, {20, 20, true}}, {1713, {20, 20, true}},
    {1714, {20, 20, true}}, {1715, {20, 20, true}}, {1716, {20, 20, true}},
    {1717, {20, 20, true}}, {1718, {20, 20, true}},
    {363, {45, 45, false}}, {364, {60, 60, false}}, {365, {75, 75, false}},
    {366, {45, 45, false}}, {367, {60, 60, false}}, {368, {75, 75, false}},
};

static const std::set<int> HAZARD_IDS = {
    8, 39, 103, 392, 9, 61,
    243, 244, 245, 246, 247, 248, 249,
    363, 364, 365, 366, 367, 368,
    446, 447, 678, 679, 680,
    1705, 1706, 1707, 1708, 1709, 1710,
    1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718,
    88, 89, 98, 397, 398, 399, 740, 741, 742
};

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

// ============================================================================
// LEVEL OBJECTS
// ============================================================================

struct LevelObj {
    int id = 0;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    float rot = 0;
    float scaleX = 1;
    float scaleY = 1;
    bool triangle = false;
    bool isHazard = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;
    int subType = 0;
    BotGameMode portalMode = BotGameMode::Cube;
    bool gravityPortal = false;
    bool gravityUp = false;
    bool sizePortal = false;
    bool sizeMini = false;
    bool speedPortal = false;
    float speedVal = 1.0f;
};

static std::vector<LevelObj> g_objects;
static std::vector<LevelObj*> g_hazards;
static std::vector<LevelObj*> g_orbs;
static std::vector<LevelObj*> g_pads;
static std::vector<LevelObj*> g_portals;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

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

// ============================================================================
// ANALYZE LEVEL
// ============================================================================

void analyzeLevel(PlayLayer* pl) {
    g_objects.clear();
    g_hazards.clear();
    g_orbs.clear();
    g_pads.clear();
    g_portals.clear();
    g_levelAnalyzed = false;
    
    if (!pl || !pl->m_objects) return;
    
    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;
        
        int id = obj->m_objectID;
        
        LevelObj lo;
        lo.id = id;
        lo.x = obj->getPositionX();
        lo.y = obj->getPositionY();
        lo.rot = obj->getRotation();
        lo.scaleX = obj->getScaleX();
        lo.scaleY = obj->getScaleY();
        
        if (HITBOX_DATA.count(id)) {
            const HitboxInfo& hb = HITBOX_DATA.at(id);
            lo.w = hb.w * std::abs(lo.scaleX);
            lo.h = hb.h * std::abs(lo.scaleY);
            lo.triangle = hb.triangle;
        } else {
            CCSize sz = obj->getContentSize();
            float sc = obj->getScale();
            lo.w = sz.width * sc * 0.8f;
            lo.h = sz.height * sc * 0.8f;
        }
        
        lo.w = std::max(lo.w, 5.0f);
        lo.h = std::max(lo.h, 5.0f);
        
        bool important = false;
        
        if (HAZARD_IDS.count(id)) {
            lo.isHazard = true;
            important = true;
        }
        else if (ORB_IDS.count(id)) {
            lo.isOrb = true;
            lo.subType = ORB_IDS.at(id);
            important = true;
        }
        else if (PAD_IDS.count(id)) {
            lo.isPad = true;
            lo.subType = PAD_IDS.at(id);
            important = true;
        }
        else if (PORTAL_IDS.count(id)) {
            lo.isPortal = true;
            lo.portalMode = PORTAL_IDS.at(id);
            important = true;
        }
        else if (id == 10) { lo.isPortal = true; lo.gravityPortal = true; lo.gravityUp = false; important = true; }
        else if (id == 11) { lo.isPortal = true; lo.gravityPortal = true; lo.gravityUp = true; important = true; }
        else if (id == 99) { lo.isPortal = true; lo.sizePortal = true; lo.sizeMini = false; important = true; }
        else if (id == 101) { lo.isPortal = true; lo.sizePortal = true; lo.sizeMini = true; important = true; }
        else if (id == 200) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 0.7f; important = true; }
        else if (id == 201) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 0.9f; important = true; }
        else if (id == 202) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.0f; important = true; }
        else if (id == 203) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.1f; important = true; }
        else if (id == 1334) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.3f; important = true; }
        
        if (important) {
            g_objects.push_back(lo);
        }
    }
    
    std::sort(g_objects.begin(), g_objects.end(), [](const LevelObj& a, const LevelObj& b) {
        return a.x < b.x;
    });
    
    for (LevelObj& o : g_objects) {
        if (o.isHazard) g_hazards.push_back(&o);
        if (o.isOrb) g_orbs.push_back(&o);
        if (o.isPad) g_pads.push_back(&o);
        if (o.isPortal) g_portals.push_back(&o);
    }
    
    log::info("Bot: {} hazards, {} orbs, {} pads, {} portals",
        g_hazards.size(), g_orbs.size(), g_pads.size(), g_portals.size());
    g_levelAnalyzed = true;
}

// ============================================================================
// COLLISION DETECTION
// ============================================================================

float getPlayerSize() {
    float sz = Phys::PLAYER_SIZE;
    if (g_isMini) sz *= Phys::MINI_SCALE;
    if (g_gameMode == BotGameMode::Wave) sz *= Phys::WAVE_SCALE;
    return sz * g_settings.hitboxScale;
}

bool pointInTriangle(float px, float py, float x1, float y1, float x2, float y2, float x3, float y3) {
    auto sign = [](float p1x, float p1y, float p2x, float p2y, float p3x, float p3y) -> float {
        return (p1x - p3x) * (p2y - p3y) - (p2x - p3x) * (p1y - p3y);
    };
    float d1 = sign(px, py, x1, y1, x2, y2);
    float d2 = sign(px, py, x2, y2, x3, y3);
    float d3 = sign(px, py, x3, y3, x1, y1);
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(hasNeg && hasPos);
}

bool checkTriangleCollision(float px, float py, float psz, const LevelObj& s) {
    float hw = (s.w * g_settings.hazardHitboxScale) / 2.0f;
    float hh = (s.h * g_settings.hazardHitboxScale) / 2.0f;
    float rad = s.rot * 3.14159f / 180.0f;
    float cosR = std::cos(rad);
    float sinR = std::sin(rad);
    
    float pts[3][2] = {{-hw, -hh}, {hw, -hh}, {0, hh}};
    for (int i = 0; i < 3; i++) {
        float nx = pts[i][0] * cosR - pts[i][1] * sinR + s.x;
        float ny = pts[i][0] * sinR + pts[i][1] * cosR + s.y;
        pts[i][0] = nx;
        pts[i][1] = ny;
    }
    
    float half = psz / 2.0f + g_settings.safeMargin;
    float corners[4][2] = {
        {px - half, py - half},
        {px + half, py - half},
        {px - half, py + half},
        {px + half, py + half}
    };
    
    for (int i = 0; i < 4; i++) {
        if (pointInTriangle(corners[i][0], corners[i][1], pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1])) {
            return true;
        }
    }
    return false;
}

bool checkAABB(float px, float py, float psz, const LevelObj& o) {
    float ph = psz / 2.0f + g_settings.safeMargin;
    float ow = (o.w * g_settings.hazardHitboxScale) / 2.0f;
    float oh = (o.h * g_settings.hazardHitboxScale) / 2.0f;
    return !(px + ph < o.x - ow || px - ph > o.x + ow || py + ph < o.y - oh || py - ph > o.y + oh);
}

bool willHitHazard(float px, float py) {
    float psz = getPlayerSize();
    for (const LevelObj* h : g_hazards) {
        if (h->x < px - 50.0f || h->x > px + g_settings.reactionDistance) continue;
        if (h->triangle) {
            if (checkTriangleCollision(px, py, psz, *h)) return true;
        } else {
            if (checkAABB(px, py, psz, *h)) return true;
        }
    }
    if (py < Phys::GROUND - 30.0f || py > Phys::CEILING + 30.0f) return true;
    return false;
}

LevelObj* findOrb(float px, float py) {
    float psz = getPlayerSize() * 1.5f;
    for (LevelObj* o : g_orbs) {
        if (std::abs(o->x - px) > 50.0f) continue;
        if (checkAABB(px, py, psz, *o)) return o;
    }
    return nullptr;
}

LevelObj* findPad(float px, float py) {
    float psz = getPlayerSize();
    for (LevelObj* p : g_pads) {
        if (std::abs(p->x - px) > 50.0f) continue;
        if (checkAABB(px, py, psz, *p)) return p;
    }
    return nullptr;
}

LevelObj* findPortal(float px, float py) {
    float psz = getPlayerSize() * 2.0f;
    for (LevelObj* p : g_portals) {
        if (std::abs(p->x - px) > 50.0f) continue;
        if (checkAABB(px, py, psz, *p)) return p;
    }
    return nullptr;
}

// ============================================================================
// SIMULATION STATE
// ============================================================================

struct SimState {
    float x;
    float y;
    float yVel;
    float xSpeed;
    bool onGround;
    bool canJump;
    bool upsideDown;
    bool mini;
    BotGameMode mode;
    float orbCD;
    int lastOrb;
};

// ============================================================================
// SIMULATION
// ============================================================================

void simFrame(SimState& s, bool hold) {
    if (s.orbCD > 0) s.orbCD -= 1.0f / 240.0f;
    
    float g = Phys::GRAVITY * (s.mini ? 0.8f : 1.0f) * (s.upsideDown ? -1.0f : 1.0f);
    float groundY = s.upsideDown ? Phys::CEILING : Phys::GROUND;
    
    switch (s.mode) {
        case BotGameMode::Cube: {
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_VEL, Phys::MAX_VEL);
            s.y += s.yVel;
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
                s.onGround = true;
                s.canJump = true;
            } else {
                s.onGround = false;
            }
            if (hold && s.onGround && s.canJump) {
                s.yVel = (s.upsideDown ? -1.0f : 1.0f) * (s.mini ? Phys::JUMP_MINI : Phys::JUMP);
                s.onGround = false;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
            break;
        }
        case BotGameMode::Ship: {
            float acc = s.mini ? 0.6f : Phys::SHIP_ACCEL;
            float maxV = s.mini ? 6.0f : Phys::SHIP_MAX;
            if (hold) {
                s.yVel += s.upsideDown ? -acc : acc;
            } else {
                s.yVel += s.upsideDown ? acc : -acc;
            }
            s.yVel = std::clamp(s.yVel, -maxV, maxV);
            s.y += s.yVel;
            if (s.y < Phys::GROUND) {
                s.y = Phys::GROUND;
                s.yVel = std::max(s.yVel, 0.0f);
            }
            if (s.y > Phys::CEILING) {
                s.y = Phys::CEILING;
                s.yVel = std::min(s.yVel, 0.0f);
            }
            break;
        }
        case BotGameMode::Ball: {
            s.yVel -= g * 0.6f;
            s.yVel = std::clamp(s.yVel, -12.0f, 12.0f);
            s.y += s.yVel;
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
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
            break;
        }
        case BotGameMode::UFO: {
            s.yVel -= g * 0.5f;
            s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
            s.y += s.yVel;
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
            }
            if (hold && s.canJump) {
                s.yVel = (s.upsideDown ? -1.0f : 1.0f) * (s.mini ? 5.5f : Phys::UFO_BOOST);
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
            break;
        }
        case BotGameMode::Wave: {
            float ws = s.xSpeed * (s.mini ? 0.7f : 1.0f);
            if (hold) {
                s.y += s.upsideDown ? -ws : ws;
            } else {
                s.y += s.upsideDown ? ws : -ws;
            }
            break;
        }
        case BotGameMode::Robot: {
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_VEL, Phys::MAX_VEL);
            s.y += s.yVel;
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
                s.onGround = true;
                s.canJump = true;
            } else {
                s.onGround = false;
            }
            if (hold && s.onGround && s.canJump) {
                s.yVel = (s.upsideDown ? -1.0f : 1.0f) * 10.0f;
                s.onGround = false;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
            break;
        }
        case BotGameMode::Spider: {
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_VEL, Phys::MAX_VEL);
            s.y += s.yVel;
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
                s.onGround = true;
                s.canJump = true;
            } else {
                s.onGround = false;
            }
            if (hold && s.onGround && s.canJump) {
                s.upsideDown = !s.upsideDown;
                s.y = s.upsideDown ? Phys::CEILING : Phys::GROUND;
                s.yVel = 0;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
            break;
        }
        case BotGameMode::Swing: {
            float sg = (hold ? -g : g) * 0.56f;
            s.yVel += sg;
            s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
            s.y += s.yVel;
            break;
        }
    }
    
    s.x += s.xSpeed;
    s.y = std::clamp(s.y, Phys::GROUND - 30.0f, Phys::CEILING + 30.0f);
}

void simInteract(SimState& s, bool hold) {
    LevelObj* portal = findPortal(s.x, s.y);
    if (portal) {
        if (portal->gravityPortal) {
            s.upsideDown = portal->gravityUp;
        } else if (portal->sizePortal) {
            s.mini = portal->sizeMini;
        } else if (portal->speedPortal) {
            if (portal->speedVal <= 0.8f) s.xSpeed = Phys::SPEED_SLOW / 240.0f;
            else if (portal->speedVal <= 0.95f) s.xSpeed = Phys::SPEED_NORMAL / 240.0f;
            else if (portal->speedVal <= 1.05f) s.xSpeed = Phys::SPEED_FAST / 240.0f;
            else if (portal->speedVal <= 1.15f) s.xSpeed = Phys::SPEED_FASTER / 240.0f;
            else s.xSpeed = Phys::SPEED_FASTEST / 240.0f;
        } else if (portal->isPortal) {
            s.mode = portal->portalMode;
            s.yVel *= 0.5f;
        }
    }
    
    LevelObj* pad = findPad(s.x, s.y);
    if (pad) {
        float b = 0;
        switch (pad->subType) {
            case 0: b = 12.0f; break;
            case 1: b = 16.0f; break;
            case 2: b = 20.0f; break;
            case 3: b = -12.0f; break;
            case 4: s.upsideDown = !s.upsideDown; break;
        }
        if (b != 0) {
            s.yVel = s.upsideDown ? -b : b;
            s.onGround = false;
        }
    }
    
    if (hold && s.orbCD <= 0) {
        LevelObj* orb = findOrb(s.x, s.y);
        if (orb && orb->id != s.lastOrb) {
            float b = 0;
            bool flip = false;
            switch (orb->subType) {
                case 0: b = 11.2f; break;
                case 1: b = 14.0f; break;
                case 2: b = 18.0f; break;
                case 3: flip = true; b = 8.0f; break;
                case 4: flip = true; b = 11.2f; break;
                case 6:
                case 7: b = 15.0f; break;
            }
            if (flip) s.upsideDown = !s.upsideDown;
            if (b != 0) {
                s.yVel = s.upsideDown ? -b : b;
                s.onGround = false;
            }
            s.orbCD = 0.1f;
            s.lastOrb = orb->id;
        }
    }
}

// ============================================================================
// BOT DECISION
// ============================================================================

bool shouldBotClick() {
    SimState base;
    base.x = g_playerX;
    base.y = g_playerY;
    base.yVel = g_playerYVel;
    base.xSpeed = g_xSpeed;
    base.onGround = g_playerOnGround;
    base.canJump = true;
    base.upsideDown = g_isUpsideDown;
    base.mini = g_isMini;
    base.mode = g_gameMode;
    base.orbCD = 0;
    base.lastOrb = -1;
    
    int frames = g_settings.simFrames;
    int holdF = g_settings.holdDuration;
    
    // Simulate without clicking
    SimState sNo = base;
    int survNo = 0;
    for (int i = 0; i < frames; i++) {
        simFrame(sNo, false);
        simInteract(sNo, false);
        if (willHitHazard(sNo.x, sNo.y)) break;
        survNo++;
    }
    
    // Simulate with clicking
    SimState sYes = base;
    int survYes = 0;
    for (int i = 0; i < frames; i++) {
        bool h = i < holdF;
        simFrame(sYes, h);
        simInteract(sYes, h);
        if (willHitHazard(sYes.x, sYes.y)) break;
        survYes++;
    }
    
    return survYes > survNo;
}

// ============================================================================
// DEBUG OVERLAY
// ============================================================================

class BotOverlay : public CCNode {
public:
    CCLabelBMFont* m_lbl1 = nullptr;
    CCLabelBMFont* m_lbl2 = nullptr;
    CCLabelBMFont* m_lbl3 = nullptr;
    CCDrawNode* m_draw = nullptr;
    
    static BotOverlay* create() {
        BotOverlay* ret = new BotOverlay();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
    
    bool init() override {
        if (!CCNode::init()) return false;
        
        m_draw = CCDrawNode::create();
        addChild(m_draw, 10);
        
        m_lbl1 = CCLabelBMFont::create("Bot: OFF", "bigFont.fnt");
        m_lbl1->setScale(0.4f);
        m_lbl1->setAnchorPoint(ccp(0, 1));
        addChild(m_lbl1, 100);
        
        m_lbl2 = CCLabelBMFont::create("", "chatFont.fnt");
        m_lbl2->setScale(0.5f);
        m_lbl2->setAnchorPoint(ccp(0, 1));
        addChild(m_lbl2, 100);
        
        m_lbl3 = CCLabelBMFont::create("", "chatFont.fnt");
        m_lbl3->setScale(0.5f);
        m_lbl3->setAnchorPoint(ccp(0, 1));
        addChild(m_lbl3, 100);
        
        scheduleUpdate();
        return true;
    }
    
    void update(float dt) override {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;
        
        // Estimate camera position based on player
        float camX = g_playerX - 200.0f;
        float camY = 0.0f;
        
        // Position labels to follow camera
        m_lbl1->setPosition(ccp(camX + 5.0f, camY + 320.0f));
        m_lbl2->setPosition(ccp(camX + 5.0f, camY + 295.0f));
        m_lbl3->setPosition(ccp(camX + 5.0f, camY + 275.0f));
        
        m_lbl1->setVisible(g_settings.debugOverlay);
        m_lbl2->setVisible(g_settings.debugOverlay);
        m_lbl3->setVisible(g_settings.debugOverlay);
        
        if (g_settings.debugOverlay) {
            m_lbl1->setString(g_botEnabled ? "Bot: ON" : "Bot: OFF");
            m_lbl1->setColor(g_botEnabled ? ccc3(0, 255, 0) : ccc3(255, 100, 100));
            
            char buf[128];
            snprintf(buf, sizeof(buf), "X:%.0f Y:%.0f Vel:%.1f", g_playerX, g_playerY, g_playerYVel);
            m_lbl2->setString(buf);
            
            const char* modes[] = {"Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing"};
            snprintf(buf, sizeof(buf), "%s %s Clicks:%d Best:%.1f%%",
                modes[static_cast<int>(g_gameMode)],
                g_playerOnGround ? "[G]" : "[A]",
                g_totalClicks,
                g_bestProgress);
            m_lbl3->setString(buf);
        }
        
        // Debug drawing
        m_draw->clear();
        if (!g_settings.debugDraw || !g_levelAnalyzed) return;
        
        // Draw player hitbox
        float psz = getPlayerSize();
        m_draw->drawRect(
            ccp(g_playerX - psz / 2.0f, g_playerY - psz / 2.0f),
            ccp(g_playerX + psz / 2.0f, g_playerY + psz / 2.0f),
            ccc4f(1, 1, 1, 0.3f),
            1.0f,
            ccc4f(1, 1, 1, 0.8f));
        
        // Draw hazards
        for (const LevelObj* h : g_hazards) {
            if (h->x < g_playerX - 100.0f || h->x > g_playerX + 400.0f) continue;
            float hw = (h->w * g_settings.hazardHitboxScale) / 2.0f;
            float hh = (h->h * g_settings.hazardHitboxScale) / 2.0f;
            m_draw->drawRect(
                ccp(h->x - hw, h->y - hh),
                ccp(h->x + hw, h->y + hh),
                ccc4f(1, 0, 0, 0.2f),
                1.0f,
                ccc4f(1, 0, 0, 0.6f));
        }
        
        // Draw orbs
        for (const LevelObj* o : g_orbs) {
            if (o->x < g_playerX - 100.0f || o->x > g_playerX + 300.0f) continue;
            m_draw->drawDot(ccp(o->x, o->y), 8.0f, ccc4f(1, 1, 0, 0.6f));
        }
        
        // Draw pads
        for (const LevelObj* p : g_pads) {
            if (p->x < g_playerX - 100.0f || p->x > g_playerX + 300.0f) continue;
            m_draw->drawDot(ccp(p->x, p->y), 6.0f, ccc4f(1, 0, 1, 0.6f));
        }
        
        // Draw trajectory without clicking (red)
        SimState sNo;
        sNo.x = g_playerX;
        sNo.y = g_playerY;
        sNo.yVel = g_playerYVel;
        sNo.xSpeed = g_xSpeed;
        sNo.onGround = g_playerOnGround;
        sNo.canJump = true;
        sNo.upsideDown = g_isUpsideDown;
        sNo.mini = g_isMini;
        sNo.mode = g_gameMode;
        sNo.orbCD = 0;
        sNo.lastOrb = -1;
        
        for (int i = 0; i < 50; i++) {
            float ox = sNo.x;
            float oy = sNo.y;
            simFrame(sNo, false);
            float alpha = 0.7f * (1.0f - static_cast<float>(i) / 50.0f);
            m_draw->drawSegment(ccp(ox, oy), ccp(sNo.x, sNo.y), 1.5f, ccc4f(1, 0.3f, 0.3f, alpha));
            if (willHitHazard(sNo.x, sNo.y)) break;
        }
        
        // Draw trajectory with clicking (green)
        SimState sYes;
        sYes.x = g_playerX;
        sYes.y = g_playerY;
        sYes.yVel = g_playerYVel;
        sYes.xSpeed = g_xSpeed;
        sYes.onGround = g_playerOnGround;
        sYes.canJump = true;
        sYes.upsideDown = g_isUpsideDown;
        sYes.mini = g_isMini;
        sYes.mode = g_gameMode;
        sYes.orbCD = 0;
        sYes.lastOrb = -1;
        
        for (int i = 0; i < 50; i++) {
            float ox = sYes.x;
            float oy = sYes.y;
            simFrame(sYes, i < g_settings.holdDuration);
            float alpha = 0.7f * (1.0f - static_cast<float>(i) / 50.0f);
            m_draw->drawSegment(ccp(ox, oy), ccp(sYes.x, sYes.y), 1.5f, ccc4f(0.3f, 1, 0.3f, alpha));
            if (willHitHazard(sYes.x, sYes.y)) break;
        }
    }
};

static BotOverlay* g_overlay = nullptr;

// ============================================================================
// GJBASEGAMELAYER HOOK
// ============================================================================

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        
        g_frameCounter++;
        
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;
        if (!g_botEnabled) return;
        if (!m_player1) return;
        if (pl->m_isPaused) return;
        if (pl->m_hasCompletedLevel) return;
        if (m_player1->m_isDead) return;
        if (!g_levelAnalyzed) return;
        
        // Read player state
        g_playerX = m_player1->getPositionX();
        g_playerY = m_player1->getPositionY();
        g_playerYVel = m_player1->m_yVelocity;
        g_playerOnGround = m_player1->m_isOnGround;
        g_isUpsideDown = m_player1->m_isUpsideDown;
        g_isMini = m_player1->m_vehicleSize != 1.0f;
        
        // Determine gamemode
        if (m_player1->m_isShip) g_gameMode = BotGameMode::Ship;
        else if (m_player1->m_isBall) g_gameMode = BotGameMode::Ball;
        else if (m_player1->m_isBird) g_gameMode = BotGameMode::UFO;
        else if (m_player1->m_isDart) g_gameMode = BotGameMode::Wave;
        else if (m_player1->m_isRobot) g_gameMode = BotGameMode::Robot;
        else if (m_player1->m_isSpider) g_gameMode = BotGameMode::Spider;
        else if (m_player1->m_isSwing) g_gameMode = BotGameMode::Swing;
        else g_gameMode = BotGameMode::Cube;
        
        // Determine speed
        float spd = m_player1->m_playerSpeed;
        if (spd <= 0.8f) g_xSpeed = Phys::SPEED_SLOW / 240.0f;
        else if (spd <= 0.95f) g_xSpeed = Phys::SPEED_NORMAL / 240.0f;
        else if (spd <= 1.05f) g_xSpeed = Phys::SPEED_FAST / 240.0f;
        else if (spd <= 1.15f) g_xSpeed = Phys::SPEED_FASTER / 240.0f;
        else g_xSpeed = Phys::SPEED_FASTEST / 240.0f;
        
        // Track progress
        float prog = (g_playerX / pl->m_levelLength) * 100.0f;
        if (prog > g_bestProgress) g_bestProgress = prog;
        
        // Reload settings periodically
        if (g_frameCounter % 120 == 0) {
            g_settings.load();
        }
        
        // Bot decision
        bool click = shouldBotClick();
        
        // Apply input
        if (click != g_isHolding) {
            this->handleButton(click, 1, true);
            g_isHolding = click;
            if (click) g_totalClicks++;
        }
    }
};

// ============================================================================
// PLAYLAYER HOOK
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;
        g_totalClicks = 0;
        g_bestProgress = 0.0f;
        g_settings.load();
        
        g_overlay = BotOverlay::create();
        g_overlay->setZOrder(9999);
        addChild(g_overlay);
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        analyzeLevel(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        g_attempts++;
        g_frameCounter = 0;
        g_totalClicks = 0;
        
        if (g_isHolding) {
            GJBaseGameLayer* gj = GJBaseGameLayer::get();
            if (gj) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
        
        if (!g_levelAnalyzed) {
            analyzeLevel(this);
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        Notification::create(
            fmt::format("Bot Complete! {} clicks", g_totalClicks),
            NotificationIcon::Success
        )->show();
    }
    
    void onQuit() {
        if (g_isHolding) {
            GJBaseGameLayer* gj = GJBaseGameLayer::get();
            if (gj) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
        g_levelAnalyzed = false;
        g_overlay = nullptr;
        PlayLayer::onQuit();
    }
};

// ============================================================================
// PAUSE MENU
// ============================================================================

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        CCSize ws = CCDirector::sharedDirector()->getWinSize();
        
        CCMenu* menu = CCMenu::create();
        menu->setPosition(ccp(0, 0));
        addChild(menu, 100);
        
        // Bot toggle
        CCSprite* on1 = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        CCSprite* off1 = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        CCMenuItemToggler* t1 = CCMenuItemToggler::create(off1, on1, this, menu_selector(BotPauseLayer::onBot));
        t1->setPosition(ccp(ws.width - 30.0f, ws.height - 30.0f));
        t1->toggle(g_botEnabled);
        menu->addChild(t1);
        
        CCLabelBMFont* l1 = CCLabelBMFont::create("Bot", "bigFont.fnt");
        l1->setScale(0.35f);
        l1->setPosition(ccp(ws.width - 30.0f, ws.height - 50.0f));
        addChild(l1, 100);
        
        // Settings button
        CCSprite* settingsSprite = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        CCMenuItemSpriteExtra* settingsBtn = CCMenuItemSpriteExtra::create(
            settingsSprite,
            this,
            menu_selector(BotPauseLayer::onSettings)
        );
        settingsBtn->setPosition(ccp(ws.width - 30.0f, ws.height - 90.0f));
        settingsBtn->setScale(0.6f);
        menu->addChild(settingsBtn);
    }
    
    void onBot(CCObject* sender) {
        g_botEnabled = !g_botEnabled;
        if (!g_botEnabled && g_isHolding) {
            GJBaseGameLayer* gj = GJBaseGameLayer::get();
            if (gj) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
    }
    
    void onSettings(CCObject* sender) {
        openSettingsPopup(Mod::get());
    }
};

// ============================================================================
// KEYBOARD
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && key == KEY_F8) {
            g_botEnabled = !g_botEnabled;
            if (!g_botEnabled && g_isHolding) {
                GJBaseGameLayer* gj = GJBaseGameLayer::get();
                if (gj) gj->handleButton(false, 1, true);
                g_isHolding = false;
            }
            Notification::create(
                g_botEnabled ? "Bot: ON" : "Bot: OFF",
                g_botEnabled ? NotificationIcon::Success : NotificationIcon::Info
            )->show();
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// ============================================================================
// MOD INITIALIZATION
// ============================================================================

$on_mod(Loaded) {
    log::info("GD AutoBot loaded!");
    log::info("  F8 = Toggle Bot");
    log::info("  Settings available in pause menu");
    g_settings.load();
}
