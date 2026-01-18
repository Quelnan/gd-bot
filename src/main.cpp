#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

// ============================================================================
// GLOBAL STATE
// ============================================================================

static bool g_botEnabled = false;
static bool g_debugDraw = false;
static bool g_isHolding = false;
static bool g_levelAnalyzed = false;
static int g_frameCounter = 0;
static int g_totalClicks = 0;
static int g_totalAttempts = 0;
static float g_bestProgress = 0;

// ============================================================================
// ENUMS (renamed to avoid conflicts with GD's enums)
// ============================================================================

enum class BotGameMode {
    Cube,
    Ship,
    Ball,
    UFO,
    Wave,
    Robot,
    Spider,
    Swing
};

enum class BotSpeed {
    Slow,      // 0.7x
    Normal,    // 0.9x
    Fast,      // 1.0x
    Faster,    // 1.1x
    Fastest,   // 1.3x
    SuperFast  // 1.6x+
};

// ============================================================================
// PLAYER STATE - Stores simulation state
// ============================================================================

struct PlayerState {
    float x = 0;
    float y = 105.0f;
    float yVel = 0;
    float rotation = 0;
    
    BotGameMode gameMode = BotGameMode::Cube;
    BotSpeed speed = BotSpeed::Normal;
    
    bool isUpsideDown = false;
    bool isMini = false;
    bool isOnGround = true;
    bool canJump = true;
    bool isDead = false;
    
    float orbCooldown = 0;
    int lastOrbID = -1;
    
    // Robot-specific
    bool isRobotBoosting = false;
    float robotBoostTime = 0;
    
    // Spider-specific
    bool hasSpiderFlipped = false;
    
    PlayerState copy() const {
        return *this;
    }
};

// ============================================================================
// LEVEL OBJECT - Represents hazards, orbs, pads, portals
// ============================================================================

struct LevelObject {
    int objectID = 0;
    float x = 0;
    float y = 0;
    float width = 30;
    float height = 30;
    float rotation = 0;
    
    bool isHazard = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;
    bool isSolid = false;
    
    // Orb/Pad type: 0=yellow, 1=pink, 2=red, 3=blue, 4=green, 5=black, 6=dash
    int interactionType = 0;
    
    // Portal properties
    BotGameMode portalGameMode = BotGameMode::Cube;
    BotSpeed portalSpeed = BotSpeed::Normal;
    bool isGravityPortal = false;
    bool gravityGoesUp = false;
    bool isSizePortal = false;
    bool sizeIsMini = false;
    bool isSpeedPortal = false;
};

// ============================================================================
// GLOBAL LEVEL DATA
// ============================================================================

static std::vector<LevelObject> g_levelObjects;
static PlayerState g_currentPlayerState;

// ============================================================================
// OBJECT ID DEFINITIONS
// ============================================================================

// Hazard IDs (spikes, saws, etc)
static const std::set<int> HAZARD_IDS = {
    8, 39, 103, 392, 9, 61,
    243, 244, 245, 246, 247, 248, 249,
    363, 364, 365, 366, 367, 368,
    446, 447,
    678, 679, 680,
    1705, 1706, 1707, 1708, 1709, 1710,
    1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718
};

// Orb IDs and their types
static const std::map<int, int> ORB_IDS = {
    {36, 0},    // Yellow orb
    {84, 1},    // Pink orb
    {141, 2},   // Red orb
    {1022, 3},  // Blue orb
    {1330, 4},  // Green orb
    {1333, 5},  // Black orb
    {1704, 6},  // Dash orb green
    {1751, 7}   // Dash orb magenta
};

// Pad IDs and their types
static const std::map<int, int> PAD_IDS = {
    {35, 0},    // Yellow pad
    {67, 1},    // Pink pad
    {140, 2},   // Red pad
    {1332, 3},  // Blue pad
    {452, 4}    // Spider pad
};

// Portal IDs
static const std::map<int, BotGameMode> GAMEMODE_PORTAL_IDS = {
    {12, BotGameMode::Cube},
    {13, BotGameMode::Ship},
    {47, BotGameMode::Ball},
    {111, BotGameMode::UFO},
    {660, BotGameMode::Wave},
    {745, BotGameMode::Robot},
    {1331, BotGameMode::Spider},
    {1933, BotGameMode::Swing}
};

static const std::map<int, BotSpeed> SPEED_PORTAL_IDS = {
    {200, BotSpeed::Slow},
    {201, BotSpeed::Normal},
    {202, BotSpeed::Fast},
    {203, BotSpeed::Faster},
    {1334, BotSpeed::Fastest}
};

// ============================================================================
// PHYSICS ENGINE
// ============================================================================

class PhysicsEngine {
public:
    // Physics constants
    static constexpr float GROUND_Y = 105.0f;
    static constexpr float CEILING_Y = 2085.0f;
    static constexpr float PLAYER_SIZE = 30.0f;
    static constexpr float MINI_SCALE = 0.6f;
    
    // Gravity values per gamemode
    static constexpr float GRAVITY_CUBE = 0.958199f;
    static constexpr float GRAVITY_SHIP = 0.8f;
    static constexpr float GRAVITY_BALL = 0.6f;
    static constexpr float GRAVITY_UFO = 0.5f;
    static constexpr float GRAVITY_ROBOT = 0.958199f;
    static constexpr float GRAVITY_SPIDER = 0.6f;
    static constexpr float GRAVITY_SWING = 0.7f;
    
