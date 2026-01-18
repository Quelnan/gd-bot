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
#include <deque>

using namespace geode::prelude;

// ============================================================================
// GLOBAL STATE VARIABLES
// ============================================================================

static bool g_botEnabled = false;
static bool g_debugDraw = false;
static bool g_isHolding = false;
static bool g_levelAnalyzed = false;
static int g_frameCounter = 0;
static int g_totalClicks = 0;
static int g_totalAttempts = 0;
static float g_bestProgress = 0.0f;
static float g_currentProgress = 0.0f;
static float g_levelLength = 1000.0f;

// Click history for visualization
static std::deque<std::pair<float, float>> g_clickHistory;
static const size_t MAX_CLICK_HISTORY = 50;

// ============================================================================
// ENUMS - Renamed to avoid conflicts with GD's enums
// ============================================================================

enum class BotGameMode {
    Cube = 0,
    Ship = 1,
    Ball = 2,
    UFO = 3,
    Wave = 4,
    Robot = 5,
    Spider = 6,
    Swing = 7
};

enum class BotSpeed {
    Slow = 0,      // 0.7x
    Normal = 1,    // 0.9x
    Fast = 2,      // 1.0x
    Faster = 3,    // 1.1x
    Fastest = 4,   // 1.3x
    SuperFast = 5  // 1.6x+
};

// ============================================================================
// PLAYER STATE STRUCTURE
// ============================================================================

struct PlayerState {
    // Position and velocity
    float x = 0.0f;
    float y = 105.0f;
    float yVelocity = 0.0f;
    float rotation = 0.0f;
    
    // Current gamemode and speed
    BotGameMode gameMode = BotGameMode::Cube;
    BotSpeed speed = BotSpeed::Normal;
    
    // State flags
    bool isUpsideDown = false;
    bool isMini = false;
    bool isOnGround = true;
    bool canJump = true;
    bool isDead = false;
    
    // Orb interaction cooldown
    float orbCooldown = 0.0f;
    int lastOrbID = -1;
    
    // Robot-specific state
    bool isRobotBoosting = false;
    float robotBoostTime = 0.0f;
    
    // Spider-specific state
    bool hasSpiderFlipped = false;
    
    // Wave trail positions for debug
    float prevX = 0.0f;
    float prevY = 0.0f;
    
    // Copy constructor
    PlayerState copy() const {
        PlayerState newState;
        newState.x = x;
        newState.y = y;
        newState.yVelocity = yVelocity;
        newState.rotation = rotation;
        newState.gameMode = gameMode;
        newState.speed = speed;
        newState.isUpsideDown = isUpsideDown;
        newState.isMini = isMini;
        newState.isOnGround = isOnGround;
        newState.canJump = canJump;
        newState.isDead = isDead;
        newState.orbCooldown = orbCooldown;
        newState.lastOrbID = lastOrbID;
        newState.isRobotBoosting = isRobotBoosting;
        newState.robotBoostTime = robotBoostTime;
        newState.hasSpiderFlipped = hasSpiderFlipped;
        newState.prevX = prevX;
        newState.prevY = prevY;
        return newState;
    }
};

// ============================================================================
// LEVEL OBJECT STRUCTURE
// ============================================================================

struct LevelObject {
    // Basic properties
    int objectID = 0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 30.0f;
    float height = 30.0f;
    float rotation = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    
    // Object type flags
    bool isHazard = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;
    bool isSolid = false;
    bool isSlope = false;
    
    // Interaction type (for orbs/pads)
    // 0=yellow, 1=pink, 2=red, 3=blue, 4=green, 5=black, 6=dash green, 7=dash magenta
    int interactionType = 0;
    
    // Portal properties
    BotGameMode portalGameMode = BotGameMode::Cube;
    BotSpeed portalSpeed = BotSpeed::Normal;
    bool isGravityPortal = false;
    bool gravityGoesUp = false;
    bool isSizePortal = false;
    bool sizeIsMini = false;
    bool isSpeedPortal = false;
    bool isDualPortal = false;
    bool isMirrorPortal = false;
    
    // For moving objects (future feature)
    bool isMoving = false;
    float moveOffsetX = 0.0f;
    float moveOffsetY = 0.0f;
};

// ============================================================================
// GLOBAL LEVEL DATA STORAGE
// ============================================================================

static std::vector<LevelObject> g_levelObjects;
static std::vector<LevelObject*> g_hazardObjects;
static std::vector<LevelObject*> g_orbObjects;
static std::vector<LevelObject*> g_padObjects;
static std::vector<LevelObject*> g_portalObjects;
static PlayerState g_currentPlayerState;
static PlayerState g_simulatedState;

// ============================================================================
// OBJECT ID DEFINITIONS - Comprehensive list
// ============================================================================

// Spike IDs
static const std::set<int> SPIKE_IDS = {
    8, 39, 103,  // Basic spikes
    392,         // Small spike
    9, 61,       // Spike variations
    243, 244, 245, 246, 247, 248, 249,  // Colored spikes
    363, 364, 365, 366, 367, 368,       // Saw blades
    446, 447,    // More hazards
    678, 679, 680,  // 2.0 spikes
    1705, 1706, 1707, 1708, 1709, 1710,  // 2.1 spikes
    1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718,  // More 2.1 spikes
    1719, 1720, 1721, 1722, 1723, 1724, 1725,  // Even more spikes
    1726, 1727, 1728, 1729, 1730, 1731, 1732   // And more
};

// Monster/enemy hazards
static const std::set<int> MONSTER_IDS = {
    1585, 1586, 1587, 1588, 1589,  // Monsters
    1590, 1591, 1592, 1593, 1594   // More monsters
};

// Saw blade IDs
static const std::set<int> SAW_IDS = {
    88, 89, 98, 397, 398, 399,
    678, 679, 680,
    740, 741, 742
};

// All hazard IDs combined
static const std::set<int> ALL_HAZARD_IDS = []() {
    std::set<int> combined;
    combined.insert(SPIKE_IDS.begin(), SPIKE_IDS.end());
    combined.insert(MONSTER_IDS.begin(), MONSTER_IDS.end());
    combined.insert(SAW_IDS.begin(), SAW_IDS.end());
    return combined;
}();

