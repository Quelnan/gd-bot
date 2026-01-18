#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
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
static float g_playerXVel = 0;
static bool g_playerOnGround = false;
static bool g_isUpsideDown = false;
static bool g_isMini = false;
static bool g_isDual = false;
static float g_xSpeed = 311.58f / 240.0f;

// Gamemode
enum class GameMode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };
static GameMode g_gameMode = GameMode::Cube;

// ============================================================================
// SETTINGS CACHE
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
        simFrames = Mod::get()->getSettingValue<int64_t>("sim-frames");
        holdDuration = Mod::get()->getSettingValue<int64_t>("hold-duration");
        hitboxScale = static_cast<float>(Mod::get()->getSettingValue<double>("hitbox-scale"));
        hazardHitboxScale = static_cast<float>(Mod::get()->getSettingValue<double>("hazard-hitbox-scale"));
        safeMargin = static_cast<float>(Mod::get()->getSettingValue<double>("safe-margin"));
        reactionDistance = static_cast<float>(Mod::get()->getSettingValue<double>("reaction-distance"));
        debugOverlay = Mod::get()->getSettingValue<bool>("debug-overlay");
        debugDraw = Mod::get()->getSettingValue<bool>("debug-draw");
    }
};

static BotSettings g_settings;

// ============================================================================
// ACCURATE HITBOX DATA
// ============================================================================

struct HitboxData {
    float offsetX = 0;
    float offsetY = 0;
    float width = 30;
    float height = 30;
    bool isTriangle = false;  // For spike collision
    float rotation = 0;
};

// Spike hitbox is actually triangular - we approximate with smaller rectangle
static const std::map<int, HitboxData> OBJECT_HITBOXES = {
    // Basic spikes (triangular - use smaller hitbox)
    {8,   {0, 0, 20, 20, true}},   // Small spike
    {39,  {0, 0, 20, 20, true}},   // Spike
    {103, {0, 0, 20, 20, true}},   // Spike variation
    
    // Larger spikes
    {392, {0, 0, 12, 12, true}},   // Tiny spike
    {9,   {0, 0, 24, 24, true}},   // Medium spike
    {61,  {0, 0, 24, 24, true}},   // Medium spike
    
    // Colored spikes
    {243, {0, 0, 20, 20, true}},
    {244, {0, 0, 20, 20, true}},
    {245, {0, 0, 20, 20, true}},
    {246, {0, 0, 20, 20, true}},
    {247, {0, 0, 20, 20, true}},
    {248, {0, 0, 20, 20, true}},
    {249, {0, 0, 20, 20, true}},
    
    // Saw blades (circular - use inscribed square)
    {363, {0, 0, 45, 45, false}},
    {364, {0, 0, 60, 60, false}},
    {365, {0, 0, 75, 75, false}},
    {366, {0, 0, 45, 45, false}},
    {367, {0, 0, 60, 60, false}},
    {368, {0, 0, 75, 75, false}},
    
    // More hazards
    {446, {0, 0, 20, 20, true}},
    {447, {0, 0, 20, 20, true}},
    
    // 2.0+ spikes
    {678, {0, 0, 24, 24, true}},
    {679, {0, 0, 24, 24, true}},
    {680, {0, 0, 24, 24, true}},
    
    // 2.1 spikes
    {1705, {0, 0, 20, 20, true}},
    {1706, {0, 0, 20, 20, true}},
    {1707, {0, 0, 20, 20, true}},
    {1708, {0, 0, 20, 20, true}},
    {1709, {0, 0, 20, 20, true}},
    {1710, {0, 0, 20, 20, true}},
    {1711, {0, 0, 20, 20, true}},
    {1712, {0, 0, 20, 20, true}},
    {1713, {0, 0, 20, 20, true}},
    {1714, {0, 0, 20, 20, true}},
    {1715, {0, 0, 20, 20, true}},
    {1716, {0, 0, 20, 20, true}},
    {1717, {0, 0, 20, 20, true}},
    {1718, {0, 0, 20, 20, true}},
};

// All hazard IDs
static const std::set<int> HAZARD_IDS = {
    8, 39, 103, 392, 9, 61,
    243, 244, 245, 246, 247, 248, 249,
    363, 364, 365, 366, 367, 368,
    446, 447, 678, 679, 680,
    1705, 1706, 1707, 1708, 1709, 1710,
    1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718,
    // Additional hazards
    88, 89, 98, 397, 398, 399,
    740, 741, 742,
    1585, 1586, 1587, 1588, 1589,
    1590, 1591, 1592, 1593, 1594
};