    // Jump velocities
    static constexpr float JUMP_VELOCITY = 11.180032f;
    static constexpr float MINI_JUMP_VELOCITY = 9.4f;
    static constexpr float UFO_BOOST = 7.0f;
    static constexpr float SHIP_ACCEL = 0.8f;
    static constexpr float MINI_SHIP_ACCEL = 0.6f;
    
    // Get horizontal speed based on speed portal
    static float getHorizontalSpeed(BotSpeed speed) {
        switch (speed) {
            case BotSpeed::Slow:      return 251.16f;
            case BotSpeed::Normal:    return 311.58f;
            case BotSpeed::Fast:      return 387.42f;
            case BotSpeed::Faster:    return 468.0f;
            case BotSpeed::Fastest:   return 576.0f;
            case BotSpeed::SuperFast: return 700.0f;
        }
        return 311.58f;
    }
    
    // Get gravity for gamemode
    static float getGravity(BotGameMode mode, bool isMini) {
        float g;
        switch (mode) {
            case BotGameMode::Cube:   g = GRAVITY_CUBE; break;
            case BotGameMode::Ship:   g = GRAVITY_SHIP; break;
            case BotGameMode::Ball:   g = GRAVITY_BALL; break;
            case BotGameMode::UFO:    g = GRAVITY_UFO; break;
            case BotGameMode::Wave:   g = 0.0f; break;
            case BotGameMode::Robot:  g = GRAVITY_ROBOT; break;
            case BotGameMode::Spider: g = GRAVITY_SPIDER; break;
            case BotGameMode::Swing:  g = GRAVITY_SWING; break;
            default: g = GRAVITY_CUBE;
        }
        return isMini ? g * 0.8f : g;
    }
    
    // Get jump velocity for gamemode
    static float getJumpVelocity(BotGameMode mode, bool isMini) {
        if (isMini) {
            switch (mode) {
                case BotGameMode::Cube:  return 9.4f;
                case BotGameMode::Robot: return 7.5f;
                default: return MINI_JUMP_VELOCITY;
            }
        }
        switch (mode) {
            case BotGameMode::Cube:  return JUMP_VELOCITY;
            case BotGameMode::Robot: return 10.0f;
            default: return JUMP_VELOCITY;
        }
    }
    
    // Main simulation function - simulates one physics frame
    static void simulateFrame(PlayerState& state, bool isHolding, float deltaTime = 1.0f/240.0f) {
        // Calculate horizontal movement
        float xSpeed = getHorizontalSpeed(state.speed) * deltaTime;
        
        // Update cooldowns
        if (state.orbCooldown > 0) {
            state.orbCooldown -= deltaTime;
        }
        
        // Get gravity (inverted if upside down)
        float gravity = getGravity(state.gameMode, state.isMini);
        if (state.isUpsideDown) {
            gravity = -gravity;
        }
        
        // Ground Y position depends on gravity direction
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        // Simulate based on gamemode
        switch (state.gameMode) {
            case BotGameMode::Cube:
                simulateCube(state, isHolding, gravity, groundY);
                break;
                
            case BotGameMode::Ship:
                simulateShip(state, isHolding, gravity);
                break;
                
            case BotGameMode::Ball:
                simulateBall(state, isHolding, gravity, groundY);
                break;
                
            case BotGameMode::UFO:
                simulateUFO(state, isHolding, gravity, groundY);
                break;
                
            case BotGameMode::Wave:
                simulateWave(state, isHolding, xSpeed);
                break;
                
            case BotGameMode::Robot:
                simulateRobot(state, isHolding, gravity, groundY, deltaTime);
                break;
                
            case BotGameMode::Spider:
                simulateSpider(state, isHolding, gravity, groundY);
                break;
                
            case BotGameMode::Swing:
                simulateSwing(state, isHolding, gravity);
                break;
        }
        
        // Move forward
        state.x += xSpeed;
        
        // Clamp to level bounds
        state.y = std::clamp(state.y, GROUND_Y - 50.0f, CEILING_Y + 50.0f);
    }
    
private:
    static void simulateCube(PlayerState& state, bool isHolding, float gravity, float groundY) {
        // Apply gravity
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
        
        // Apply velocity
        state.y += state.yVel;
        
        // Ground collision
        bool hitGround = state.isUpsideDown ? (state.y >= groundY) : (state.y <= groundY);
        if (hitGround) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        // Jump
        if (isHolding && state.isOnGround && state.canJump) {
            float jumpVel = getJumpVelocity(BotGameMode::Cube, state.isMini);
            state.yVel = state.isUpsideDown ? -jumpVel : jumpVel;
            state.isOnGround = false;
            state.canJump = false;
        }
        
        // Reset jump ability when not holding
        if (!isHolding) {
            state.canJump = true;
        }
        
        // Update rotation
        if (!state.isOnGround) {
            state.rotation += state.isUpsideDown ? -7.5f : 7.5f;
        } else {
            state.rotation = std::round(state.rotation / 90.0f) * 90.0f;
        }
    }
    
    static void simulateShip(PlayerState& state, bool isHolding, float gravity) {
        float accel = state.isMini ? MINI_SHIP_ACCEL : SHIP_ACCEL;
        float maxVel = state.isMini ? 6.0f : 8.0f;
        
        if (isHolding) {
            state.yVel += state.isUpsideDown ? -accel : accel;
        } else {
            state.yVel += state.isUpsideDown ? accel : -accel;
        }
        
        state.yVel = std::clamp(state.yVel, -maxVel, maxVel);
        state.y += state.yVel;
        
        // Ship rotation follows velocity
        state.rotation = state.yVel * 2.0f;
        state.isOnGround = false;
    }
    