// Orb IDs and their types
static const std::map<int, int> ORB_IDS = {
    {36, 0},    // Yellow orb - regular jump
    {84, 1},    // Pink orb - higher jump
    {141, 2},   // Red orb - very high jump
    {1022, 3},  // Blue orb - gravity flip + jump
    {1330, 4},  // Green orb - jump + gravity flip
    {1333, 5},  // Black orb - no effect, keeps momentum
    {1704, 6},  // Dash orb green
    {1751, 7},  // Dash orb magenta
    {1594, 8},  // Spider orb
    {1764, 9}   // Teleport orb
};

// Pad IDs and their types
static const std::map<int, int> PAD_IDS = {
    {35, 0},    // Yellow pad - regular boost
    {67, 1},    // Pink pad - higher boost
    {140, 2},   // Red pad - very high boost
    {1332, 3},  // Blue pad - gravity flip boost
    {452, 4},   // Spider pad - teleport to opposite surface
    {1697, 5}   // Rebound pad
};

// Gamemode portal IDs
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

// Speed portal IDs
static const std::map<int, BotSpeed> SPEED_PORTAL_IDS = {
    {200, BotSpeed::Slow},
    {201, BotSpeed::Normal},
    {202, BotSpeed::Fast},
    {203, BotSpeed::Faster},
    {1334, BotSpeed::Fastest},
    {1334, BotSpeed::Fastest}  // Duplicate for safety
};

// Gravity portal IDs
static const int GRAVITY_DOWN_PORTAL = 10;
static const int GRAVITY_UP_PORTAL = 11;

// Size portal IDs
static const int MINI_PORTAL = 101;
static const int NORMAL_SIZE_PORTAL = 99;

// Mirror portal IDs
static const int MIRROR_ON_PORTAL = 45;
static const int MIRROR_OFF_PORTAL = 46;

// Dual portal IDs
static const int DUAL_ON_PORTAL = 286;
static const int DUAL_OFF_PORTAL = 287;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

namespace Physics {
    // World bounds
    constexpr float GROUND_Y = 105.0f;
    constexpr float CEILING_Y = 2085.0f;
    constexpr float DEATH_Y_MIN = 50.0f;
    constexpr float DEATH_Y_MAX = 2100.0f;
    
    // Player size
    constexpr float PLAYER_SIZE = 30.0f;
    constexpr float MINI_SCALE = 0.6f;
    constexpr float WAVE_HITBOX_SCALE = 0.6f;
    
    // Gravity values
    constexpr float GRAVITY_CUBE = 0.958199f;
    constexpr float GRAVITY_SHIP = 0.8f;
    constexpr float GRAVITY_BALL = 0.6f;
    constexpr float GRAVITY_UFO = 0.5f;
    constexpr float GRAVITY_WAVE = 0.0f;
    constexpr float GRAVITY_ROBOT = 0.958199f;
    constexpr float GRAVITY_SPIDER = 0.6f;
    constexpr float GRAVITY_SWING = 0.7f;
    
    // Jump velocities
    constexpr float JUMP_VELOCITY_CUBE = 11.180032f;
    constexpr float JUMP_VELOCITY_CUBE_MINI = 9.4f;
    constexpr float JUMP_VELOCITY_ROBOT = 10.0f;
    constexpr float JUMP_VELOCITY_ROBOT_MINI = 7.5f;
    
    // UFO boost
    constexpr float UFO_BOOST = 7.0f;
    constexpr float UFO_BOOST_MINI = 5.5f;
    
    // Ship acceleration
    constexpr float SHIP_ACCEL = 0.8f;
    constexpr float SHIP_ACCEL_MINI = 0.6f;
    constexpr float SHIP_MAX_VELOCITY = 8.0f;
    constexpr float SHIP_MAX_VELOCITY_MINI = 6.0f;
    
    // Ball velocity
    constexpr float BALL_SWITCH_VELOCITY = 6.0f;
    
    // Terminal velocities
    constexpr float MAX_FALL_VELOCITY = 15.0f;
    constexpr float MAX_RISE_VELOCITY = 15.0f;
    
    // Robot boost
    constexpr float ROBOT_MAX_BOOST_TIME = 0.25f;
    constexpr float ROBOT_BOOST_ACCEL = 0.5f;
    
    // Orb boosts
    constexpr float ORB_YELLOW_BOOST = 11.2f;
    constexpr float ORB_PINK_BOOST = 14.0f;
    constexpr float ORB_RED_BOOST = 18.0f;
    constexpr float ORB_BLUE_BOOST = 8.0f;
    constexpr float ORB_GREEN_BOOST = 11.2f;
    constexpr float ORB_DASH_BOOST = 15.0f;
    
    // Pad boosts
    constexpr float PAD_YELLOW_BOOST = 12.0f;
    constexpr float PAD_PINK_BOOST = 16.0f;
    constexpr float PAD_RED_BOOST = 20.0f;
    constexpr float PAD_BLUE_BOOST = 12.0f;
    
    // Cooldowns
    constexpr float ORB_COOLDOWN = 0.1f;
    
    // Speed multipliers (units per second)
    constexpr float SPEED_SLOW = 251.16f;
    constexpr float SPEED_NORMAL = 311.58f;
    constexpr float SPEED_FAST = 387.42f;
    constexpr float SPEED_FASTER = 468.0f;
    constexpr float SPEED_FASTEST = 576.0f;
    constexpr float SPEED_SUPERFAST = 700.0f;
    
    // Rotation speeds
    constexpr float CUBE_ROTATION_SPEED = 7.5f;
    constexpr float BALL_ROTATION_SPEED = 10.0f;
    
    // Physics timestep
    constexpr float PHYSICS_DT = 1.0f / 240.0f;
}

// ============================================================================
// PHYSICS ENGINE CLASS
// ============================================================================

class PhysicsEngine {
public:
    // Get horizontal speed based on speed setting
    static float getHorizontalSpeed(BotSpeed speed) {
        switch (speed) {
            case BotSpeed::Slow:      return Physics::SPEED_SLOW;
            case BotSpeed::Normal:    return Physics::SPEED_NORMAL;
            case BotSpeed::Fast:      return Physics::SPEED_FAST;
            case BotSpeed::Faster:    return Physics::SPEED_FASTER;
            case BotSpeed::Fastest:   return Physics::SPEED_FASTEST;
            case BotSpeed::SuperFast: return Physics::SPEED_SUPERFAST;
            default:                  return Physics::SPEED_NORMAL;
        }
    }
    