// Orb IDs
static const std::map<int, int> ORB_IDS = {
    {36, 0}, {84, 1}, {141, 2}, {1022, 3}, 
    {1330, 4}, {1333, 5}, {1704, 6}, {1751, 7}
};

// Pad IDs
static const std::map<int, int> PAD_IDS = {
    {35, 0}, {67, 1}, {140, 2}, {1332, 3}, {452, 4}
};

// Portal IDs
static const std::map<int, GameMode> GAMEMODE_PORTALS = {
    {12, GameMode::Cube}, {13, GameMode::Ship}, {47, GameMode::Ball},
    {111, GameMode::UFO}, {660, GameMode::Wave}, {745, GameMode::Robot},
    {1331, GameMode::Spider}, {1933, GameMode::Swing}
};

// ============================================================================
// LEVEL OBJECT STORAGE
// ============================================================================

struct LevelObj {
    int id;
    float x, y;
    float width, height;
    float rotation;
    float scaleX, scaleY;
    bool isTriangle;
    
    // Type flags
    bool isHazard = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;
    
    int subType = 0;  // For orb/pad type
    GameMode portalMode = GameMode::Cube;
    bool isGravityPortal = false;
    bool gravityUp = false;
    bool isSizePortal = false;
    bool sizeMini = false;
    bool isSpeedPortal = false;
    float speedValue = 1.0f;
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
    constexpr float GRAVITY_SHIP = 0.8f;
    constexpr float GRAVITY_BALL = 0.6f;
    constexpr float GRAVITY_UFO = 0.5f;
    
    constexpr float JUMP_VEL = 11.180032f;
    constexpr float JUMP_VEL_MINI = 9.4f;
    
    constexpr float SHIP_ACCEL = 0.8f;
    constexpr float SHIP_ACCEL_MINI = 0.6f;
    constexpr float SHIP_MAX = 8.0f;
    constexpr float SHIP_MAX_MINI = 6.0f;
    
    constexpr float UFO_BOOST = 7.0f;
    constexpr float UFO_BOOST_MINI = 5.5f;
    
    constexpr float BALL_VEL = 6.0f;
    
    constexpr float ROBOT_JUMP = 10.0f;
    constexpr float ROBOT_BOOST = 0.5f;
    constexpr float ROBOT_MAX_BOOST_TIME = 0.25f;
    
    constexpr float MAX_YVEL = 15.0f;
    
    // Speed values (units per second)
    constexpr float SPEED_SLOW = 251.16f;
    constexpr float SPEED_NORMAL = 311.58f;
    constexpr float SPEED_FAST = 387.42f;
    constexpr float SPEED_FASTER = 468.0f;
    constexpr float SPEED_FASTEST = 576.0f;
    constexpr float SPEED_SUPER = 700.0f;
    
    // Player hitbox
    constexpr float PLAYER_SIZE = 30.0f;
    constexpr float MINI_SCALE = 0.6f;
    constexpr float WAVE_SCALE = 0.5f;
}

// ============================================================================
// LEVEL ANALYZER
// ============================================================================