    static void simulateBall(PlayerState& state, bool isHolding, float gravity, float groundY) {
        // Apply reduced gravity
        state.yVel -= gravity * 0.6f;
        state.yVel = std::clamp(state.yVel, -12.0f, 12.0f);
        state.y += state.yVel;
        
        // Ground collision
        bool hitGround = state.isUpsideDown ? (state.y >= groundY) : (state.y <= groundY);
        if (hitGround) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
        } else {
            state.isOnGround = false;
        }
        
        // Click to reverse gravity
        if (isHolding && state.isOnGround && state.canJump) {
            state.isUpsideDown = !state.isUpsideDown;
            state.yVel = state.isUpsideDown ? -6.0f : 6.0f;
            state.canJump = false;
        }
        
        if (!isHolding) {
            state.canJump = true;
        }
        
        // Ball always rotates
        state.rotation += state.isUpsideDown ? -10.0f : 10.0f;
    }
    
    static void simulateUFO(PlayerState& state, bool isHolding, float gravity, float groundY) {
        // Apply reduced gravity
        state.yVel -= gravity * 0.5f;
        state.yVel = std::clamp(state.yVel, -8.0f, 8.0f);
        state.y += state.yVel;
        
        // Ground collision
        bool hitGround = state.isUpsideDown ? (state.y >= groundY) : (state.y <= groundY);
        if (hitGround) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
        } else {
            state.isOnGround = false;
        }
        
        // Each click gives a boost
        if (isHolding && state.canJump) {
            float boost = state.isMini ? 5.5f : UFO_BOOST;
            state.yVel = state.isUpsideDown ? -boost : boost;
            state.canJump = false;
        }
        
        if (!isHolding) {
            state.canJump = true;
        }
    }
    
    static void simulateWave(PlayerState& state, bool isHolding, float xSpeed) {
        // Wave moves diagonally
        float waveMultiplier = state.isMini ? 0.7f : 1.0f;
        float diagonalSpeed = xSpeed * waveMultiplier;
        
        if (isHolding) {
            state.y += state.isUpsideDown ? -diagonalSpeed : diagonalSpeed;
        } else {
            state.y += state.isUpsideDown ? diagonalSpeed : -diagonalSpeed;
        }
        
        // Wave rotation
        if (isHolding) {
            state.rotation = state.isUpsideDown ? -45.0f : 45.0f;
        } else {
            state.rotation = state.isUpsideDown ? 45.0f : -45.0f;
        }
        
        state.isOnGround = false;
    }
    
    static void simulateRobot(PlayerState& state, bool isHolding, float gravity, float groundY, float dt) {
        // Robot can hold to jump higher
        if (state.isRobotBoosting && isHolding) {
            state.robotBoostTime += dt;
            float maxBoostTime = 0.25f;
            if (state.robotBoostTime < maxBoostTime) {
                float boostAccel = state.isUpsideDown ? -0.5f : 0.5f;
                state.yVel += boostAccel;
            }
        }
        
        // Apply gravity
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
        state.y += state.yVel;
        
        // Ground collision
        bool hitGround = state.isUpsideDown ? (state.y >= groundY) : (state.y <= groundY);
        if (hitGround) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
            state.isRobotBoosting = false;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        // Start jump
        if (isHolding && state.isOnGround && state.canJump) {
            float jumpVel = getJumpVelocity(BotGameMode::Robot, state.isMini);
            state.yVel = state.isUpsideDown ? -jumpVel : jumpVel;
            state.isRobotBoosting = true;
            state.robotBoostTime = 0;
            state.canJump = false;
            state.isOnGround = false;
        }
        
        // Stop boosting when released
        if (!isHolding) {
            state.isRobotBoosting = false;
            state.canJump = true;
        }
    }
    
    static void simulateSpider(PlayerState& state, bool isHolding, float gravity, float groundY) {
        // Apply gravity
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
        state.y += state.yVel;
        
        // Ground collision
        bool hitGround = state.isUpsideDown ? (state.y >= groundY) : (state.y <= groundY);
        if (hitGround) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
            state.hasSpiderFlipped = false;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        // Spider teleports to opposite surface
        if (isHolding && state.isOnGround && state.canJump && !state.hasSpiderFlipped) {
            state.isUpsideDown = !state.isUpsideDown;
            // Teleport to opposite surface
            state.y = state.isUpsideDown ? CEILING_Y : GROUND_Y;
            state.yVel = 0;
            state.hasSpiderFlipped = true;
            state.canJump = false;
        }
        
        if (!isHolding) {
            state.canJump = true;
        }
    }
    
    static void simulateSwing(PlayerState& state, bool isHolding, float gravity) {
        // Swing copter - gravity direction depends on holding
        float swingGravity = isHolding ? -gravity : gravity;
        
        state.yVel += swingGravity * 0.8f;
        state.yVel = std::clamp(state.yVel, -8.0f, 8.0f);
        state.y += state.yVel;
        
        state.rotation = state.yVel * 3.0f;
        state.isOnGround = false;
    }
};

// ============================================================================
// COLLISION DETECTION
// ============================================================================