    // Get gravity for gamemode
    static float getGravity(BotGameMode mode, bool isMini) {
        float baseGravity;
        switch (mode) {
            case BotGameMode::Cube:   baseGravity = Physics::GRAVITY_CUBE; break;
            case BotGameMode::Ship:   baseGravity = Physics::GRAVITY_SHIP; break;
            case BotGameMode::Ball:   baseGravity = Physics::GRAVITY_BALL; break;
            case BotGameMode::UFO:    baseGravity = Physics::GRAVITY_UFO; break;
            case BotGameMode::Wave:   baseGravity = Physics::GRAVITY_WAVE; break;
            case BotGameMode::Robot:  baseGravity = Physics::GRAVITY_ROBOT; break;
            case BotGameMode::Spider: baseGravity = Physics::GRAVITY_SPIDER; break;
            case BotGameMode::Swing:  baseGravity = Physics::GRAVITY_SWING; break;
            default:                  baseGravity = Physics::GRAVITY_CUBE; break;
        }
        return isMini ? baseGravity * 0.8f : baseGravity;
    }
    
    // Get jump velocity for gamemode
    static float getJumpVelocity(BotGameMode mode, bool isMini) {
        switch (mode) {
            case BotGameMode::Cube:
                return isMini ? Physics::JUMP_VELOCITY_CUBE_MINI : Physics::JUMP_VELOCITY_CUBE;
            case BotGameMode::Robot:
                return isMini ? Physics::JUMP_VELOCITY_ROBOT_MINI : Physics::JUMP_VELOCITY_ROBOT;
            default:
                return isMini ? Physics::JUMP_VELOCITY_CUBE_MINI : Physics::JUMP_VELOCITY_CUBE;
        }
    }
    
    // Main simulation function
    static void simulateFrame(PlayerState& state, bool isHolding, float dt = Physics::PHYSICS_DT) {
        // Store previous position
        state.prevX = state.x;
        state.prevY = state.y;
        
        // Calculate horizontal movement
        float xSpeed = getHorizontalSpeed(state.speed) * dt;
        
        // Update cooldowns
        if (state.orbCooldown > 0) {
            state.orbCooldown -= dt;
            if (state.orbCooldown < 0) state.orbCooldown = 0;
        }
        
        // Get gravity (inverted if upside down)
        float gravity = getGravity(state.gameMode, state.isMini);
        if (state.isUpsideDown) {
            gravity = -gravity;
        }
        
        // Ground Y position depends on gravity direction
        float groundY = state.isUpsideDown ? Physics::CEILING_Y : Physics::GROUND_Y;
        
        // Simulate based on gamemode
        switch (state.gameMode) {
            case BotGameMode::Cube:
                simulateCubeMode(state, isHolding, gravity, groundY, dt);
                break;
            case BotGameMode::Ship:
                simulateShipMode(state, isHolding, gravity, dt);
                break;
            case BotGameMode::Ball:
                simulateBallMode(state, isHolding, gravity, groundY, dt);
                break;
            case BotGameMode::UFO:
                simulateUFOMode(state, isHolding, gravity, groundY, dt);
                break;
            case BotGameMode::Wave:
                simulateWaveMode(state, isHolding, xSpeed, dt);
                break;
            case BotGameMode::Robot:
                simulateRobotMode(state, isHolding, gravity, groundY, dt);
                break;
            case BotGameMode::Spider:
                simulateSpiderMode(state, isHolding, gravity, groundY, dt);
                break;
            case BotGameMode::Swing:
                simulateSwingMode(state, isHolding, gravity, dt);
                break;
        }
        
        // Move forward
        state.x += xSpeed;
        
        // Clamp to level bounds
        state.y = std::clamp(state.y, Physics::DEATH_Y_MIN, Physics::DEATH_Y_MAX);
    }
    
private:
    // Cube mode simulation
    static void simulateCubeMode(PlayerState& state, bool isHolding, float gravity, float groundY, float dt) {
        // Apply gravity
        state.yVelocity -= gravity;
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -Physics::MAX_FALL_VELOCITY, Physics::MAX_RISE_VELOCITY);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Ground collision
        bool hitGround;
        if (state.isUpsideDown) {
            hitGround = state.y >= groundY;
        } else {
            hitGround = state.y <= groundY;
        }
        
        if (hitGround) {
            state.y = groundY;
            state.yVelocity = 0;
            state.isOnGround = true;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        // Jump when holding on ground
        if (isHolding && state.isOnGround && state.canJump) {
            float jumpVel = getJumpVelocity(BotGameMode::Cube, state.isMini);
            state.yVelocity = state.isUpsideDown ? -jumpVel : jumpVel;
            state.isOnGround = false;
            state.canJump = false;
        }
        
        // Reset jump ability when not holding
        if (!isHolding) {
            state.canJump = true;
        }
        
        // Update rotation
        if (!state.isOnGround) {
            float rotSpeed = state.isUpsideDown ? -Physics::CUBE_ROTATION_SPEED : Physics::CUBE_ROTATION_SPEED;
            state.rotation += rotSpeed;
        } else {
            // Snap to nearest 90 degrees
            state.rotation = std::round(state.rotation / 90.0f) * 90.0f;
        }
    }
    
    // Ship mode simulation
    static void simulateShipMode(PlayerState& state, bool isHolding, float gravity, float dt) {
        float accel = state.isMini ? Physics::SHIP_ACCEL_MINI : Physics::SHIP_ACCEL;
        float maxVel = state.isMini ? Physics::SHIP_MAX_VELOCITY_MINI : Physics::SHIP_MAX_VELOCITY;
        
        if (isHolding) {
            state.yVelocity += state.isUpsideDown ? -accel : accel;
        } else {
            state.yVelocity += state.isUpsideDown ? accel : -accel;
        }
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -maxVel, maxVel);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Ship rotation follows velocity
        state.rotation = state.yVelocity * 2.0f;
        
        // Ship is never "on ground" in the jumping sense
        state.isOnGround = false;
        