void analyzeLevel(PlayLayer* pl) {
    g_objects.clear();
    g_hazards.clear();
    g_orbs.clear();
    g_pads.clear();
    g_portals.clear();
    g_levelAnalyzed = false;
    
    if (!pl || !pl->m_objects) {
        log::error("Bot: No level!");
        return;
    }
    
    int hazardCount = 0, orbCount = 0, padCount = 0, portalCount = 0;
    
    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;
        
        int id = obj->m_objectID;
        
        LevelObj lo;
        lo.id = id;
        lo.x = obj->getPositionX();
        lo.y = obj->getPositionY();
        lo.rotation = obj->getRotation();
        lo.scaleX = obj->getScaleX();
        lo.scaleY = obj->getScaleY();
        
        // Get hitbox from predefined data or calculate
        if (OBJECT_HITBOXES.count(id)) {
            auto& hb = OBJECT_HITBOXES.at(id);
            lo.width = hb.width * std::abs(lo.scaleX);
            lo.height = hb.height * std::abs(lo.scaleY);
            lo.isTriangle = hb.isTriangle;
        } else {
            auto size = obj->getContentSize();
            float scale = obj->getScale();
            lo.width = size.width * scale * 0.8f;
            lo.height = size.height * scale * 0.8f;
            lo.isTriangle = false;
        }
        
        // Ensure minimum size
        lo.width = std::max(lo.width, 5.0f);
        lo.height = std::max(lo.height, 5.0f);
        
        // Categorize
        bool important = false;
        
        if (HAZARD_IDS.count(id)) {
            lo.isHazard = true;
            hazardCount++;
            important = true;
        }
        else if (ORB_IDS.count(id)) {
            lo.isOrb = true;
            lo.subType = ORB_IDS.at(id);
            orbCount++;
            important = true;
        }
        else if (PAD_IDS.count(id)) {
            lo.isPad = true;
            lo.subType = PAD_IDS.at(id);
            padCount++;
            important = true;
        }
        else if (GAMEMODE_PORTALS.count(id)) {
            lo.isPortal = true;
            lo.portalMode = GAMEMODE_PORTALS.at(id);
            portalCount++;
            important = true;
        }
        else if (id == 10) {  // Gravity down
            lo.isPortal = true;
            lo.isGravityPortal = true;
            lo.gravityUp = false;
            portalCount++;
            important = true;
        }
        else if (id == 11) {  // Gravity up
            lo.isPortal = true;
            lo.isGravityPortal = true;
            lo.gravityUp = true;
            portalCount++;
            important = true;
        }
        else if (id == 99) {  // Normal size
            lo.isPortal = true;
            lo.isSizePortal = true;
            lo.sizeMini = false;
            portalCount++;
            important = true;
        }
        else if (id == 101) {  // Mini
            lo.isPortal = true;
            lo.isSizePortal = true;
            lo.sizeMini = true;
            portalCount++;
            important = true;
        }
        else if (id == 200) { lo.isPortal = true; lo.isSpeedPortal = true; lo.speedValue = 0.7f; portalCount++; important = true; }
        else if (id == 201) { lo.isPortal = true; lo.isSpeedPortal = true; lo.speedValue = 0.9f; portalCount++; important = true; }
        else if (id == 202) { lo.isPortal = true; lo.isSpeedPortal = true; lo.speedValue = 1.0f; portalCount++; important = true; }
        else if (id == 203) { lo.isPortal = true; lo.isSpeedPortal = true; lo.speedValue = 1.1f; portalCount++; important = true; }
        else if (id == 1334) { lo.isPortal = true; lo.isSpeedPortal = true; lo.speedValue = 1.3f; portalCount++; important = true; }
        
        if (important) {
            g_objects.push_back(lo);
        }
    }
    
    // Sort by X
    std::sort(g_objects.begin(), g_objects.end(), [](const LevelObj& a, const LevelObj& b) {
        return a.x < b.x;
    });
    
    // Build pointer lists
    for (auto& obj : g_objects) {
        if (obj.isHazard) g_hazards.push_back(&obj);
        if (obj.isOrb) g_orbs.push_back(&obj);
        if (obj.isPad) g_pads.push_back(&obj);
        if (obj.isPortal) g_portals.push_back(&obj);
    }
    
    log::info("Bot: Analyzed - {} hazards, {} orbs, {} pads, {} portals",
        hazardCount, orbCount, padCount, portalCount);
    g_levelAnalyzed = true;
}

// ============================================================================
// ACCURATE COLLISION DETECTION
// ============================================================================

float getPlayerHitboxSize() {
    float size = Phys::PLAYER_SIZE;
    if (g_isMini) size *= Phys::MINI_SCALE;
    if (g_gameMode == GameMode::Wave) size *= Phys::WAVE_SCALE;
    return size * g_settings.hitboxScale;
}

bool pointInTriangle(float px, float py, float x1, float y1, float x2, float y2, float x3, float y3) {
    auto sign = [](float p1x, float p1y, float p2x, float p2y, float p3x, float p3y) {
        return (p1x - p3x) * (p2y - p3y) - (p2x - p3x) * (p1y - p3y);
    };
    
    float d1 = sign(px, py, x1, y1, x2, y2);
    float d2 = sign(px, py, x2, y2, x3, y3);
    float d3 = sign(px, py, x3, y3, x1, y1);
    
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    
    return !(hasNeg && hasPos);
}