class CollisionSystem {
public:
    // Check if player hitbox collides with an object
    static bool checkCollision(const PlayerState& state, const LevelObject& obj) {
        // Calculate player hitbox size
        float playerSize = state.isMini ? 
            PhysicsEngine::PLAYER_SIZE * PhysicsEngine::MINI_SCALE : 
            PhysicsEngine::PLAYER_SIZE;
        
        // Wave has smaller hitbox
        if (state.gameMode == BotGameMode::Wave) {
            playerSize *= 0.6f;
        }
        
        float halfPlayer = playerSize / 2.0f;
        
        // AABB collision
        float playerLeft = state.x - halfPlayer;
        float playerRight = state.x + halfPlayer;
        float playerBottom = state.y - halfPlayer;
        float playerTop = state.y + halfPlayer;
        
        float objLeft = obj.x - obj.width / 2.0f;
        float objRight = obj.x + obj.width / 2.0f;
        float objBottom = obj.y - obj.height / 2.0f;
        float objTop = obj.y + obj.height / 2.0f;
        
        // Check overlap
        bool collides = !(playerRight < objLeft || playerLeft > objRight ||
                         playerTop < objBottom || playerBottom > objTop);
        
        return collides;
    }
    
    // Check if player will die at current position
    static bool willPlayerDie(const PlayerState& state) {
        // Check level bounds
        if (state.y < 60.0f || state.y > 2080.0f) {
            return true;
        }
        
        // Check collision with hazards
        for (const auto& obj : g_levelObjects) {
            if (obj.isHazard) {
                // Only check nearby objects for performance
                if (std::abs(obj.x - state.x) > 100.0f) continue;
                
                if (checkCollision(state, obj)) {
                    return true;
                }
            }
        }
        
        return false;
    }
    
    // Find orb collision
    static const LevelObject* findOrbCollision(const PlayerState& state) {
        for (const auto& obj : g_levelObjects) {
            if (obj.isOrb && std::abs(obj.x - state.x) < 50.0f) {
                if (checkCollision(state, obj)) {
                    return &obj;
                }
            }
        }
        return nullptr;
    }
    
    // Find pad collision
    static const LevelObject* findPadCollision(const PlayerState& state) {
        for (const auto& obj : g_levelObjects) {
            if (obj.isPad && std::abs(obj.x - state.x) < 50.0f) {
                if (checkCollision(state, obj)) {
                    return &obj;
                }
            }
        }
        return nullptr;
    }
    
    // Find portal collision
    static const LevelObject* findPortalCollision(const PlayerState& state) {
        for (const auto& obj : g_levelObjects) {
            if (obj.isPortal && std::abs(obj.x - state.x) < 50.0f) {
                if (checkCollision(state, obj)) {
                    return &obj;
                }
            }
        }
        return nullptr;
    }
};

// ============================================================================
// INTERACTION HANDLER
// ============================================================================

class InteractionHandler {
public:
    // Handle all object interactions during simulation
    static void handleInteractions(PlayerState& state, bool isHolding) {
        handlePortals(state);
        handlePads(state);
        if (isHolding) {
            handleOrbs(state);
        }
    }
    
private:
    static void handlePortals(PlayerState& state) {
        const LevelObject* portal = CollisionSystem::findPortalCollision(state);
        if (!portal) return;
        
        if (portal->isGravityPortal) {
            state.isUpsideDown = portal->gravityGoesUp;
        }
        else if (portal->isSizePortal) {
            state.isMini = portal->sizeIsMini;
        }
        else if (portal->isSpeedPortal) {
            state.speed = portal->portalSpeed;
        }
        else if (portal->isPortal) {
            state.gameMode = portal->portalGameMode;
            // Reset some state on gamemode change
            state.yVel *= 0.5f;
        }
    }
    
    static void handlePads(PlayerState& state) {
        const LevelObject* pad = CollisionSystem::findPadCollision(state);
        if (!pad) return;
        
        float boost = 0;
        switch (pad->interactionType) {
            case 0: boost = 12.0f; break;  // Yellow
            case 1: boost = 16.0f; break;  // Pink
            case 2: boost = 20.0f; break;  // Red
            case 3: boost = -12.0f; break; // Blue
            case 4: // Spider pad
                state.isUpsideDown = !state.isUpsideDown;
                return;
        }
        
        if (state.isUpsideDown) boost = -boost;
        state.yVel = boost;
        state.isOnGround = false;
    }
    
    static void handleOrbs(PlayerState& state) {
        if (state.orbCooldown > 0) return;
        
        const LevelObject* orb = CollisionSystem::findOrbCollision(state);
        if (!orb) return;
        if (orb->objectID == state.lastOrbID) return;
        
        float boost = 0;
        switch (orb->interactionType) {
            case 0: boost = 11.2f; break;  // Yellow
            case 1: boost = 14.0f; break;  // Pink
            case 2: boost = 18.0f; break;  // Red
            case 3: // Blue - reverses gravity
                state.isUpsideDown = !state.isUpsideDown;
                boost = 8.0f;
                break;
            case 4: // Green - jump + reverse
                state.isUpsideDown = !state.isUpsideDown;
                boost = 11.2f;
                break;
            case 5: // Black - no effect
                return;
            case 6: // Dash green
            case 7: // Dash magenta
                boost = 15.0f;
                break;
        }
        
        // Apply upside down to boost
        if (state.isUpsideDown && orb->interactionType != 3 && orb->interactionType != 4) {
            boost = -boost;
        }
        
        state.yVel = boost;
        state.orbCooldown = 0.1f;
        state.lastOrbID = orb->objectID;
        state.isOnGround = false;
    }
};