        // Boundary collision
        if (state.y <= Physics::GROUND_Y) {
            state.y = Physics::GROUND_Y;
            state.yVelocity = std::max(state.yVelocity, 0.0f);
        }
        if (state.y >= Physics::CEILING_Y) {
            state.y = Physics::CEILING_Y;
            state.yVelocity = std::min(state.yVelocity, 0.0f);
        }
    }
    
    // Ball mode simulation
    static void simulateBallMode(PlayerState& state, bool isHolding, float gravity, float groundY, float dt) {
        // Apply reduced gravity
        state.yVelocity -= gravity * 0.6f;
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -12.0f, 12.0f);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Ground collision
        bool hitGround;
        if (state.isUpsideDown) {
            hitGround = state.y >= groundY;
        } else {
            hitGround = state.y <= groundY;
        }
        
        if (hitGround) {
            state.y = groundY;
            state.yVelocity = 0;
            state.isOnGround = true;
        } else {
            state.isOnGround = false;
        }
        
        // Click to reverse gravity
        if (isHolding && state.isOnGround && state.canJump) {
            state.isUpsideDown = !state.isUpsideDown;
            state.yVelocity = state.isUpsideDown ? -Physics::BALL_SWITCH_VELOCITY : Physics::BALL_SWITCH_VELOCITY;
            state.canJump = false;
            state.isOnGround = false;
        }
        
        if (!isHolding) {
            state.canJump = true;
        }
        
        // Ball always rotates
        float rotSpeed = state.isUpsideDown ? -Physics::BALL_ROTATION_SPEED : Physics::BALL_ROTATION_SPEED;
        state.rotation += rotSpeed;
    }
    
    // UFO mode simulation
    static void simulateUFOMode(PlayerState& state, bool isHolding, float gravity, float groundY, float dt) {
        // Apply reduced gravity
        state.yVelocity -= gravity * 0.5f;
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -8.0f, 8.0f);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Ground collision
        bool hitGround;
        if (state.isUpsideDown) {
            hitGround = state.y >= groundY;
        } else {
            hitGround = state.y <= groundY;
        }
        
        if (hitGround) {
            state.y = groundY;
            state.yVelocity = 0;
            state.isOnGround = true;
        } else {
            state.isOnGround = false;
        }
        
        // Each click gives a boost
        if (isHolding && state.canJump) {
            float boost = state.isMini ? Physics::UFO_BOOST_MINI : Physics::UFO_BOOST;
            state.yVelocity = state.isUpsideDown ? -boost : boost;
            state.canJump = false;
        }
        
        if (!isHolding) {
            state.canJump = true;
        }
    }
    
    // Wave mode simulation
    static void simulateWaveMode(PlayerState& state, bool isHolding, float xSpeed, float dt) {
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
    
    // Robot mode simulation
    static void simulateRobotMode(PlayerState& state, bool isHolding, float gravity, float groundY, float dt) {
        // Robot can hold to jump higher
        if (state.isRobotBoosting && isHolding) {
            state.robotBoostTime += dt;
            if (state.robotBoostTime < Physics::ROBOT_MAX_BOOST_TIME) {
                float boostAccel = state.isUpsideDown ? -Physics::ROBOT_BOOST_ACCEL : Physics::ROBOT_BOOST_ACCEL;
                state.yVelocity += boostAccel;
            }
        }
        
        // Apply gravity
        state.yVelocity -= gravity;
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -Physics::MAX_FALL_VELOCITY, Physics::MAX_RISE_VELOCITY);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Ground collision
        bool hitGround;
        if (state.isUpsideDown) {
            hitGround = state.y >= groundY;
        } else {
            hitGround = state.y <= groundY;
        }
        
        if (hitGround) {
            state.y = groundY;
            state.yVelocity = 0;
            state.isOnGround = true;
            state.isRobotBoosting = false;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        // Start jump
        if (isHolding && state.isOnGround && state.canJump) {
            float jumpVel = getJumpVelocity(BotGameMode::Robot, state.isMini);
            state.yVelocity = state.isUpsideDown ? -jumpVel : jumpVel;
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
    
    // Spider mode simulation
    static void simulateSpiderMode(PlayerState& state, bool isHolding, float gravity, float groundY, float dt) {
        // Apply gravity
        state.yVelocity -= gravity;
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -Physics::MAX_FALL_VELOCITY, Physics::MAX_RISE_VELOCITY);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Ground collision
        bool hitGround;
        if (state.isUpsideDown) {
            hitGround = state.y >= groundY;
        } else {
            hitGround = state.y <= groundY;
        }
        
        if (hitGround) {
            state.y = groundY;
            state.yVelocity = 0;
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
            state.y = state.isUpsideDown ? Physics::CEILING_Y : Physics::GROUND_Y;
            state.yVelocity = 0;
            state.hasSpiderFlipped = true;
            state.canJump = false;
        }
        
        if (!isHolding) {
            state.canJump = true;
        }
    }
    
    // Swing mode simulation
    static void simulateSwingMode(PlayerState& state, bool isHolding, float gravity, float dt) {
        // Swing copter - gravity direction depends on holding
        float swingGravity;
        if (isHolding) {
            swingGravity = state.isUpsideDown ? gravity : -gravity;
        } else {
            swingGravity = state.isUpsideDown ? -gravity : gravity;
        }
        
        state.yVelocity += swingGravity * 0.8f;
        
        // Clamp velocity
        state.yVelocity = std::clamp(state.yVelocity, -8.0f, 8.0f);
        
        // Apply velocity
        state.y += state.yVelocity;
        
        // Rotation follows velocity
        state.rotation = state.yVelocity * 3.0f;
        
        state.isOnGround = false;
    }
};

// ============================================================================
// COLLISION DETECTION SYSTEM
// ============================================================================

class CollisionSystem {
public:
    // Get player hitbox size
    static float getPlayerSize(const PlayerState& state) {
        float size = Physics::PLAYER_SIZE;
        
        if (state.isMini) {
            size *= Physics::MINI_SCALE;
        }
        
        if (state.gameMode == BotGameMode::Wave) {
            size *= Physics::WAVE_HITBOX_SCALE;
        }
        
        return size;
    }
    
    // Check AABB collision between player and object
    static bool checkCollision(const PlayerState& state, const LevelObject& obj) {
        float playerSize = getPlayerSize(state);
        float halfPlayer = playerSize / 2.0f;
        
        // Player bounds
        float playerLeft = state.x - halfPlayer;
        float playerRight = state.x + halfPlayer;
        float playerBottom = state.y - halfPlayer;
        float playerTop = state.y + halfPlayer;
        
        // Object bounds
        float objHalfW = obj.width / 2.0f;
        float objHalfH = obj.height / 2.0f;
        float objLeft = obj.x - objHalfW;
        float objRight = obj.x + objHalfW;
        float objBottom = obj.y - objHalfH;
        float objTop = obj.y + objHalfH;
        
        // Check overlap
        bool overlapsX = playerRight > objLeft && playerLeft < objRight;
        bool overlapsY = playerTop > objBottom && playerBottom < objTop;
        
        return overlapsX && overlapsY;
    }
    
    // Check if player will die at current position
    static bool willPlayerDie(const PlayerState& state) {
        // Check level bounds
        if (state.y < Physics::DEATH_Y_MIN + 10.0f || state.y > Physics::DEATH_Y_MAX - 10.0f) {
            return true;
        }
        
        // Check collision with hazards
        for (const auto* hazard : g_hazardObjects) {
            // Skip objects too far away
            float dist = hazard->x - state.x;
            if (dist < -100.0f || dist > 100.0f) continue;
            
            if (checkCollision(state, *hazard)) {
                return true;
            }
        }
        
        return false;
    }
    
    // Find orb collision (returns nullptr if none)
    static const LevelObject* findOrbCollision(const PlayerState& state) {
        for (const auto* orb : g_orbObjects) {
            float dist = orb->x - state.x;
            if (dist < -50.0f || dist > 50.0f) continue;
            
            if (checkCollision(state, *orb)) {
                return orb;
            }
        }
        return nullptr;
    }
    
    // Find pad collision
    static const LevelObject* findPadCollision(const PlayerState& state) {
        for (const auto* pad : g_padObjects) {
            float dist = pad->x - state.x;
            if (dist < -50.0f || dist > 50.0f) continue;
            
            if (checkCollision(state, *pad)) {
                return pad;
            }
        }
        return nullptr;
    }
    
    // Find portal collision
    static const LevelObject* findPortalCollision(const PlayerState& state) {
        for (const auto* portal : g_portalObjects) {
            float dist = portal->x - state.x;
            if (dist < -50.0f || dist > 50.0f) continue;
            
            if (checkCollision(state, *portal)) {
                return portal;
            }
        }
        return nullptr;
    }
    
    // Find next hazard ahead of player
    static const LevelObject* findNextHazard(const PlayerState& state, float maxDistance = 300.0f) {
        const LevelObject* nearest = nullptr;
        float nearestDist = maxDistance;
        
        for (const auto* hazard : g_hazardObjects) {
            float dist = hazard->x - state.x;
            if (dist > 0 && dist < nearestDist) {
                nearest = hazard;
                nearestDist = dist;
            }
        }
        
        return nearest;
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
            // Gamemode change
            state.gameMode = portal->portalGameMode;
            // Reduce velocity on gamemode change
            state.yVelocity *= 0.5f;
        }
    }
    
    static void handlePads(PlayerState& state) {
        const LevelObject* pad = CollisionSystem::findPadCollision(state);
        if (!pad) return;
        
        float boost = 0;
        switch (pad->interactionType) {
            case 0: boost = Physics::PAD_YELLOW_BOOST; break;
            case 1: boost = Physics::PAD_PINK_BOOST; break;
            case 2: boost = Physics::PAD_RED_BOOST; break;
            case 3: boost = -Physics::PAD_BLUE_BOOST; break;
            case 4: // Spider pad
                state.isUpsideDown = !state.isUpsideDown;
                return;
            default: return;
        }
        
        if (state.isUpsideDown) boost = -boost;
        state.yVelocity = boost;
        state.isOnGround = false;
    }
    
    static void handleOrbs(PlayerState& state) {
        if (state.orbCooldown > 0) return;
        
        const LevelObject* orb = CollisionSystem::findOrbCollision(state);
        if (!orb) return;
        if (orb->objectID == state.lastOrbID) return;
        
        float boost = 0;
        bool flipGravity = false;
        
        switch (orb->interactionType) {
            case 0: // Yellow
                boost = Physics::ORB_YELLOW_BOOST;
                break;
            case 1: // Pink
                boost = Physics::ORB_PINK_BOOST;
                break;
            case 2: // Red
                boost = Physics::ORB_RED_BOOST;
                break;
            case 3: // Blue
                flipGravity = true;
                boost = Physics::ORB_BLUE_BOOST;
                break;
            case 4: // Green
                flipGravity = true;
                boost = Physics::ORB_GREEN_BOOST;
                break;
            case 5: // Black - no boost
                return;
            case 6: // Dash green
            case 7: // Dash magenta
                boost = Physics::ORB_DASH_BOOST;
                break;
            default:
                return;
        }
        
        if (flipGravity) {
            state.isUpsideDown = !state.isUpsideDown;
        }
        
        // Apply upside down to boost (except for gravity-flipping orbs)
        if (state.isUpsideDown && !flipGravity) {
            boost = -boost;
        }
        
        state.yVelocity = boost;
        state.orbCooldown = Physics::ORB_COOLDOWN;
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
        // Clear all data
        g_levelObjects.clear();
        g_hazardObjects.clear();
        g_orbObjects.clear();
        g_padObjects.clear();
        g_portalObjects.clear();
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
        
        // Get level length
        g_levelLength = playLayer->m_levelLength;
        if (g_levelLength <= 0) g_levelLength = 1000.0f;
        
        // Counters for logging
        int hazardCount = 0;
        int orbCount = 0;
        int padCount = 0;
        int portalCount = 0;
        
        // Iterate all objects
        for (int i = 0; i < objects->count(); i++) {
            auto* gameObj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!gameObj) continue;
            
            int objID = gameObj->m_objectID;
            
            // Create level object
            LevelObject levelObj;
            levelObj.objectID = objID;
            levelObj.x = gameObj->getPositionX();
            levelObj.y = gameObj->getPositionY();
            levelObj.rotation = gameObj->getRotation();
            levelObj.scaleX = gameObj->getScaleX();
            levelObj.scaleY = gameObj->getScaleY();
            
            // Get hitbox size
            auto contentSize = gameObj->getContentSize();
            float scale = gameObj->getScale();
            levelObj.width = contentSize.width * scale * 0.8f;
            levelObj.height = contentSize.height * scale * 0.8f;
            
            // Minimum hitbox size
            if (levelObj.width < 10.0f) levelObj.width = 10.0f;
            if (levelObj.height < 10.0f) levelObj.height = 10.0f;
            
            // Categorize object
            bool isImportant = false;
            
            // Check hazards
            if (ALL_HAZARD_IDS.count(objID)) {
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
            else if (objID == GRAVITY_DOWN_PORTAL) {
                levelObj.isPortal = true;
                levelObj.isGravityPortal = true;
                levelObj.gravityGoesUp = false;
                portalCount++;
                isImportant = true;
            }
            else if (objID == GRAVITY_UP_PORTAL) {
                levelObj.isPortal = true;
                levelObj.isGravityPortal = true;
                levelObj.gravityGoesUp = true;
                portalCount++;
                isImportant = true;
            }
            // Check size portals
            else if (objID == NORMAL_SIZE_PORTAL) {
                levelObj.isPortal = true;
                levelObj.isSizePortal = true;
                levelObj.sizeIsMini = false;
                portalCount++;
                isImportant = true;
            }
            else if (objID == MINI_PORTAL) {
                levelObj.isPortal = true;
                levelObj.isSizePortal = true;
                levelObj.sizeIsMini = true;
                portalCount++;
                isImportant = true;
            }
            
            // Add to main list if important
            if (isImportant) {
                g_levelObjects.push_back(levelObj);
            }
        }
        
        // Sort all objects by X position
        std::sort(g_levelObjects.begin(), g_levelObjects.end(),
            [](const LevelObject& a, const LevelObject& b) {
                return a.x < b.x;
            });
        
        // Build category-specific pointer lists for fast access
        for (auto& obj : g_levelObjects) {
            if (obj.isHazard) {
                g_hazardObjects.push_back(&obj);
            }
            if (obj.isOrb) {
                g_orbObjects.push_back(&obj);
            }
            if (obj.isPad) {
                g_padObjects.push_back(&obj);
            }
            if (obj.isPortal) {
                g_portalObjects.push_back(&obj);
            }
        }
        
        log::info("AutoBot: Level analyzed successfully!");
        log::info("  - {} hazards", hazardCount);
        log::info("  - {} orbs", orbCount);
        log::info("  - {} pads", padCount);
        log::info("  - {} portals", portalCount);
        log::info("  - {} total tracked objects", g_levelObjects.size());
        log::info("  - Level length: {:.0f}", g_levelLength);
        
        g_levelAnalyzed = true;
    }
};