bool checkTriangleCollision(float px, float py, float psize, const LevelObj& spike) {
    // Spike triangle points (approximate based on rotation)
    float hw = (spike.width * g_settings.hazardHitboxScale) / 2.0f;
    float hh = (spike.height * g_settings.hazardHitboxScale) / 2.0f;
    
    float rad = spike.rotation * 3.14159f / 180.0f;
    float cosR = std::cos(rad);
    float sinR = std::sin(rad);
    
    // Triangle base at bottom, point at top (default orientation)
    float x1 = -hw, y1 = -hh;  // Bottom left
    float x2 = hw, y2 = -hh;   // Bottom right
    float x3 = 0, y3 = hh;     // Top point
    
    // Rotate
    auto rotate = [&](float& x, float& y) {
        float nx = x * cosR - y * sinR;
        float ny = x * sinR + y * cosR;
        x = nx; y = ny;
    };
    rotate(x1, y1);
    rotate(x2, y2);
    rotate(x3, y3);
    
    // Translate to world position
    x1 += spike.x; y1 += spike.y;
    x2 += spike.x; y2 += spike.y;
    x3 += spike.x; y3 += spike.y;
    
    // Check player corners against triangle
    float half = psize / 2.0f + g_settings.safeMargin;
    float corners[4][2] = {
        {px - half, py - half},
        {px + half, py - half},
        {px - half, py + half},
        {px + half, py + half}
    };
    
    for (auto& c : corners) {
        if (pointInTriangle(c[0], c[1], x1, y1, x2, y2, x3, y3)) {
            return true;
        }
    }
    
    // Also check if triangle points are inside player box
    if (px - half < x1 && x1 < px + half && py - half < y1 && y1 < py + half) return true;
    if (px - half < x2 && x2 < px + half && py - half < y2 && y2 < py + half) return true;
    if (px - half < x3 && x3 < px + half && py - half < y3 && y3 < py + half) return true;
    
    return false;
}

bool checkAABBCollision(float px, float py, float psize, const LevelObj& obj) {
    float phalf = psize / 2.0f + g_settings.safeMargin;
    float ohw = (obj.width * g_settings.hazardHitboxScale) / 2.0f;
    float ohh = (obj.height * g_settings.hazardHitboxScale) / 2.0f;
    
    return !(px + phalf < obj.x - ohw ||
             px - phalf > obj.x + ohw ||
             py + phalf < obj.y - ohh ||
             py - phalf > obj.y + ohh);
}

bool willHitHazard(float px, float py) {
    float psize = getPlayerHitboxSize();
    
    for (const auto* h : g_hazards) {
        float dist = h->x - px;
        if (dist < -50 || dist > g_settings.reactionDistance) continue;
        
        if (h->isTriangle) {
            if (checkTriangleCollision(px, py, psize, *h)) return true;
        } else {
            if (checkAABBCollision(px, py, psize, *h)) return true;
        }
    }
    
    // Bounds check
    float margin = 30.0f;
    if (py < Phys::GROUND - margin || py > Phys::CEILING + margin) return true;
    
    return false;
}

// ============================================================================
// ORB/PAD/PORTAL COLLISION
// ============================================================================

LevelObj* findOrbCollision(float px, float py) {
    float psize = getPlayerHitboxSize();
    for (auto* o : g_orbs) {
        if (std::abs(o->x - px) > 50) continue;
        if (checkAABBCollision(px, py, psize * 1.5f, *o)) return o;
    }
    return nullptr;
}

LevelObj* findPadCollision(float px, float py) {
    float psize = getPlayerHitboxSize();
    for (auto* p : g_pads) {
        if (std::abs(p->x - px) > 50) continue;
        if (checkAABBCollision(px, py, psize, *p)) return p;
    }
    return nullptr;
}

LevelObj* findPortalCollision(float px, float py) {
    float psize = getPlayerHitboxSize();
    for (auto* p : g_portals) {
        if (std::abs(p->x - px) > 50) continue;
        if (checkAABBCollision(px, py, psize * 2.0f, *p)) return p;
    }
    return nullptr;
}

// ============================================================================
// PHYSICS SIMULATION
// ============================================================================

struct SimState {
    float x, y, yVel;
    bool onGround, canJump;
    bool upsideDown, mini;
    GameMode mode;
    float xSpeed;
    
    // Robot boost tracking
    bool robotBoosting;
    float robotBoostTime;
    
    // Orb cooldown
    float orbCooldown;
    int lastOrbId;
};