// ============================================================================
// LEVEL ANALYZER
// ============================================================================

class LevelAnalyzer {
public:
    static void analyzeLevel(PlayLayer* playLayer) {
        g_levelObjects.clear();
        g_levelAnalyzed = false;
        
        if (!playLayer) {
            log::error("AutoBot: PlayLayer is null!");
            return;
        }
        
        auto* objects = playLayer->m_objects;
        if (!objects) {
            log::error("AutoBot: No objects in level!");
            return;
        }
        
        int hazardCount = 0;
        int orbCount = 0;
        int padCount = 0;
        int portalCount = 0;
        
        for (int i = 0; i < objects->count(); i++) {
            auto* gameObj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!gameObj) continue;
            
            int objID = gameObj->m_objectID;
            
            LevelObject levelObj;
            levelObj.objectID = objID;
            levelObj.x = gameObj->getPositionX();
            levelObj.y = gameObj->getPositionY();
            levelObj.rotation = gameObj->getRotation();
            
            // Get hitbox size
            auto contentSize = gameObj->getContentSize();
            float scale = gameObj->getScale();
            levelObj.width = contentSize.width * scale * 0.8f;
            levelObj.height = contentSize.height * scale * 0.8f;
            
            // Categorize object
            bool isImportant = false;
            
            // Check hazards
            if (HAZARD_IDS.count(objID)) {
                levelObj.isHazard = true;
                hazardCount++;
                isImportant = true;
            }
            // Check orbs
            else if (ORB_IDS.count(objID)) {
                levelObj.isOrb = true;
                levelObj.interactionType = ORB_IDS.at(objID);
                orbCount++;
                isImportant = true;
            }
            // Check pads
            else if (PAD_IDS.count(objID)) {
                levelObj.isPad = true;
                levelObj.interactionType = PAD_IDS.at(objID);
                padCount++;
                isImportant = true;
            }
            // Check gamemode portals
            else if (GAMEMODE_PORTAL_IDS.count(objID)) {
                levelObj.isPortal = true;
                levelObj.portalGameMode = GAMEMODE_PORTAL_IDS.at(objID);
                portalCount++;
                isImportant = true;
            }
            // Check speed portals
            else if (SPEED_PORTAL_IDS.count(objID)) {
                levelObj.isPortal = true;
                levelObj.isSpeedPortal = true;
                levelObj.portalSpeed = SPEED_PORTAL_IDS.at(objID);
                portalCount++;
                isImportant = true;
            }
            // Check gravity portals
            else if (objID == 10) { // Gravity down
                levelObj.isPortal = true;
                levelObj.isGravityPortal = true;
                levelObj.gravityGoesUp = false;
                portalCount++;
                isImportant = true;
            }
            else if (objID == 11) { // Gravity up
                levelObj.isPortal = true;
                levelObj.isGravityPortal = true;
                levelObj.gravityGoesUp = true;
                portalCount++;
                isImportant = true;
            }
            // Check size portals
            else if (objID == 99) { // Normal size
                levelObj.isPortal = true;
                levelObj.isSizePortal = true;
                levelObj.sizeIsMini = false;
                portalCount++;
                isImportant = true;
            }
            else if (objID == 101) { // Mini
                levelObj.isPortal = true;
                levelObj.isSizePortal = true;
                levelObj.sizeIsMini = true;
                portalCount++;
                isImportant = true;
            }
            
            if (isImportant) {
                g_levelObjects.push_back(levelObj);
            }
        }
        
        // Sort by X position for faster lookup
        std::sort(g_levelObjects.begin(), g_levelObjects.end(),
            [](const LevelObject& a, const LevelObject& b) {
                return a.x < b.x;
            });
        
        log::info("AutoBot: Analyzed level - {} hazards, {} orbs, {} pads, {} portals",
            hazardCount, orbCount, padCount, portalCount);
        
        g_levelAnalyzed = true;
    }
};

// ============================================================================
// BOT BRAIN - Decision making
// ============================================================================

class BotBrain {
public:
    // Main decision function: should we click this frame?
    static bool shouldClick(const PlayerState& currentState) {
        // Simulate both scenarios
        int surviveWithoutClick = simulateAndCountSurvival(currentState, false);
        int surviveWithClick = simulateAndCountSurvival(currentState, true);
        
        // Log decision periodically
        if (g_frameCounter % 60 == 0) {
            log::info("Bot decision at x={:.0f}: noClick={} click={} onGround={}",
                currentState.x, surviveWithoutClick, surviveWithClick, currentState.onGround);
        }
        
        // Click if:
        // 1. Clicking survives longer than not clicking
        // 2. We're about to die and clicking might save us
        // 3. We're on ground and there's a hazard ahead we need to jump over
        
        if (surviveWithClick > surviveWithoutClick) {
            return true;
        }
        
        // Emergency click if we're about to die
        if (surviveWithoutClick < 5 && surviveWithClick >= surviveWithoutClick) {
            return true;
        }
        
        // Proactive jump: check if there's a hazard we need to jump over
        if (currentState.isOnGround && currentState.gameMode == BotGameMode::Cube) {
            for (const auto& obj : g_levelObjects) {
                if (obj.isHazard && obj.x > currentState.x && obj.x < currentState.x + 200) {
                    // Ground-level hazard
                    if (obj.y < 200) {
                        float distance = obj.x - currentState.x;
                        if (distance > 40 && distance < 150) {
                            return true;
                        }
                    }
                }
            }
        }
        
        return false;
    }
    
private:
    // Simulate X frames and return how many we survive
    static int simulateAndCountSurvival(const PlayerState& startState, bool doClick) {
        PlayerState simState = startState;
        
        const int simulationFrames = 50;
        const int holdDuration = 12; // How long to hold when clicking
        
        for (int frame = 0; frame < simulationFrames; frame++) {
            // Determine if holding this frame
            bool isHolding = doClick && (frame < holdDuration);
            
            // Simulate physics
            PhysicsEngine::simulateFrame(simState, isHolding);
            
            // Handle interactions
            InteractionHandler::handleInteractions(simState, isHolding);
            
            // Check if dead
            if (CollisionSystem::willPlayerDie(simState)) {
                return frame;
            }
        }
        
        return simulationFrames;
    }
};