// ============================================================================
// BOT BRAIN - Decision Making
// ============================================================================

class BotBrain {
public:
    // Main decision function
    static bool shouldClick(const PlayerState& currentState) {
        // Simulate both options
        int surviveNoClick = simulateAndCountSurvival(currentState, false);
        int surviveClick = simulateAndCountSurvival(currentState, true);
        
        // Debug logging
        if (g_frameCounter % 60 == 0) {
            log::info("Bot @ x={:.0f} y={:.0f}: noClick={} click={} ground={}",
                currentState.x, currentState.y, 
                surviveNoClick, surviveClick, 
                currentState.isOnGround);
        }
        
        // Decision logic:
        
        // 1. If clicking survives longer, click
        if (surviveClick > surviveNoClick) {
            return true;
        }
        
        // 2. Emergency: about to die, try clicking
        if (surviveNoClick < 5 && surviveClick >= surviveNoClick) {
            return true;
        }
        
        // 3. Proactive jumping for cube mode
        if (currentState.gameMode == BotGameMode::Cube && currentState.isOnGround) {
            const LevelObject* nextHazard = CollisionSystem::findNextHazard(currentState, 200.0f);
            if (nextHazard) {
                float distance = nextHazard->x - currentState.x;
                // Jump if hazard is at ground level and approaching
                if (nextHazard->y < 200.0f && distance > 40.0f && distance < 120.0f) {
                    return true;
                }
            }
        }
        
        // 4. Ship mode: maintain altitude
        if (currentState.gameMode == BotGameMode::Ship) {
            // Try to stay in middle-ish area
            float targetY = 300.0f;
            if (currentState.y < targetY - 50.0f && currentState.yVelocity < 2.0f) {
                return true;
            }
        }
        
        // 5. Wave mode: need to hold to go up
        if (currentState.gameMode == BotGameMode::Wave) {
            // Check if we need to go up
            const LevelObject* hazard = CollisionSystem::findNextHazard(currentState, 150.0f);
            if (hazard && hazard->y > currentState.y) {
                return true;
            }
        }
        
        return false;
    }
    
private:
    // Simulate X frames and count how many we survive
    static int simulateAndCountSurvival(const PlayerState& startState, bool doClick) {
        PlayerState simState = startState.copy();
        
        const int maxFrames = 50;
        const int clickDuration = 12; // Frames to hold when clicking
        
        for (int frame = 0; frame < maxFrames; frame++) {
            // Determine if holding this frame
            bool isHolding = doClick && (frame < clickDuration);
            
            // Simulate physics
            PhysicsEngine::simulateFrame(simState, isHolding);
            
            // Handle interactions
            InteractionHandler::handleInteractions(simState, isHolding);
            
            // Check death
            if (CollisionSystem::willPlayerDie(simState)) {
                return frame;
            }
        }
        
        return maxFrames;
    }
};