void simFrame(SimState& s, bool hold, float dt = 1.0f/240.0f) {
    // Update cooldowns
    if (s.orbCooldown > 0) s.orbCooldown -= dt;
    
    float gravity = Phys::GRAVITY;
    if (s.mini) gravity *= 0.8f;
    if (s.upsideDown) gravity = -gravity;
    
    float groundY = s.upsideDown ? Phys::CEILING : Phys::GROUND;
    
    switch (s.mode) {
        case GameMode::Cube: {
            s.yVel -= gravity;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_YVEL, Phys::MAX_YVEL);
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
                float jv = s.mini ? Phys::JUMP_VEL_MINI : Phys::JUMP_VEL;
                s.yVel = s.upsideDown ? -jv : jv;
                s.onGround = false;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
            break;
        }
        
        case GameMode::Ship: {
            float accel = s.mini ? Phys::SHIP_ACCEL_MINI : Phys::SHIP_ACCEL;
            float maxV = s.mini ? Phys::SHIP_MAX_MINI : Phys::SHIP_MAX;
            
            s.yVel += hold ? (s.upsideDown ? -accel : accel) : (s.upsideDown ? accel : -accel);
            s.yVel = std::clamp(s.yVel, -maxV, maxV);
            s.y += s.yVel;
            s.onGround = false;
            
            // Boundary collision
            if (s.y <= Phys::GROUND) { s.y = Phys::GROUND; s.yVel = std::max(s.yVel, 0.0f); }
            if (s.y >= Phys::CEILING) { s.y = Phys::CEILING; s.yVel = std::min(s.yVel, 0.0f); }
            break;
        }
        
        case GameMode::Ball: {
            float g = gravity * 0.6f;
            s.yVel -= g;
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
        
        case GameMode::UFO: {
            float g = gravity * 0.5f;
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
            s.y += s.yVel;
            
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
            }
            
            if (hold && s.canJump) {
                float boost = s.mini ? Phys::UFO_BOOST_MINI : Phys::UFO_BOOST;
                s.yVel = s.upsideDown ? -boost : boost;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
            break;
        }
        
        case GameMode::Wave: {
            float waveSpeed = s.xSpeed * (s.mini ? 0.7f : 1.0f);
            s.y += hold ? (s.upsideDown ? -waveSpeed : waveSpeed) : (s.upsideDown ? waveSpeed : -waveSpeed);
            s.onGround = false;
            break;
        }
        
        case GameMode::Robot: {
            if (s.robotBoosting && hold) {
                s.robotBoostTime += dt;
                if (s.robotBoostTime < Phys::ROBOT_MAX_BOOST_TIME) {
                    s.yVel += s.upsideDown ? -Phys::ROBOT_BOOST : Phys::ROBOT_BOOST;
                }
            }
            
            s.yVel -= gravity;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_YVEL, Phys::MAX_YVEL);
            s.y += s.yVel;
            
            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
                s.onGround = true;
                s.robotBoosting = false;
                s.canJump = true;
            } else {
                s.onGround = false;
            }
            
            if (hold && s.onGround && s.canJump) {
                s.yVel = s.upsideDown ? -Phys::ROBOT_JUMP : Phys::ROBOT_JUMP;
                s.robotBoosting = true;
                s.robotBoostTime = 0;
                s.canJump = false;
                s.onGround = false;
            }
            if (!hold) {
                s.robotBoosting = false;
                s.canJump = true;
            }
            break;
        }
        
        case GameMode::Spider: {
            s.yVel -= gravity;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_YVEL, Phys::MAX_YVEL);
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
        
        case GameMode::Swing: {
            float g = gravity * 0.7f;
            float swingG = hold ? -g : g;
            s.yVel += swingG * 0.8f;
            s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
            s.y += s.yVel;
            s.onGround = false;
            break;
        }
    }
    
    s.x += s.xSpeed;
    
    // Clamp Y
    s.y = std::clamp(s.y, Phys::GROUND - 30.0f, Phys::CEILING + 30.0f);
}