// ============================================================================
// PLAYER STATE SYNC - Get state from actual game
// ============================================================================

class PlayerStateSync {
public:
    static void syncFromGame(PlayerObject* player) {
        if (!player) return;
        
        g_currentPlayerState.x = player->getPositionX();
        g_currentPlayerState.y = player->getPositionY();
        g_currentPlayerState.yVel = player->m_yVelocity;
        g_currentPlayerState.rotation = player->getRotation();
        
        g_currentPlayerState.isUpsideDown = player->m_isUpsideDown;
        g_currentPlayerState.isMini = player->m_vehicleSize != 1.0f;
        g_currentPlayerState.isOnGround = player->m_isOnGround;
        
        // Determine gamemode
        if (player->m_isShip) {
            g_currentPlayerState.gameMode = BotGameMode::Ship;
        } else if (player->m_isBall) {
            g_currentPlayerState.gameMode = BotGameMode::Ball;
        } else if (player->m_isBird) {
            g_currentPlayerState.gameMode = BotGameMode::UFO;
        } else if (player->m_isDart) {
            g_currentPlayerState.gameMode = BotGameMode::Wave;
        } else if (player->m_isRobot) {
            g_currentPlayerState.gameMode = BotGameMode::Robot;
        } else if (player->m_isSpider) {
            g_currentPlayerState.gameMode = BotGameMode::Spider;
        } else if (player->m_isSwing) {
            g_currentPlayerState.gameMode = BotGameMode::Swing;
        } else {
            g_currentPlayerState.gameMode = BotGameMode::Cube;
        }
        
        // Determine speed
        float playerSpeed = player->m_playerSpeed;
        if (playerSpeed <= 0.8f) {
            g_currentPlayerState.speed = BotSpeed::Slow;
        } else if (playerSpeed <= 0.95f) {
            g_currentPlayerState.speed = BotSpeed::Normal;
        } else if (playerSpeed <= 1.05f) {
            g_currentPlayerState.speed = BotSpeed::Fast;
        } else if (playerSpeed <= 1.15f) {
            g_currentPlayerState.speed = BotSpeed::Faster;
        } else {
            g_currentPlayerState.speed = BotSpeed::Fastest;
        }
    }
};

// ============================================================================
// DEBUG OVERLAY UI
// ============================================================================

class BotOverlay : public CCNode {
public:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_statsLabel = nullptr;
    CCLabelBMFont* m_posLabel = nullptr;
    CCDrawNode* m_trajectoryDraw = nullptr;
    
    static BotOverlay* create() {
        auto* overlay = new BotOverlay();
        if (overlay && overlay->initOverlay()) {
            overlay->autorelease();
            return overlay;
        }
        CC_SAFE_DELETE(overlay);
        return nullptr;
    }
    
    bool initOverlay() {
        if (!CCNode::init()) return false;
        
        // Status label
        m_statusLabel = CCLabelBMFont::create("AutoBot: OFF", "bigFont.fnt");
        m_statusLabel->setScale(0.45f);
        m_statusLabel->setAnchorPoint({0, 1});
        m_statusLabel->setPosition({8, 318});
        m_statusLabel->setOpacity(200);
        addChild(m_statusLabel, 100);
        
        // Stats label
        m_statsLabel = CCLabelBMFont::create("Clicks: 0 | Best: 0%", "chatFont.fnt");
        m_statsLabel->setScale(0.55f);
        m_statsLabel->setAnchorPoint({0, 1});
        m_statsLabel->setPosition({8, 292});
        m_statsLabel->setOpacity(180);
        addChild(m_statsLabel, 100);
        
        // Position label
        m_posLabel = CCLabelBMFont::create("X: 0 Y: 0", "chatFont.fnt");
        m_posLabel->setScale(0.5f);
        m_posLabel->setAnchorPoint({0, 1});
        m_posLabel->setPosition({8, 272});
        m_posLabel->setOpacity(150);
        addChild(m_posLabel, 100);
        
        // Trajectory draw node
        m_trajectoryDraw = CCDrawNode::create();
        addChild(m_trajectoryDraw, 50);
        
        scheduleUpdate();
        
        return true;
    }
    