// ============================================================================
// PLAYER STATE SYNCHRONIZATION
// ============================================================================

class PlayerStateSync {
public:
    static void syncFromGame(PlayerObject* player) {
        if (!player) return;
        
        // Position and velocity
        g_currentPlayerState.x = player->getPositionX();
        g_currentPlayerState.y = player->getPositionY();
        g_currentPlayerState.yVelocity = player->m_yVelocity;
        g_currentPlayerState.rotation = player->getRotation();
        
        // State flags
        g_currentPlayerState.isUpsideDown = player->m_isUpsideDown;
        g_currentPlayerState.isMini = player->m_vehicleSize != 1.0f;
        g_currentPlayerState.isOnGround = player->m_isOnGround;
        
        // Gamemode detection
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
        
        // Speed detection
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
    CCLabelBMFont* m_positionLabel = nullptr;
    CCLabelBMFont* m_modeLabel = nullptr;
    CCDrawNode* m_trajectoryDraw = nullptr;
    CCDrawNode* m_hazardDraw = nullptr;
    
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
        
        // Background panel
        auto* bgPanel = CCLayerColor::create(ccc4(0, 0, 0, 100), 180, 85);
        bgPanel->setPosition({3, 235});
        addChild(bgPanel, 0);
        
        // Status label (ON/OFF)
        m_statusLabel = CCLabelBMFont::create("AutoBot: OFF", "bigFont.fnt");
        m_statusLabel->setScale(0.4f);
        m_statusLabel->setAnchorPoint({0, 1});
        m_statusLabel->setPosition({8, 315});
        addChild(m_statusLabel, 100);
        
        // Stats label (clicks/attempts/progress)
        m_statsLabel = CCLabelBMFont::create("Clicks: 0", "chatFont.fnt");
        m_statsLabel->setScale(0.5f);
        m_statsLabel->setAnchorPoint({0, 1});
        m_statsLabel->setPosition({8, 292});
        addChild(m_statsLabel, 100);
        
        // Position label
        m_positionLabel = CCLabelBMFont::create("X: 0  Y: 0", "chatFont.fnt");
        m_positionLabel->setScale(0.45f);
        m_positionLabel->setAnchorPoint({0, 1});
        m_positionLabel->setPosition({8, 272});
        addChild(m_positionLabel, 100);
        
        // Mode label
        m_modeLabel = CCLabelBMFont::create("Mode: Cube", "chatFont.fnt");
        m_modeLabel->setScale(0.45f);
        m_modeLabel->setAnchorPoint({0, 1});
        m_modeLabel->setPosition({8, 254});
        addChild(m_modeLabel, 100);
        
        // Trajectory draw node
        m_trajectoryDraw = CCDrawNode::create();
        addChild(m_trajectoryDraw, 50);
        
        // Hazard draw node
        m_hazardDraw = CCDrawNode::create();
        addChild(m_hazardDraw, 49);
        
        // Schedule update
        scheduleUpdate();
        
        return true;
    }
    