void handleSimInteractions(SimState& s, bool hold) {
    // Portals
    auto* portal = findPortalCollision(s.x, s.y);
    if (portal) {
        if (portal->isGravityPortal) s.upsideDown = portal->gravityUp;
        else if (portal->isSizePortal) s.mini = portal->sizeMini;
        else if (portal->isSpeedPortal) {
            if (portal->speedValue <= 0.8f) s.xSpeed = Phys::SPEED_SLOW / 240.0f;
            else if (portal->speedValue <= 0.95f) s.xSpeed = Phys::SPEED_NORMAL / 240.0f;
            else if (portal->speedValue <= 1.05f) s.xSpeed = Phys::SPEED_FAST / 240.0f;
            else if (portal->speedValue <= 1.15f) s.xSpeed = Phys::SPEED_FASTER / 240.0f;
            else s.xSpeed = Phys::SPEED_FASTEST / 240.0f;
        }
        else if (portal->isPortal) {
            s.mode = portal->portalMode;
            s.yVel *= 0.5f;
        }
    }
    
    // Pads
    auto* pad = findPadCollision(s.x, s.y);
    if (pad) {
        float boost = 0;
        switch (pad->subType) {
            case 0: boost = 12.0f; break;
            case 1: boost = 16.0f; break;
            case 2: boost = 20.0f; break;
            case 3: boost = -12.0f; break;
            case 4: s.upsideDown = !s.upsideDown; break;
        }
        if (boost != 0) {
            s.yVel = s.upsideDown ? -boost : boost;
            s.onGround = false;
        }
    }
    
    // Orbs (only when clicking)
    if (hold && s.orbCooldown <= 0) {
        auto* orb = findOrbCollision(s.x, s.y);
        if (orb && orb->id != s.lastOrbId) {
            float boost = 0;
            bool flipGrav = false;
            switch (orb->subType) {
                case 0: boost = 11.2f; break;
                case 1: boost = 14.0f; break;
                case 2: boost = 18.0f; break;
                case 3: flipGrav = true; boost = 8.0f; break;
                case 4: flipGrav = true; boost = 11.2f; break;
                case 5: break;  // Black orb
                case 6: case 7: boost = 15.0f; break;
            }
            if (flipGrav) s.upsideDown = !s.upsideDown;
            if (boost != 0) {
                s.yVel = s.upsideDown ? -boost : boost;
                s.onGround = false;
            }
            s.orbCooldown = 0.1f;
            s.lastOrbId = orb->id;
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
    base.onGround = g_playerOnGround;
    base.canJump = true;
    base.upsideDown = g_isUpsideDown;
    base.mini = g_isMini;
    base.mode = g_gameMode;
    base.xSpeed = g_xSpeed;
    base.robotBoosting = false;
    base.robotBoostTime = 0;
    base.orbCooldown = 0;
    base.lastOrbId = -1;
    
    int frames = g_settings.simFrames;
    int holdFrames = g_settings.holdDuration;
    
    // Simulate NO click
    SimState sNo = base;
    int survNo = 0;
    for (int i = 0; i < frames; i++) {
        simFrame(sNo, false);
        handleSimInteractions(sNo, false);
        if (willHitHazard(sNo.x, sNo.y)) break;
        survNo++;
    }
    
    // Simulate click
    SimState sYes = base;
    int survYes = 0;
    for (int i = 0; i < frames; i++) {
        bool hold = (i < holdFrames);
        simFrame(sYes, hold);
        handleSimInteractions(sYes, hold);
        if (willHitHazard(sYes.x, sYes.y)) break;
        survYes++;
    }
    
    // Decision
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
        auto* r = new BotOverlay();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }
    
    bool init() override {
        if (!CCNode::init()) return false;
        
        m_draw = CCDrawNode::create();
        addChild(m_draw, 10);
        
        m_lbl1 = CCLabelBMFont::create("Bot: OFF", "bigFont.fnt");
        m_lbl1->setScale(0.4f);
        m_lbl1->setAnchorPoint({0,1});
        m_lbl1->setPosition({5, 320});
        addChild(m_lbl1, 100);
        
        m_lbl2 = CCLabelBMFont::create("", "chatFont.fnt");
        m_lbl2->setScale(0.5f);
        m_lbl2->setAnchorPoint({0,1});
        m_lbl2->setPosition({5, 295});
        addChild(m_lbl2, 100);
        
        m_lbl3 = CCLabelBMFont::create("", "chatFont.fnt");
        m_lbl3->setScale(0.5f);
        m_lbl3->setAnchorPoint({0,1});
        m_lbl3->setPosition({5, 275});
        addChild(m_lbl3, 100);
        
        scheduleUpdate();
        return true;
    }
    
    void update(float dt) override {
        // Reload settings
        g_settings.debugOverlay = Mod::get()->getSettingValue<bool>("debug-overlay");
        g_settings.debugDraw = Mod::get()->getSettingValue<bool>("debug-draw");
        
        // Toggle visibility
        m_lbl1->setVisible(g_settings.debugOverlay);
        m_lbl2->setVisible(g_settings.debugOverlay);
        m_lbl3->setVisible(g_settings.debugOverlay);
        
        if (g_settings.debugOverlay) {
            if (g_botEnabled) {
                m_lbl1->setString("Bot: ON");
                m_lbl1->setColor(ccc3(0,255,0));
            } else {
                m_lbl1->setString("Bot: OFF");
                m_lbl1->setColor(ccc3(255,100,100));
            }
            
            char buf[128];
            snprintf(buf, 128, "X:%.0f Y:%.0f Vel:%.1f", g_playerX, g_playerY, g_playerYVel);
            m_lbl2->setString(buf);
            
            const char* modeNames[] = {"Cube","Ship","Ball","UFO","Wave","Robot","Spider","Swing"};
            snprintf(buf, 128, "%s %s Clicks:%d Best:%.1f%%",
                modeNames[(int)g_gameMode],
                g_playerOnGround ? "[G]" : "[A]",
                g_totalClicks,
                g_bestProgress);
            m_lbl3->setString(buf);
        }
        
        // Debug draw
        m_draw->clear();
        if (!g_settings.debugDraw || !g_levelAnalyzed) return;
        
        // Player
        float psize = getPlayerHitboxSize();
        m_draw->drawRect(
            ccp(g_playerX - psize/2, g_playerY - psize/2),
            ccp(g_playerX + psize/2, g_playerY + psize/2),
            ccc4f(1,1,1,0.3f), 1.0f, ccc4f(1,1,1,0.8f));
        
        // Hazards
        for (auto* h : g_hazards) {
            if (h->x < g_playerX - 100 || h->x > g_playerX + 400) continue;
            
            float hw = (h->width * g_settings.hazardHitboxScale) / 2;
            float hh = (h->height * g_settings.hazardHitboxScale) / 2;
            
            m_draw->drawRect(
                ccp(h->x - hw, h->y - hh),
                ccp(h->x + hw, h->y + hh),
                ccc4f(1,0,0,0.2f), 1.0f, ccc4f(1,0,0,0.6f));
        }
        
        // Orbs
        for (auto* o : g_orbs) {
            if (o->x < g_playerX - 100 || o->x > g_playerX + 300) continue;
            m_draw->drawDot(ccp(o->x, o->y), 8, ccc4f(1,1,0,0.6f));
        }
        
        // Pads
        for (auto* p : g_pads) {
            if (p->x < g_playerX - 100 || p->x > g_playerX + 300) continue;
            m_draw->drawDot(ccp(p->x, p->y), 6, ccc4f(1,0,1,0.6f));
        }
        
        // Paths
        SimState sNo = {g_playerX, g_playerY, g_playerYVel, g_playerOnGround, true,
                        g_isUpsideDown, g_isMini, g_gameMode, g_xSpeed, false, 0, 0, -1};
        for (int i = 0; i < 50; i++) {
            float ox = sNo.x, oy = sNo.y;
            simFrame(sNo, false);
            float alpha = 0.7f * (1 - i/50.0f);
            m_draw->drawSegment(ccp(ox,oy), ccp(sNo.x,sNo.y), 1.5f, ccc4f(1,0.3f,0.3f,alpha));
            if (willHitHazard(sNo.x, sNo.y)) break;
        }
        
        SimState sYes = {g_playerX, g_playerY, g_playerYVel, g_playerOnGround, true,
                         g_isUpsideDown, g_isMini, g_gameMode, g_xSpeed, false, 0, 0, -1};
        for (int i = 0; i < 50; i++) {
            float ox = sYes.x, oy = sYes.y;
            simFrame(sYes, i < g_settings.holdDuration);
            float alpha = 0.7f * (1 - i/50.0f);
            m_draw->drawSegment(ccp(ox,oy), ccp(sYes.x,sYes.y), 1.5f, ccc4f(0.3f,1,0.3f,alpha));
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
        
        auto* pl = PlayLayer::get();
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
        
        // Gamemode
        if (m_player1->m_isShip) g_gameMode = GameMode::Ship;
        else if (m_player1->m_isBall) g_gameMode = GameMode::Ball;
        else if (m_player1->m_isBird) g_gameMode = GameMode::UFO;
        else if (m_player1->m_isDart) g_gameMode = GameMode::Wave;
        else if (m_player1->m_isRobot) g_gameMode = GameMode::Robot;
        else if (m_player1->m_isSpider) g_gameMode = GameMode::Spider;
        else if (m_player1->m_isSwing) g_gameMode = GameMode::Swing;
        else g_gameMode = GameMode::Cube;
        
        // Speed
        float spd = m_player1->m_playerSpeed;
        if (spd <= 0.8f) g_xSpeed = Phys::SPEED_SLOW / 240.0f;
        else if (spd <= 0.95f) g_xSpeed = Phys::SPEED_NORMAL / 240.0f;
        else if (spd <= 1.05f) g_xSpeed = Phys::SPEED_FAST / 240.0f;
        else if (spd <= 1.15f) g_xSpeed = Phys::SPEED_FASTER / 240.0f;
        else g_xSpeed = Phys::SPEED_FASTEST / 240.0f;
        
        // Progress tracking
        float progress = (g_playerX / pl->m_levelLength) * 100.0f;
        if (progress > g_bestProgress) g_bestProgress = progress;
        
        // Reload settings periodically
        if (g_frameCounter % 60 == 0) {
            g_settings.load();
        }
        
        // Bot decision
        bool click = shouldBotClick();
        
        if (click != g_isHolding) {
            this->handleButton(click, 1, true);
            g_isHolding = click;
            
            if (click) {
                g_totalClicks++;
                if (g_frameCounter % 30 == 0) {
                    log::info("Bot: Click #{} at ({:.0f},{:.0f})", g_totalClicks, g_playerX, g_playerY);
                }
            }
        }
    }
};

// ============================================================================
// PLAYLAYER HOOK
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        log::info("Bot: Level init");
        
        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;
        g_totalClicks = 0;
        g_bestProgress = 0;
        
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
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
        
        if (!g_levelAnalyzed) analyzeLevel(this);
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        log::info("Bot: COMPLETE! {} clicks, best {:.1f}%", g_totalClicks, g_bestProgress);
        Notification::create(fmt::format("Bot Complete!\n{} clicks", g_totalClicks), NotificationIcon::Success)->show();
    }
    
    void onQuit() {
        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
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
        
        auto ws = CCDirector::sharedDirector()->getWinSize();
        auto* menu = CCMenu::create();
        menu->setPosition({0,0});
        addChild(menu, 100);
        
        // Bot toggle
        auto* on1 = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* off1 = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* t1 = CCMenuItemToggler::create(off1, on1, this, menu_selector(BotPauseLayer::onBot));
        t1->setPosition({ws.width-30, ws.height-30});
        t1->toggle(g_botEnabled);
        menu->addChild(t1);
        
        auto* l1 = CCLabelBMFont::create("Bot", "bigFont.fnt");
        l1->setScale(0.35f);
        l1->setPosition({ws.width-30, ws.height-50});
        addChild(l1, 100);
        
        // Settings button
        auto* settingsBtn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"),
            this,
            menu_selector(BotPauseLayer::onSettings)
        );
        settingsBtn->setPosition({ws.width-30, ws.height-90});
        settingsBtn->setScale(0.6f);
        menu->addChild(settingsBtn);
    }
    
    void onBot(CCObject*) {
        g_botEnabled = !g_botEnabled;
        log::info("Bot: {}", g_botEnabled ? "ON" : "OFF");
        if (!g_botEnabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
    }
    
    void onSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};