    void update(float dt) override {
        updateLabels();
        updateTrajectoryDraw();
    }
    
private:
    void updateLabels() {
        // Status
        if (g_botEnabled) {
            m_statusLabel->setString("AutoBot: ON");
            m_statusLabel->setColor(ccc3(50, 255, 50));
        } else {
            m_statusLabel->setString("AutoBot: OFF");
            m_statusLabel->setColor(ccc3(255, 80, 80));
        }
        
        // Stats
        char statsBuffer[128];
        snprintf(statsBuffer, sizeof(statsBuffer), 
            "Clicks: %d | Best: %.1f%%", 
            g_totalClicks, g_bestProgress);
        m_statsLabel->setString(statsBuffer);
        
        // Position
        char posBuffer[128];
        const char* modeNames[] = {"Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing"};
        snprintf(posBuffer, sizeof(posBuffer),
            "X: %.0f  Y: %.0f  %s %s",
            g_currentPlayerState.x,
            g_currentPlayerState.y,
            modeNames[(int)g_currentPlayerState.gameMode],
            g_currentPlayerState.isOnGround ? "[Ground]" : "[Air]");
        m_posLabel->setString(posBuffer);
    }
    
    void updateTrajectoryDraw() {
        m_trajectoryDraw->clear();
        
        if (!g_debugDraw || !g_botEnabled) return;
        
        // Draw "no click" trajectory in red
        PlayerState noClickSim = g_currentPlayerState;
        CCPoint prevPoint = ccp(noClickSim.x, noClickSim.y);
        
        for (int i = 0; i < 60; i++) {
            PhysicsEngine::simulateFrame(noClickSim, false);
            CCPoint newPoint = ccp(noClickSim.x, noClickSim.y);
            
            float alpha = 1.0f - (float)i / 60.0f;
            m_trajectoryDraw->drawSegment(
                prevPoint, newPoint, 
                1.5f, 
                ccc4f(1.0f, 0.3f, 0.3f, alpha * 0.7f)
            );
            
            prevPoint = newPoint;
            
            if (CollisionSystem::willPlayerDie(noClickSim)) break;
        }
        
        // Draw "click" trajectory in green
        PlayerState clickSim = g_currentPlayerState;
        prevPoint = ccp(clickSim.x, clickSim.y);
        
        for (int i = 0; i < 60; i++) {
            bool hold = (i < 12);
            PhysicsEngine::simulateFrame(clickSim, hold);
            InteractionHandler::handleInteractions(clickSim, hold);
            CCPoint newPoint = ccp(clickSim.x, clickSim.y);
            
            float alpha = 1.0f - (float)i / 60.0f;
            m_trajectoryDraw->drawSegment(
                prevPoint, newPoint,
                1.5f,
                ccc4f(0.3f, 1.0f, 0.3f, alpha * 0.7f)
            );
            
            prevPoint = newPoint;
            
            if (CollisionSystem::willPlayerDie(clickSim)) break;
        }
        
        // Draw nearby hazards
        for (const auto& obj : g_levelObjects) {
            if (obj.isHazard) {
                float dist = obj.x - g_currentPlayerState.x;
                if (dist > -100 && dist < 500) {
                    m_trajectoryDraw->drawDot(
                        ccp(obj.x, obj.y),
                        obj.width / 3.0f,
                        ccc4f(1.0f, 0.0f, 0.0f, 0.4f)
                    );
                }
            }
            else if (obj.isOrb && std::abs(obj.x - g_currentPlayerState.x) < 300) {
                m_trajectoryDraw->drawDot(
                    ccp(obj.x, obj.y),
                    10.0f,
                    ccc4f(1.0f, 1.0f, 0.0f, 0.5f)
                );
            }
        }
        
        // Draw player position
        m_trajectoryDraw->drawDot(
            ccp(g_currentPlayerState.x, g_currentPlayerState.y),
            10.0f,
            ccc4f(1.0f, 1.0f, 1.0f, 0.9f)
        );
    }
};

static BotOverlay* g_botOverlay = nullptr;