    void update(float dt) override {
        updateLabels();
        
        if (g_debugDraw && g_botEnabled) {
            updateTrajectoryVisualization();
            updateHazardVisualization();
        } else {
            m_trajectoryDraw->clear();
            m_hazardDraw->clear();
        }
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
        g_currentProgress = (g_currentPlayerState.x / g_levelLength) * 100.0f;
        if (g_currentProgress > g_bestProgress) {
            g_bestProgress = g_currentProgress;
        }
        snprintf(statsBuffer, sizeof(statsBuffer),
            "Clicks: %d | Best: %.1f%%",
            g_totalClicks, g_bestProgress);
        m_statsLabel->setString(statsBuffer);
        
        // Position
        char posBuffer[64];
        snprintf(posBuffer, sizeof(posBuffer),
            "X: %.0f  Y: %.0f",
            g_currentPlayerState.x, g_currentPlayerState.y);
        m_positionLabel->setString(posBuffer);
        
        // Mode
        static const char* modeNames[] = {
            "Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing"
        };
        char modeBuffer[64];
        snprintf(modeBuffer, sizeof(modeBuffer),
            "%s %s %s",
            modeNames[(int)g_currentPlayerState.gameMode],
            g_currentPlayerState.isMini ? "[Mini]" : "",
            g_currentPlayerState.isOnGround ? "[Ground]" : "[Air]");
        m_modeLabel->setString(modeBuffer);
    }
    
    void updateTrajectoryVisualization() {
        m_trajectoryDraw->clear();
        
        // Simulate "no click" trajectory (RED)
        {
            PlayerState simState = g_currentPlayerState.copy();
            CCPoint prevPoint = ccp(simState.x, simState.y);
            
            for (int i = 0; i < 60; i++) {
                PhysicsEngine::simulateFrame(simState, false);
                CCPoint newPoint = ccp(simState.x, simState.y);
                
                float alpha = 1.0f - (float)i / 60.0f;
                m_trajectoryDraw->drawSegment(
                    prevPoint, newPoint,
                    1.5f,
                    ccc4f(1.0f, 0.3f, 0.3f, alpha * 0.7f)
                );
                
                prevPoint = newPoint;
                
                if (CollisionSystem::willPlayerDie(simState)) {
                    // Draw death marker
                    m_trajectoryDraw->drawDot(newPoint, 5.0f, ccc4f(1.0f, 0.0f, 0.0f, 0.8f));
                    break;
                }
            }
        }
        
        // Simulate "click" trajectory (GREEN)
        {
            PlayerState simState = g_currentPlayerState.copy();
            CCPoint prevPoint = ccp(simState.x, simState.y);
            
            for (int i = 0; i < 60; i++) {
                bool hold = (i < 12);
                PhysicsEngine::simulateFrame(simState, hold);
                InteractionHandler::handleInteractions(simState, hold);
                CCPoint newPoint = ccp(simState.x, simState.y);
                
                float alpha = 1.0f - (float)i / 60.0f;
                m_trajectoryDraw->drawSegment(
                    prevPoint, newPoint,
                    1.5f,
                    ccc4f(0.3f, 1.0f, 0.3f, alpha * 0.7f)
                );
                
                prevPoint = newPoint;
                
                if (CollisionSystem::willPlayerDie(simState)) {
                    m_trajectoryDraw->drawDot(newPoint, 5.0f, ccc4f(0.0f, 1.0f, 0.0f, 0.8f));
                    break;
                }
            }
        }
        
        // Draw click history
        for (const auto& click : g_clickHistory) {
            m_trajectoryDraw->drawDot(ccp(click.first, click.second), 4.0f, ccc4f(1.0f, 1.0f, 0.0f, 0.6f));
        }
        
        // Draw player position
        m_trajectoryDraw->drawDot(
            ccp(g_currentPlayerState.x, g_currentPlayerState.y),
            8.0f,
            ccc4f(1.0f, 1.0f, 1.0f, 0.9f)
        );
    }
    
    void updateHazardVisualization() {
        m_hazardDraw->clear();
        
        float viewStart = g_currentPlayerState.x - 100;
        float viewEnd = g_currentPlayerState.x + 500;
        
        for (const auto& obj : g_levelObjects) {
            if (obj.x < viewStart || obj.x > viewEnd) continue;
            
            if (obj.isHazard) {
                // Red for hazards
                m_hazardDraw->drawDot(
                    ccp(obj.x, obj.y),
                    obj.width / 3.0f,
                    ccc4f(1.0f, 0.0f, 0.0f, 0.4f)
                );
            }
            else if (obj.isOrb) {
                // Yellow for orbs
                m_hazardDraw->drawDot(
                    ccp(obj.x, obj.y),
                    12.0f,
                    ccc4f(1.0f, 1.0f, 0.0f, 0.5f)
                );
            }
            else if (obj.isPad) {
                // Magenta for pads
                m_hazardDraw->drawDot(
                    ccp(obj.x, obj.y),
                    10.0f,
                    ccc4f(1.0f, 0.0f, 1.0f, 0.5f)
                );
            }
            else if (obj.isPortal) {
                // Cyan for portals
                m_hazardDraw->drawDot(
                    ccp(obj.x, obj.y),
                    15.0f,
                    ccc4f(0.0f, 1.0f, 1.0f, 0.5f)
                );
            }
        }
    }
};

static BotOverlay* g_botOverlay = nullptr;