// ============================================================================
// KEYBOARD
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            if (key == KEY_F8) {
                g_botEnabled = !g_botEnabled;
                log::info("Bot: {} (F8)", g_botEnabled ? "ON" : "OFF");
                if (!g_botEnabled && g_isHolding) {
                    if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
                    g_isHolding = false;
                }
                Notification::create(g_botEnabled ? "Bot: ON" : "Bot: OFF",
                    g_botEnabled ? NotificationIcon::Success : NotificationIcon::Info)->show();
                return true;
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// ============================================================================
// MOD INIT
// ============================================================================

$on_mod(Loaded) {
    log::info("=========================================");
    log::info("  GD AutoBot Loaded!");
    log::info("  F8 = Toggle Bot");
    log::info("  Settings in Pause Menu or Geode Menu");
    log::info("=========================================");
    
    g_settings.load();
    
    // Listen for setting changes
    listenForSettingChanges("sim-frames", [](int64_t v) { g_settings.simFrames = v; });
    listenForSettingChanges("hold-duration", [](int64_t v) { g_settings.holdDuration = v; });
    listenForSettingChanges("hitbox-scale", [](double v) { g_settings.hitboxScale = v; });
    listenForSettingChanges("hazard-hitbox-scale", [](double v) { g_settings.hazardHitboxScale = v; });
    listenForSettingChanges("safe-margin", [](double v) { g_settings.safeMargin = v; });
    listenForSettingChanges("reaction-distance", [](double v) { g_settings.reactionDistance = v; });
    listenForSettingChanges("debug-overlay", [](bool v) { g_settings.debugOverlay = v; });
    listenForSettingChanges("debug-draw", [](bool v) { g_settings.debugDraw = v; });
}