// ============================================================================
// PAUSE LAYER HOOK - Add bot controls to pause menu
// ============================================================================

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        // Create menu
        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 200);
        
        // Bot toggle button
        auto* botOnSprite = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* botOffSprite = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        
        auto* botToggle = CCMenuItemToggler::create(
            botOffSprite, botOnSprite,
            this, menu_selector(BotPauseLayer::onBotToggle)
        );
        botToggle->setPosition({winSize.width - 40, winSize.height - 40});
        botToggle->toggle(g_botEnabled);
        menu->addChild(botToggle);
        
        // Bot label
        auto* botLabel = CCLabelBMFont::create("AutoBot", "bigFont.fnt");
        botLabel->setScale(0.35f);
        botLabel->setPosition({winSize.width - 40, winSize.height - 60});
        this->addChild(botLabel, 200);
        
        // Debug toggle
        auto* debugOnSprite = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* debugOffSprite = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        debugOnSprite->setScale(0.7f);
        debugOffSprite->setScale(0.7f);
        
        auto* debugToggle = CCMenuItemToggler::create(
            debugOffSprite, debugOnSprite,
            this, menu_selector(BotPauseLayer::onDebugToggle)
        );
        debugToggle->setPosition({winSize.width - 40, winSize.height - 90});
        debugToggle->toggle(g_debugDraw);
        menu->addChild(debugToggle);
        
        // Debug label
        auto* debugLabel = CCLabelBMFont::create("Debug", "bigFont.fnt");
        debugLabel->setScale(0.25f);
        debugLabel->setPosition({winSize.width - 40, winSize.height - 108});
        this->addChild(debugLabel, 200);
    }
    
    void onBotToggle(CCObject* sender) {
        g_botEnabled = !g_botEnabled;
        log::info("AutoBot: {} (menu)", g_botEnabled ? "ENABLED" : "DISABLED");
        
        // Release input if disabling
        if (!g_botEnabled && g_isHolding) {
            if (auto* gameLayer = GJBaseGameLayer::get()) {
                gameLayer->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
    }
    
    void onDebugToggle(CCObject* sender) {
        g_debugDraw = !g_debugDraw;
        log::info("Debug draw: {}", g_debugDraw ? "ON" : "OFF");
    }
};

// ============================================================================
// PLAY LAYER HOOK - Main bot logic
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        
        log::info("AutoBot: PlayLayer initialized");
        
        // Reset state
        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;
        g_totalClicks = 0;
        g_totalAttempts = 0;
        g_bestProgress = 0;
        
        // Create overlay
        g_botOverlay = BotOverlay::create();
        g_botOverlay->setZOrder(9999);
        this->addChild(g_botOverlay);
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        
        // Analyze level after setup
        LevelAnalyzer::analyzeLevel(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        log::info("AutoBot: Level reset (attempt {})", g_totalAttempts + 1);
        
        // Release any held input
        if (g_isHolding) {
            if (auto* gameLayer = GJBaseGameLayer::get()) {
                gameLayer->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        // Track attempts and progress
        g_totalAttempts++;
        g_frameCounter = 0;
        g_totalClicks = 0;
        
        // Re-analyze if needed
        if (!g_levelAnalyzed) {
            LevelAnalyzer::analyzeLevel(this);
        }
    }
    
    void update(float dt) {
        // Call original update
        PlayLayer::update(dt);
        
        g_frameCounter++;
        
        // Skip if bot disabled
        if (!g_botEnabled) {
            return;
        }
        
        // Skip if no player
        if (!m_player1) {
            return;
        }
        
        // Skip if paused, completed, or dead
        if (m_isPaused || m_hasCompletedLevel || m_player1->m_isDead) {
            return;
        }
        
        // Skip if level not analyzed
        if (!g_levelAnalyzed) {
            if (g_frameCounter % 60 == 0) {
                log::warn("AutoBot: Level not analyzed yet!");
            }
            return;
        }
        
        // Sync player state from game
        PlayerStateSync::syncFromGame(m_player1);
        
        // Track progress
        float progress = (g_currentPlayerState.x / m_levelLength) * 100.0f;
        if (progress > g_bestProgress) {
            g_bestProgress = progress;
        }
        
        // Get bot decision
        bool shouldClick = BotBrain::shouldClick(g_currentPlayerState);
        
        // Apply input if changed
        if (shouldClick != g_isHolding) {
            auto* gameLayer = GJBaseGameLayer::get();
            if (gameLayer) {
                gameLayer->handleButton(shouldClick, 1, true);
                g_isHolding = shouldClick;
                
                if (shouldClick) {
                    g_totalClicks++;
                    log::info("AutoBot: CLICK #{} at x={:.0f} y={:.0f}",
                        g_totalClicks, g_currentPlayerState.x, g_currentPlayerState.y);
                }
            }
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        
        log::info("AutoBot: LEVEL COMPLETE! Clicks: {} Attempts: {}",
            g_totalClicks, g_totalAttempts);
        
        Notification::create(
            fmt::format("AutoBot Complete!\n{} clicks, {} attempts", 
                g_totalClicks, g_totalAttempts),
            NotificationIcon::Success
        )->show();
    }
    
    void onQuit() {
        // Release input
        if (g_isHolding) {
            if (auto* gameLayer = GJBaseGameLayer::get()) {
                gameLayer->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        g_levelAnalyzed = false;
        g_botOverlay = nullptr;
        
        PlayLayer::onQuit();
    }
};

// ============================================================================
// KEYBOARD HOOK - Hotkeys
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            // F8 - Toggle bot
            if (key == KEY_F8) {
                g_botEnabled = !g_botEnabled;
                
                log::info("AutoBot: {} (F8)", g_botEnabled ? "ENABLED" : "DISABLED");
                
                if (!g_botEnabled && g_isHolding) {
                    if (auto* gameLayer = GJBaseGameLayer::get()) {
                        gameLayer->handleButton(false, 1, true);
                    }
                    g_isHolding = false;
                }
                
                Notification::create(
                    g_botEnabled ? "AutoBot: ON" : "AutoBot: OFF",
                    g_botEnabled ? NotificationIcon::Success : NotificationIcon::Info
                )->show();
                
                return true;
            }
            
            // F9 - Toggle debug
            if (key == KEY_F9) {
                g_debugDraw = !g_debugDraw;
                
                log::info("Debug: {}", g_debugDraw ? "ON" : "OFF");
                
                Notification::create(
                    g_debugDraw ? "Debug Draw: ON" : "Debug Draw: OFF",
                    NotificationIcon::Info
                )->show();
                
                return true;
            }
        }
        
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// ============================================================================
// MOD INITIALIZATION
// ============================================================================

$on_mod(Loaded) {
    log::info("================================================");
    log::info("       AutoBot Mod Loaded Successfully!");
    log::info("================================================");
    log::info("  Controls:");
    log::info("    F8 = Toggle AutoBot ON/OFF");
    log::info("    F9 = Toggle Debug Visualization");
    log::info("    Pause Menu = Bot toggle buttons");
    log::info("================================================");
    log::info("  Features:");
    log::info("    - Physics simulation for all 8 gamemodes");
    log::info("    - Automatic hazard avoidance");
    log::info("    - Orb and pad handling");
    log::info("    - Portal support");
    log::info("    - Visual trajectory debugging");
    log::info("================================================");
}