// ============================================================================
// PAUSE LAYER MODIFICATION - Add bot controls
// ============================================================================

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        // Create menu for bot controls
        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 200);
        
        // Bot toggle
        auto* botOnSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* botOffSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        
        auto* botToggle = CCMenuItemToggler::create(
            botOffSpr, botOnSpr,
            this, menu_selector(BotPauseLayer::onToggleBot)
        );
        botToggle->setPosition({winSize.width - 35, winSize.height - 35});
        botToggle->toggle(g_botEnabled);
        menu->addChild(botToggle);
        
        auto* botLabel = CCLabelBMFont::create("AutoBot", "bigFont.fnt");
        botLabel->setScale(0.3f);
        botLabel->setPosition({winSize.width - 35, winSize.height - 55});
        this->addChild(botLabel, 200);
        
        // Debug toggle
        auto* dbgOnSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* dbgOffSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        dbgOnSpr->setScale(0.7f);
        dbgOffSpr->setScale(0.7f);
        
        auto* dbgToggle = CCMenuItemToggler::create(
            dbgOffSpr, dbgOnSpr,
            this, menu_selector(BotPauseLayer::onToggleDebug)
        );
        dbgToggle->setPosition({winSize.width - 35, winSize.height - 80});
        dbgToggle->toggle(g_debugDraw);
        menu->addChild(dbgToggle);
        
        auto* dbgLabel = CCLabelBMFont::create("Debug", "bigFont.fnt");
        dbgLabel->setScale(0.25f);
        dbgLabel->setPosition({winSize.width - 35, winSize.height - 96});
        this->addChild(dbgLabel, 200);
    }
    
    void onToggleBot(CCObject*) {
        g_botEnabled = !g_botEnabled;
        log::info("AutoBot: {} (menu)", g_botEnabled ? "ON" : "OFF");
        
        if (!g_botEnabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
    }
    
    void onToggleDebug(CCObject*) {
        g_debugDraw = !g_debugDraw;
        log::info("Debug: {}", g_debugDraw ? "ON" : "OFF");
    }
};

// ============================================================================
// PLAY LAYER MODIFICATION - Main bot logic
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        
        log::info("AutoBot: Level initialized");
        
        // Reset state
        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;
        g_totalClicks = 0;
        g_totalAttempts = 0;
        g_bestProgress = 0.0f;
        g_currentProgress = 0.0f;
        g_clickHistory.clear();
        
        // Create overlay
        g_botOverlay = BotOverlay::create();
        g_botOverlay->setZOrder(9999);
        this->addChild(g_botOverlay);
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        LevelAnalyzer::analyzeLevel(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        log::info("AutoBot: Reset (attempt {})", g_totalAttempts + 1);
        
        // Release held input
        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        // Update counters
        g_totalAttempts++;
        g_frameCounter = 0;
        g_totalClicks = 0;
        g_clickHistory.clear();
        
        // Re-analyze if needed
        if (!g_levelAnalyzed) {
            LevelAnalyzer::analyzeLevel(this);
        }
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        
        g_frameCounter++;
        
        // Early exits
        if (!g_botEnabled) return;
        if (!m_player1) return;
        if (m_isPaused || m_hasCompletedLevel || m_player1->m_isDead) return;
        if (!g_levelAnalyzed) return;
        
        // Sync state from game
        PlayerStateSync::syncFromGame(m_player1);
        
        // Get bot decision
        bool shouldClick = BotBrain::shouldClick(g_currentPlayerState);
        
        // Apply input if changed
        if (shouldClick != g_isHolding) {
            auto* gj = GJBaseGameLayer::get();
            if (gj) {
                gj->handleButton(shouldClick, 1, true);
                g_isHolding = shouldClick;
                
                if (shouldClick) {
                    g_totalClicks++;
                    
                    // Add to click history
                    g_clickHistory.push_back({g_currentPlayerState.x, g_currentPlayerState.y});
                    if (g_clickHistory.size() > MAX_CLICK_HISTORY) {
                        g_clickHistory.pop_front();
                    }
                    
                    log::info("CLICK #{} @ x={:.0f} y={:.0f}",
                        g_totalClicks, g_currentPlayerState.x, g_currentPlayerState.y);
                }
            }
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        
        log::info("========================================");
        log::info("  AutoBot: LEVEL COMPLETE!");
        log::info("  Clicks: {}", g_totalClicks);
        log::info("  Attempts: {}", g_totalAttempts);
        log::info("========================================");
        
        Notification::create(
            fmt::format("AutoBot Complete!\n{} clicks | {} attempts",
                g_totalClicks, g_totalAttempts),
            NotificationIcon::Success
        )->show();
    }
    
    void onQuit() {
        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        g_levelAnalyzed = false;
        g_botOverlay = nullptr;
        
        PlayLayer::onQuit();
    }
};

// ============================================================================
// KEYBOARD HANDLER
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            // F8 - Toggle bot
            if (key == KEY_F8) {
                g_botEnabled = !g_botEnabled;
                
                log::info("AutoBot: {} (F8)", g_botEnabled ? "ON" : "OFF");
                
                if (!g_botEnabled && g_isHolding) {
                    if (auto* gj = GJBaseGameLayer::get()) {
                        gj->handleButton(false, 1, true);
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
                    g_debugDraw ? "Debug: ON" : "Debug: OFF",
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
    log::info("========================================================");
    log::info("           AutoBot Mod Loaded Successfully!");
    log::info("========================================================");
    log::info("");
    log::info("  CONTROLS:");
    log::info("    F8 ............. Toggle AutoBot ON/OFF");
    log::info("    F9 ............. Toggle Debug Visualization");
    log::info("    Pause Menu ..... Bot toggle buttons");
    log::info("");
    log::info("  FEATURES:");
    log::info("    * Full physics simulation for all 8 gamemodes");
    log::info("    * Automatic hazard detection and avoidance");
    log::info("    * Orb and pad interaction handling");
    log::info("    * Portal support (gamemode, speed, gravity, size)");
    log::info("    * Visual trajectory debugging");
    log::info("    * Click history visualization");
    log::info("    * Progress tracking");
    log::info("");
    log::info("  DEBUG COLORS:");
    log::info("    Red line ....... No-click trajectory");
    log::info("    Green line ..... Click trajectory");
    log::info("    Red dots ....... Hazards");
    log::info("    Yellow dots .... Orbs");
    log::info("    Magenta dots ... Pads");
    log::info("    Cyan dots ...... Portals");
    log::info("    Yellow dots .... Click history");
    log::info("");
    log::info("========================================================");
}
