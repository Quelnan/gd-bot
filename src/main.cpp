#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <queue>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <functional>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace geode::prelude;

// ============================================================================
// LOGGING SYSTEM
// ============================================================================

class PathfinderLogger {
private:
    std::ofstream m_logFile;
    std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_startTime;
    
public:
    static PathfinderLogger& get() {
        static PathfinderLogger instance;
        return instance;
    }
    
    void init() {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto path = Mod::get()->getSaveDir() / "pathfinder.log";
        m_logFile.open(path, std::ios::out | std::ios::trunc);
        m_startTime = std::chrono::steady_clock::now();
        log("INFO", "Pathfinder Logger Initialized");
        log("INFO", "Log file: " + path.string());
    }
    
    void log(const std::string& level, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_logFile.is_open()) return;
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count();
        
        m_logFile << "[" << std::setw(8) << elapsed << "ms] [" << level << "] " << message << std::endl;
        m_logFile.flush();
        
        // Also log to Geode console
        if (level == "ERROR") {
            geode::log::error("{}", message);
        } else if (level == "WARN") {
            geode::log::warn("{}", message);
        } else if (level == "DEBUG") {
            geode::log::debug("{}", message);
        } else {
            geode::log::info("{}", message);
        }
    }
    
    void logf(const std::string& level, const char* fmt, ...) {
        char buffer[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        log(level, std::string(buffer));
    }
    
    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
    }
};

#define LOG_INFO(msg) PathfinderLogger::get().log("INFO", msg)
#define LOG_DEBUG(msg) PathfinderLogger::get().log("DEBUG", msg)
#define LOG_WARN(msg) PathfinderLogger::get().log("WARN", msg)
#define LOG_ERROR(msg) PathfinderLogger::get().log("ERROR", msg)
#define LOG_INFOF(...) PathfinderLogger::get().logf("INFO", __VA_ARGS__)
#define LOG_DEBUGF(...) PathfinderLogger::get().logf("DEBUG", __VA_ARGS__)
#define LOG_WARNF(...) PathfinderLogger::get().logf("WARN", __VA_ARGS__)
#define LOG_ERRORF(...) PathfinderLogger::get().logf("ERROR", __VA_ARGS__)

// ============================================================================
// CONSTANTS - GD PHYSICS (Extracted from game analysis)
// ============================================================================

namespace GDPhysics {
    // Frame rate
    constexpr double PHYSICS_FPS = 240.0;
    constexpr double PHYSICS_DT = 1.0 / PHYSICS_FPS;
    
    // Gravity values per speed
    constexpr double GRAVITY_NORMAL = 0.958199;
    
    // Speed multipliers (blocks per second at 60fps base)
    constexpr double SPEED_SLOW = 0.7;      // 0.5x
    constexpr double SPEED_NORMAL = 0.9;    // 1x
    constexpr double SPEED_FAST = 1.1;      // 2x
    constexpr double SPEED_FASTER = 1.3;    // 3x
    constexpr double SPEED_FASTEST = 1.6;   // 4x
    
    // X velocity per speed (blocks per frame at 240fps)
    constexpr double XVEL_SLOW = 5.770002 / 240.0;
    constexpr double XVEL_NORMAL = 5.770002 * 1.0 / 240.0;
    constexpr double XVEL_FAST = 5.770002 * 1.243 / 240.0;
    constexpr double XVEL_FASTER = 5.770002 * 1.502 / 240.0;
    constexpr double XVEL_FASTEST = 5.770002 * 1.849 / 240.0;
    
    // Cube physics
    constexpr double CUBE_JUMP_VELOCITY = 11.180032;
    constexpr double CUBE_GRAVITY = 0.958199;
    
    // Ship physics
    constexpr double SHIP_FLY_ACCEL = 0.8;
    constexpr double SHIP_GRAVITY = 0.5;
    constexpr double SHIP_MAX_VELOCITY = 8.0;
    
    // Ball physics
    constexpr double BALL_JUMP_VELOCITY = 11.180032;
    
    // UFO physics
    constexpr double UFO_JUMP_VELOCITY = 7.0;
    constexpr double UFO_GRAVITY = 0.5;
    
    // Wave physics
    constexpr double WAVE_TRAIL_SPEED = 1.0;
    
    // Robot physics
    constexpr double ROBOT_JUMP_VELOCITY_MIN = 7.0;
    constexpr double ROBOT_JUMP_VELOCITY_MAX = 14.0;
    constexpr double ROBOT_JUMP_ACCEL = 0.5;
    
    // Spider physics
    constexpr double SPIDER_TELEPORT_DISTANCE = 30.0;
    
    // Swing physics
    constexpr double SWING_GRAVITY = 0.6;
    constexpr double SWING_FLY_ACCEL = 0.9;
    
    // Player hitbox sizes (in game units)
    constexpr double CUBE_HITBOX_WIDTH = 25.0;
    constexpr double CUBE_HITBOX_HEIGHT = 25.0;
    constexpr double SHIP_HITBOX_WIDTH = 25.0;
    constexpr double SHIP_HITBOX_HEIGHT = 20.0;
    constexpr double BALL_HITBOX_WIDTH = 20.0;
    constexpr double BALL_HITBOX_HEIGHT = 20.0;
    constexpr double UFO_HITBOX_WIDTH = 25.0;
    constexpr double UFO_HITBOX_HEIGHT = 22.0;
    constexpr double WAVE_HITBOX_WIDTH = 14.0;
    constexpr double WAVE_HITBOX_HEIGHT = 14.0;
    constexpr double ROBOT_HITBOX_WIDTH = 25.0;
    constexpr double ROBOT_HITBOX_HEIGHT = 30.0;
    constexpr double SPIDER_HITBOX_WIDTH = 25.0;
    constexpr double SPIDER_HITBOX_HEIGHT = 25.0;
    constexpr double SWING_HITBOX_WIDTH = 25.0;
    constexpr double SWING_HITBOX_HEIGHT = 20.0;
    
    // Block size
    constexpr double BLOCK_SIZE = 30.0;
}

// ============================================================================
// ENUMS AND TYPES
// ============================================================================

enum class GameMode {
    Cube = 0,
    Ship = 1,
    Ball = 2,
    UFO = 3,
    Wave = 4,
    Robot = 5,
    Spider = 6,
    Swing = 7
};

enum class SpeedType {
    Slow = 0,
    Normal = 1,
    Fast = 2,
    Faster = 3,
    Fastest = 4
};

enum class ObjectType {
    Unknown = 0,
    Solid = 1,
    Spike = 2,
    SpikeHalf = 3,
    Orb = 4,
    Pad = 5,
    Portal = 6,
    Trigger = 7,
    Slope = 8,
    SlopeInverted = 9,
    MovingPlatform = 10,
    Saw = 11,
    Decoration = 12
};

enum class OrbType {
    Yellow = 0,
    Blue = 1,
    Pink = 2,
    Red = 3,
    Green = 4,
    Black = 5,
    Dash = 6,
    Spider = 7
};

enum class PadType {
    Yellow = 0,
    Blue = 1,
    Pink = 2,
    Red = 3
};

enum class PortalType {
    Cube = 0,
    Ship = 1,
    Ball = 2,
    UFO = 3,
    Wave = 4,
    Robot = 5,
    Spider = 6,
    Swing = 7,
    GravityFlip = 8,
    GravityNormal = 9,
    MirrorOn = 10,
    MirrorOff = 11,
    SizeNormal = 12,
    SizeMini = 13,
    DualOn = 14,
    DualOff = 15,
    SpeedSlow = 16,
    SpeedNormal = 17,
    SpeedFast = 18,
    SpeedFaster = 19,
    SpeedFastest = 20,
    Teleport = 21
};

// ============================================================================
// GEOMETRY STRUCTURES
// ============================================================================

struct Rect {
    double x, y, width, height;
    
    Rect() : x(0), y(0), width(0), height(0) {}
    Rect(double x, double y, double w, double h) : x(x), y(y), width(w), height(h) {}
    
    double left() const { return x; }
    double right() const { return x + width; }
    double bottom() const { return y; }
    double top() const { return y + height; }
    double centerX() const { return x + width / 2.0; }
    double centerY() const { return y + height / 2.0; }
    
    bool intersects(const Rect& other) const {
        return !(right() <= other.left() || other.right() <= left() ||
                 top() <= other.bottom() || other.top() <= bottom());
    }
    
    bool contains(double px, double py) const {
        return px >= left() && px <= right() && py >= bottom() && py <= top();
    }
};

struct Triangle {
    double x1, y1, x2, y2, x3, y3;
    
    Triangle() : x1(0), y1(0), x2(0), y2(0), x3(0), y3(0) {}
    Triangle(double x1, double y1, double x2, double y2, double x3, double y3)
        : x1(x1), y1(y1), x2(x2), y2(y2), x3(x3), y3(y3) {}
    
    bool containsPoint(double px, double py) const {
        auto sign = [](double p1x, double p1y, double p2x, double p2y, double p3x, double p3y) {
            return (p1x - p3x) * (p2y - p3y) - (p2x - p3x) * (p1y - p3y);
        };
        
        double d1 = sign(px, py, x1, y1, x2, y2);
        double d2 = sign(px, py, x2, y2, x3, y3);
        double d3 = sign(px, py, x3, y3, x1, y1);
        
        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        
        return !(hasNeg && hasPos);
    }
    
    bool intersectsRect(const Rect& rect) const {
        // Check if any corner of rect is in triangle
        if (containsPoint(rect.left(), rect.bottom()) ||
            containsPoint(rect.right(), rect.bottom()) ||
            containsPoint(rect.left(), rect.top()) ||
            containsPoint(rect.right(), rect.top())) {
            return true;
        }
        
        // Check if any triangle vertex is in rect
        if (rect.contains(x1, y1) || rect.contains(x2, y2) || rect.contains(x3, y3)) {
            return true;
        }
        
        // TODO: Full edge intersection check
        return false;
    }
};

// ============================================================================
// SIMULATED OBJECT
// ============================================================================

struct SimObject {
    int id;
    ObjectType type;
    Rect hitbox;
    Triangle triangleHitbox;  // For spikes and slopes
    bool hasTriangleHitbox;
    bool isFlippedX;
    bool isFlippedY;
    double rotation;
    
    // Object-specific data
    OrbType orbType;
    PadType padType;
    PortalType portalType;
    int groupID;
    bool isActive;
    double scaleX, scaleY;
    
    // Trigger data
    int targetGroup;
    double duration;
    double moveX, moveY;
    int easing;
    
    // Teleport data
    double teleportX, teleportY;
    
    SimObject() : id(0), type(ObjectType::Unknown), hasTriangleHitbox(false),
                  isFlippedX(false), isFlippedY(false), rotation(0),
                  orbType(OrbType::Yellow), padType(PadType::Yellow),
                  portalType(PortalType::Cube), groupID(0), isActive(true),
                  scaleX(1), scaleY(1), targetGroup(0), duration(0),
                  moveX(0), moveY(0), easing(0), teleportX(0), teleportY(0) {}
};

// ============================================================================
// SIMULATION STATE
// ============================================================================

struct PlayerState {
    double x, y;
    double velX, velY;
    double rotation;
    double rotationVel;
    GameMode gameMode;
    SpeedType speed;
    bool isGravityFlipped;
    bool isMini;
    bool isDual;
    bool isHolding;
    bool wasHolding;
    bool isOnGround;
    bool isDead;
    bool hasWon;
    int frame;
    
    // Robot-specific
    double robotJumpTime;
    bool robotJumping;
    
    // Orb state
    bool canUseOrb;
    int lastOrbFrame;
    
    // Input state
    bool clickedThisFrame;
    bool releasedThisFrame;
    
    PlayerState() : x(0), y(105), velX(0), velY(0), rotation(0), rotationVel(0),
                    gameMode(GameMode::Cube), speed(SpeedType::Normal),
                    isGravityFlipped(false), isMini(false), isDual(false),
                    isHolding(false), wasHolding(false), isOnGround(true),
                    isDead(false), hasWon(false), frame(0),
                    robotJumpTime(0), robotJumping(false), canUseOrb(false),
                    lastOrbFrame(-100), clickedThisFrame(false), releasedThisFrame(false) {}
    
    Rect getHitbox() const {
        double w, h;
        double scale = isMini ? 0.6 : 1.0;
        
        switch (gameMode) {
            case GameMode::Cube:
                w = GDPhysics::CUBE_HITBOX_WIDTH * scale;
                h = GDPhysics::CUBE_HITBOX_HEIGHT * scale;
                break;
            case GameMode::Ship:
                w = GDPhysics::SHIP_HITBOX_WIDTH * scale;
                h = GDPhysics::SHIP_HITBOX_HEIGHT * scale;
                break;
            case GameMode::Ball:
                w = GDPhysics::BALL_HITBOX_WIDTH * scale;
                h = GDPhysics::BALL_HITBOX_HEIGHT * scale;
                break;
            case GameMode::UFO:
                w = GDPhysics::UFO_HITBOX_WIDTH * scale;
                h = GDPhysics::UFO_HITBOX_HEIGHT * scale;
                break;
            case GameMode::Wave:
                w = GDPhysics::WAVE_HITBOX_WIDTH * scale;
                h = GDPhysics::WAVE_HITBOX_HEIGHT * scale;
                break;
            case GameMode::Robot:
                w = GDPhysics::ROBOT_HITBOX_WIDTH * scale;
                h = GDPhysics::ROBOT_HITBOX_HEIGHT * scale;
                break;
            case GameMode::Spider:
                w = GDPhysics::SPIDER_HITBOX_WIDTH * scale;
                h = GDPhysics::SPIDER_HITBOX_HEIGHT * scale;
                break;
            case GameMode::Swing:
                w = GDPhysics::SWING_HITBOX_WIDTH * scale;
                h = GDPhysics::SWING_HITBOX_HEIGHT * scale;
                break;
            default:
                w = GDPhysics::CUBE_HITBOX_WIDTH;
                h = GDPhysics::CUBE_HITBOX_HEIGHT;
        }
        
        return Rect(x - w/2, y - h/2, w, h);
    }
    
    // For state hashing in pathfinder
    size_t hash() const {
        size_t h = 0;
        auto hashCombine = [&h](size_t value) {
            h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        
        hashCombine(std::hash<int>{}(static_cast<int>(x * 10)));
        hashCombine(std::hash<int>{}(static_cast<int>(y * 10)));
        hashCombine(std::hash<int>{}(static_cast<int>(velY * 10)));
        hashCombine(std::hash<int>{}(static_cast<int>(gameMode)));
        hashCombine(std::hash<bool>{}(isGravityFlipped));
        hashCombine(std::hash<bool>{}(isOnGround));
        
        return h;
    }
    
    bool operator==(const PlayerState& other) const {
        return std::abs(x - other.x) < 0.1 &&
               std::abs(y - other.y) < 0.5 &&
               std::abs(velY - other.velY) < 0.5 &&
               gameMode == other.gameMode &&
               isGravityFlipped == other.isGravityFlipped;
    }
};

// ============================================================================
// LEVEL DATA
// ============================================================================

struct LevelData {
    std::vector<SimObject> objects;
    std::vector<SimObject> hazards;
    std::vector<SimObject> solids;
    std::vector<SimObject> orbs;
    std::vector<SimObject> pads;
    std::vector<SimObject> portals;
    std::vector<SimObject> triggers;
    
    double levelLength;
    double startX;
    double startY;
    GameMode startMode;
    SpeedType startSpeed;
    bool startFlipped;
    bool startMini;
    
    LevelData() : levelLength(0), startX(0), startY(105),
                  startMode(GameMode::Cube), startSpeed(SpeedType::Normal),
                  startFlipped(false), startMini(false) {}
    
    void clear() {
        objects.clear();
        hazards.clear();
        solids.clear();
        orbs.clear();
        pads.clear();
        portals.clear();
        triggers.clear();
        levelLength = 0;
    }
    
    void sortByX() {
        auto sortFunc = [](const SimObject& a, const SimObject& b) {
            return a.hitbox.x < b.hitbox.x;
        };
        
        std::sort(hazards.begin(), hazards.end(), sortFunc);
        std::sort(solids.begin(), solids.end(), sortFunc);
        std::sort(orbs.begin(), orbs.end(), sortFunc);
        std::sort(pads.begin(), pads.end(), sortFunc);
        std::sort(portals.begin(), portals.end(), sortFunc);
    }
};

// ============================================================================
// OBJECT ID MAPPINGS (GD Object IDs)
// ============================================================================

namespace ObjectIDs {
    // Spikes
    const std::unordered_set<int> SPIKES = {8, 39, 103, 392, 140, 1332, 1333, 1334, 1711, 1712, 1713, 1714, 1715, 1716, 1717};
    const std::unordered_set<int> HALF_SPIKES = {9, 61};
    
    // Saws
    const std::unordered_set<int> SAWS = {88, 89, 98, 397, 398, 399, 1705, 1706, 1707, 1708};
    
    // Solid blocks (basic)
    const std::unordered_set<int> BASIC_SOLIDS = {1, 2, 3, 4, 5, 6, 7, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50};
    
    // Slopes
    const std::unordered_set<int> SLOPES = {289, 290, 291, 292, 293, 294, 295, 296, 297, 298};
    const std::unordered_set<int> SLOPES_INVERTED = {299, 300, 301, 302, 303, 304, 305, 306, 307, 308};
    
    // Orbs
    const std::unordered_map<int, OrbType> ORB_IDS = {
        {36, OrbType::Yellow},
        {141, OrbType::Blue},
        {1022, OrbType::Pink},
        {1333, OrbType::Red},
        {1704, OrbType::Green},
        {1594, OrbType::Black},
        {1704, OrbType::Dash}
    };
    
    // Pads
    const std::unordered_map<int, PadType> PAD_IDS = {
        {35, PadType::Yellow},
        {140, PadType::Blue},
        {1332, PadType::Pink},
        {1333, PadType::Red}
    };
    
    // Portals - Gamemodes
    const std::unordered_map<int, PortalType> PORTAL_IDS = {
        // Gamemode portals
        {12, PortalType::Cube},
        {13, PortalType::Ship},
        {47, PortalType::Ball},
        {111, PortalType::UFO},
        {660, PortalType::Wave},
        {745, PortalType::Robot},
        {1331, PortalType::Spider},
        {1933, PortalType::Swing},
        
        // Gravity
        {10, PortalType::GravityFlip},
        {11, PortalType::GravityNormal},
        
        // Mirror
        {45, PortalType::MirrorOn},
        {46, PortalType::MirrorOff},
        
        // Size
        {99, PortalType::SizeMini},
        {101, PortalType::SizeNormal},
        
        // Dual
        {286, PortalType::DualOn},
        {287, PortalType::DualOff},
        
        // Speed
        {200, PortalType::SpeedSlow},
        {201, PortalType::SpeedNormal},
        {202, PortalType::SpeedFast},
        {203, PortalType::SpeedFaster},
        {1334, PortalType::SpeedFastest},
        
        // Teleport
        {747, PortalType::Teleport}
    };
    
    // All hazard IDs
    inline bool isHazard(int id) {
        return SPIKES.count(id) || HALF_SPIKES.count(id) || SAWS.count(id);
    }
    
    inline bool isSolid(int id) {
        return BASIC_SOLIDS.count(id) || SLOPES.count(id) || SLOPES_INVERTED.count(id);
    }
    
    inline bool isSlope(int id) {
        return SLOPES.count(id) || SLOPES_INVERTED.count(id);
    }
}

// ============================================================================
// GD SIMULATOR - Core Physics Engine
// ============================================================================

class GDSimulator {
private:
    LevelData m_level;
    std::mutex m_mutex;
    
    double getXVelocity(SpeedType speed) const {
        switch (speed) {
            case SpeedType::Slow: return GDPhysics::XVEL_SLOW * 240.0;
            case SpeedType::Normal: return GDPhysics::XVEL_NORMAL * 240.0;
            case SpeedType::Fast: return GDPhysics::XVEL_FAST * 240.0;
            case SpeedType::Faster: return GDPhysics::XVEL_FASTER * 240.0;
            case SpeedType::Fastest: return GDPhysics::XVEL_FASTEST * 240.0;
            default: return GDPhysics::XVEL_NORMAL * 240.0;
        }
    }
    
    double getGravity(GameMode mode, bool mini) const {
        double baseGravity = GDPhysics::CUBE_GRAVITY;
        double scale = mini ? 0.8 : 1.0;
        
        switch (mode) {
            case GameMode::Cube:
            case GameMode::Ball:
            case GameMode::Robot:
            case GameMode::Spider:
                return baseGravity * scale;
            case GameMode::Ship:
                return GDPhysics::SHIP_GRAVITY * scale;
            case GameMode::UFO:
                return GDPhysics::UFO_GRAVITY * scale;
            case GameMode::Wave:
                return 0; // Wave doesn't use gravity
            case GameMode::Swing:
                return GDPhysics::SWING_GRAVITY * scale;
            default:
                return baseGravity;
        }
    }
    
public:
    static GDSimulator& get() {
        static GDSimulator instance;
        return instance;
    }
    
    void loadLevel(GJBaseGameLayer* layer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_level.clear();
        
        LOG_INFO("Loading level for simulation...");
        
        if (!layer) {
            LOG_ERROR("Layer is null!");
            return;
        }
        
        // Get all objects from the level
        auto* objects = layer->m_objects;
        if (!objects) {
            LOG_ERROR("Objects array is null!");
            return;
        }
        
        int objectCount = objects->count();
        LOG_INFOF("Processing %d objects", objectCount);
        
        double maxX = 0;
        
        for (int i = 0; i < objectCount; i++) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!obj) continue;
            
            SimObject simObj;
            simObj.id = obj->m_objectID;
            simObj.rotation = obj->getRotation();
            simObj.isFlippedX = obj->m_isFlippedX;
            simObj.isFlippedY = obj->m_isFlippedY;
            simObj.scaleX = obj->getScaleX();
            simObj.scaleY = obj->getScaleY();
            simObj.groupID = 0; // Would need to extract from obj->m_groups
            simObj.isActive = obj->m_isActive;
            
            // Get position and size
            auto pos = obj->getPosition();
            auto contentSize = obj->getContentSize();
            
            // Calculate hitbox
            double width = contentSize.width * std::abs(simObj.scaleX);
            double height = contentSize.height * std::abs(simObj.scaleY);
            
            // Adjust for anchor point
            double anchorX = obj->getAnchorPoint().x;
            double anchorY = obj->getAnchorPoint().y;
            
            simObj.hitbox = Rect(
                pos.x - width * anchorX,
                pos.y - height * anchorY,
                width,
                height
            );
            
            // Determine object type
            int objID = obj->m_objectID;
            
            if (ObjectIDs::isHazard(objID)) {
                simObj.type = ObjectType::Spike;
                
                // Create triangle hitbox for spikes
                double spikeWidth = GDPhysics::BLOCK_SIZE * 0.8;
                double spikeHeight = GDPhysics::BLOCK_SIZE * 0.8;
                
                if (simObj.isFlippedY || simObj.rotation == 180) {
                    // Upside down spike
                    simObj.triangleHitbox = Triangle(
                        pos.x - spikeWidth/2, pos.y + spikeHeight/2,
                        pos.x + spikeWidth/2, pos.y + spikeHeight/2,
                        pos.x, pos.y - spikeHeight/2
                    );
                } else {
                    // Normal spike
                    simObj.triangleHitbox = Triangle(
                        pos.x - spikeWidth/2, pos.y - spikeHeight/2,
                        pos.x + spikeWidth/2, pos.y - spikeHeight/2,
                        pos.x, pos.y + spikeHeight/2
                    );
                }
                simObj.hasTriangleHitbox = true;
                
                m_level.hazards.push_back(simObj);
            }
            else if (ObjectIDs::isSolid(objID)) {
                if (ObjectIDs::isSlope(objID)) {
                    simObj.type = ObjectType::Slope;
                    simObj.hasTriangleHitbox = true;
                    
                    // Create triangle for slope
                    if (ObjectIDs::SLOPES.count(objID)) {
                        simObj.triangleHitbox = Triangle(
                            pos.x - width/2, pos.y - height/2,
                            pos.x + width/2, pos.y - height/2,
                            pos.x + width/2, pos.y + height/2
                        );
                    } else {
                        simObj.triangleHitbox = Triangle(
                            pos.x - width/2, pos.y - height/2,
                            pos.x - width/2, pos.y + height/2,
                            pos.x + width/2, pos.y - height/2
                        );
                    }
                } else {
                    simObj.type = ObjectType::Solid;
                    simObj.hasTriangleHitbox = false;
                }
                m_level.solids.push_back(simObj);
            }
            else if (ObjectIDs::ORB_IDS.count(objID)) {
                simObj.type = ObjectType::Orb;
                simObj.orbType = ObjectIDs::ORB_IDS.at(objID);
                m_level.orbs.push_back(simObj);
            }
            else if (ObjectIDs::PAD_IDS.count(objID)) {
                simObj.type = ObjectType::Pad;
                simObj.padType = ObjectIDs::PAD_IDS.at(objID);
                m_level.pads.push_back(simObj);
            }
            else if (ObjectIDs::PORTAL_IDS.count(objID)) {
                simObj.type = ObjectType::Portal;
                simObj.portalType = ObjectIDs::PORTAL_IDS.at(objID);
                
                // Handle teleport portal
                if (simObj.portalType == PortalType::Teleport) {
                    // Would need to find linked teleport
                    // For now, we'll handle this during simulation
                }
                
                m_level.portals.push_back(simObj);
            }
            
            m_level.objects.push_back(simObj);
            
            if (pos.x > maxX) maxX = pos.x;
        }
        
        m_level.levelLength = maxX + 100;
        m_level.sortByX();
        
        // Get starting conditions
        m_level.startX = 0;
        m_level.startY = 105;
        m_level.startMode = GameMode::Cube;
        m_level.startSpeed = SpeedType::Normal;
        m_level.startFlipped = false;
        m_level.startMini = false;
        
        LOG_INFOF("Level loaded: %zu hazards, %zu solids, %zu orbs, %zu pads, %zu portals",
                  m_level.hazards.size(), m_level.solids.size(),
                  m_level.orbs.size(), m_level.pads.size(), m_level.portals.size());
        LOG_INFOF("Level length: %.2f", m_level.levelLength);
    }
    
    PlayerState createInitialState() const {
        PlayerState state;
        state.x = m_level.startX;
        state.y = m_level.startY;
        state.velX = getXVelocity(m_level.startSpeed);
        state.velY = 0;
        state.gameMode = m_level.startMode;
        state.speed = m_level.startSpeed;
        state.isGravityFlipped = m_level.startFlipped;
        state.isMini = m_level.startMini;
        state.isOnGround = true;
        state.frame = 0;
        return state;
    }
    
    void simulateFrame(PlayerState& state, bool input) {
        if (state.isDead || state.hasWon) return;
        
        // Update input state
        state.clickedThisFrame = input && !state.wasHolding;
        state.releasedThisFrame = !input && state.wasHolding;
        state.wasHolding = state.isHolding;
        state.isHolding = input;
        
        double dt = GDPhysics::PHYSICS_DT;
        double gravity = getGravity(state.gameMode, state.isMini);
        int gravityDir = state.isGravityFlipped ? 1 : -1;
        
        // Process physics based on gamemode
        switch (state.gameMode) {
            case GameMode::Cube:
                simulateCube(state, input, gravity, gravityDir, dt);
                break;
            case GameMode::Ship:
                simulateShip(state, input, gravity, gravityDir, dt);
                break;
            case GameMode::Ball:
                simulateBall(state, input, gravity, gravityDir, dt);
                break;
            case GameMode::UFO:
                simulateUFO(state, input, gravity, gravityDir, dt);
                break;
            case GameMode::Wave:
                simulateWave(state, input, gravityDir, dt);
                break;
            case GameMode::Robot:
                simulateRobot(state, input, gravity, gravityDir, dt);
                break;
            case GameMode::Spider:
                simulateSpider(state, input, gravity, gravityDir, dt);
                break;
            case GameMode::Swing:
                simulateSwing(state, input, gravity, gravityDir, dt);
                break;
        }
        
        // Move horizontally
        state.x += state.velX * dt;
        
        // Check collisions
        checkCollisions(state);
        
        // Check orbs and pads
        checkOrbs(state, input);
        checkPads(state);
        checkPortals(state);
        
        // Check win condition
        if (state.x >= m_level.levelLength - 50) {
            state.hasWon = true;
        }
        
        state.frame++;
    }
    
    void simulateCube(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        // Apply gravity
        state.velY += gravity * gravityDir * dt * 60.0;
        
        // Jump if on ground and input
        if (state.isOnGround && state.clickedThisFrame) {
            double jumpVel = GDPhysics::CUBE_JUMP_VELOCITY;
            if (state.isMini) jumpVel *= 0.8;
            state.velY = jumpVel * -gravityDir;
            state.isOnGround = false;
        }
        
        // Apply velocity
        state.y += state.velY * dt * 60.0;
        
        // Clamp velocity
        if (std::abs(state.velY) > 20) {
            state.velY = 20 * (state.velY > 0 ? 1 : -1);
        }
    }
    
    void simulateShip(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        if (input) {
            // Flying up
            state.velY += GDPhysics::SHIP_FLY_ACCEL * -gravityDir * dt * 60.0;
        } else {
            // Falling
            state.velY += gravity * gravityDir * dt * 60.0;
        }
        
        // Clamp velocity
        if (std::abs(state.velY) > GDPhysics::SHIP_MAX_VELOCITY) {
            state.velY = GDPhysics::SHIP_MAX_VELOCITY * (state.velY > 0 ? 1 : -1);
        }
        
        state.y += state.velY * dt * 60.0;
    }
    
    void simulateBall(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        // Apply gravity
        state.velY += gravity * gravityDir * dt * 60.0;
        
        // Switch gravity on ground click
        if (state.isOnGround && state.clickedThisFrame) {
            state.isGravityFlipped = !state.isGravityFlipped;
        }
        
        state.y += state.velY * dt * 60.0;
    }
    
    void simulateUFO(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        // Apply gravity
        state.velY += gravity * gravityDir * dt * 60.0;
        
        // Jump burst on click
        if (state.clickedThisFrame) {
            double jumpVel = GDPhysics::UFO_JUMP_VELOCITY;
            if (state.isMini) jumpVel *= 0.8;
            state.velY = jumpVel * -gravityDir;
        }
        
        state.y += state.velY * dt * 60.0;
    }
    
    void simulateWave(PlayerState& state, bool input, int gravityDir, double dt) {
        double waveSpeed = GDPhysics::WAVE_TRAIL_SPEED;
        if (state.isMini) waveSpeed *= 0.8;
        
        if (input) {
            state.velY = waveSpeed * -gravityDir * 60.0 * dt * 50;
        } else {
            state.velY = waveSpeed * gravityDir * 60.0 * dt * 50;
        }
        
        state.y += state.velY;
    }
    
    void simulateRobot(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        // Apply gravity
        state.velY += gravity * gravityDir * dt * 60.0;
        
        // Variable jump
        if (state.isOnGround && state.clickedThisFrame) {
            state.robotJumping = true;
            state.robotJumpTime = 0;
            state.velY = GDPhysics::ROBOT_JUMP_VELOCITY_MIN * -gravityDir;
            state.isOnGround = false;
        }
        
        if (state.robotJumping && input && state.robotJumpTime < 0.3) {
            state.robotJumpTime += dt;
            state.velY += GDPhysics::ROBOT_JUMP_ACCEL * -gravityDir;
            if (std::abs(state.velY) > GDPhysics::ROBOT_JUMP_VELOCITY_MAX) {
                state.velY = GDPhysics::ROBOT_JUMP_VELOCITY_MAX * -gravityDir;
            }
        }
        
        if (!input || state.robotJumpTime >= 0.3) {
            state.robotJumping = false;
        }
        
        state.y += state.velY * dt * 60.0;
    }
    
    void simulateSpider(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        // Apply gravity
        state.velY += gravity * gravityDir * dt * 60.0;
        
        // Teleport to ceiling/floor on click
        if (state.isOnGround && state.clickedThisFrame) {
            state.isGravityFlipped = !state.isGravityFlipped;
            // Find nearest surface in opposite direction
            double targetY = findSpiderTeleportY(state);
            if (targetY != state.y) {
                state.y = targetY;
                state.velY = 0;
            }
        }
        
        state.y += state.velY * dt * 60.0;
    }
    
    void simulateSwing(PlayerState& state, bool input, double gravity, int gravityDir, double dt) {
        if (input) {
            // Fly in opposite direction of gravity
            state.velY += GDPhysics::SWING_FLY_ACCEL * -gravityDir * dt * 60.0;
        } else {
            // Fall normally
            state.velY += gravity * gravityDir * dt * 60.0;
        }
        
        // Clamp velocity
        if (std::abs(state.velY) > GDPhysics::SHIP_MAX_VELOCITY) {
            state.velY = GDPhysics::SHIP_MAX_VELOCITY * (state.velY > 0 ? 1 : -1);
        }
        
        state.y += state.velY * dt * 60.0;
        
        // Swing changes gravity on ground hit
        if (state.isOnGround) {
            state.isGravityFlipped = !state.isGravityFlipped;
            state.velY = 0;
        }
    }
    
    double findSpiderTeleportY(const PlayerState& state) const {
        Rect playerHitbox = state.getHitbox();
        double searchDir = state.isGravityFlipped ? -1 : 1;
        double targetY = state.y;
        
        // Search for collision in the direction we'll flip to
        for (double y = state.y; y > 0 && y < 1000; y += searchDir * 5) {
            Rect testHitbox = playerHitbox;
            testHitbox.y = y - testHitbox.height / 2;
            
            for (const auto& solid : m_level.solids) {
                if (testHitbox.intersects(solid.hitbox)) {
                    return y - searchDir * 15;
                }
            }
        }
        
        return targetY;
    }
    
    void checkCollisions(PlayerState& state) {
        Rect playerHitbox = state.getHitbox();
        state.isOnGround = false;
        
        // Check hazards
        for (const auto& hazard : m_level.hazards) {
            if (hazard.hitbox.right() < state.x - 50) continue;
            if (hazard.hitbox.left() > state.x + 100) break;
            
            bool collides = false;
            if (hazard.hasTriangleHitbox) {
                collides = hazard.triangleHitbox.intersectsRect(playerHitbox);
            } else {
                collides = playerHitbox.intersects(hazard.hitbox);
            }
            
            if (collides) {
                state.isDead = true;
                LOG_DEBUGF("Player died at x=%.2f, y=%.2f, frame=%d (hit hazard id=%d)",
                          state.x, state.y, state.frame, hazard.id);
                return;
            }
        }
        
        // Check solids
        for (const auto& solid : m_level.solids) {
            if (solid.hitbox.right() < state.x - 50) continue;
            if (solid.hitbox.left() > state.x + 100) break;
            
            if (playerHitbox.intersects(solid.hitbox)) {
                // Resolve collision
                resolveCollision(state, solid);
            }
        }
        
        // Check floor/ceiling
        double floorY = 90;  // Ground level
        double ceilY = 500;  // Ceiling
        
        if (state.y - playerHitbox.height/2 <= floorY) {
            state.y = floorY + playerHitbox.height/2;
            if (!state.isGravityFlipped) {
                state.isOnGround = true;
                state.velY = 0;
            }
        }
        
        if (state.y + playerHitbox.height/2 >= ceilY) {
            state.y = ceilY - playerHitbox.height/2;
            if (state.isGravityFlipped) {
                state.isOnGround = true;
                state.velY = 0;
            }
        }
    }
    
    void resolveCollision(PlayerState& state, const SimObject& solid) {
        Rect playerHitbox = state.getHitbox();
        
        // Calculate overlap
        double overlapLeft = playerHitbox.right() - solid.hitbox.left();
        double overlapRight = solid.hitbox.right() - playerHitbox.left();
        double overlapBottom = playerHitbox.top() - solid.hitbox.bottom();
        double overlapTop = solid.hitbox.top() - playerHitbox.bottom();
        
        // Find smallest overlap
        double minOverlap = std::min({overlapLeft, overlapRight, overlapBottom, overlapTop});
        
        if (minOverlap == overlapBottom && state.velY > 0) {
            // Hit ceiling
            state.y = solid.hitbox.bottom() - playerHitbox.height/2 - 0.1;
            if (state.isGravityFlipped) {
                state.isOnGround = true;
            }
            state.velY = 0;
        } else if (minOverlap == overlapTop && state.velY < 0) {
            // Hit floor
            state.y = solid.hitbox.top() + playerHitbox.height/2 + 0.1;
            if (!state.isGravityFlipped) {
                state.isOnGround = true;
            }
            state.velY = 0;
        } else if (minOverlap == overlapLeft || minOverlap == overlapRight) {
            // Side collision - death in most cases
            state.isDead = true;
            LOG_DEBUGF("Player died at x=%.2f, y=%.2f (side collision)", state.x, state.y);
        }
    }
    
    void checkOrbs(PlayerState& state, bool input) {
        if (!input) return;
        
        Rect playerHitbox = state.getHitbox();
        
        for (const auto& orb : m_level.orbs) {
            if (orb.hitbox.right() < state.x - 50) continue;
            if (orb.hitbox.left() > state.x + 100) break;
            
            if (playerHitbox.intersects(orb.hitbox)) {
                if (state.frame - state.lastOrbFrame < 5) continue;  // Prevent double-trigger
                
                switch (orb.orbType) {
                    case OrbType::Yellow:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * (state.isGravityFlipped ? 1 : -1);
                        break;
                    case OrbType::Blue:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * (state.isGravityFlipped ? -1 : 1);
                        state.isGravityFlipped = !state.isGravityFlipped;
                        break;
                    case OrbType::Pink:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * 0.7 * (state.isGravityFlipped ? 1 : -1);
                        break;
                    case OrbType::Red:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * 1.3 * (state.isGravityFlipped ? 1 : -1);
                        break;
                    case OrbType::Green:
                        state.velY = -state.velY * 1.1;
                        state.isGravityFlipped = !state.isGravityFlipped;
                        break;
                    case OrbType::Black:
                        state.velY = -GDPhysics::CUBE_JUMP_VELOCITY * (state.isGravityFlipped ? 1 : -1);
                        break;
                    default:
                        break;
                }
                
                state.lastOrbFrame = state.frame;
                state.isOnGround = false;
                LOG_DEBUGF("Used orb at x=%.2f, frame=%d", state.x, state.frame);
            }
        }
    }
    
    void checkPads(PlayerState& state) {
        Rect playerHitbox = state.getHitbox();
        
        for (const auto& pad : m_level.pads) {
            if (pad.hitbox.right() < state.x - 50) continue;
            if (pad.hitbox.left() > state.x + 100) break;
            
            if (playerHitbox.intersects(pad.hitbox)) {
                switch (pad.padType) {
                    case PadType::Yellow:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * 1.1 * (state.isGravityFlipped ? 1 : -1);
                        break;
                    case PadType::Blue:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * 1.1 * (state.isGravityFlipped ? -1 : 1);
                        state.isGravityFlipped = !state.isGravityFlipped;
                        break;
                    case PadType::Pink:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * 0.8 * (state.isGravityFlipped ? 1 : -1);
                        break;
                    case PadType::Red:
                        state.velY = GDPhysics::CUBE_JUMP_VELOCITY * 1.4 * (state.isGravityFlipped ? 1 : -1);
                        break;
                }
                
                state.isOnGround = false;
            }
        }
    }
    
    void checkPortals(PlayerState& state) {
        Rect playerHitbox = state.getHitbox();
        
        for (const auto& portal : m_level.portals) {
            if (portal.hitbox.right() < state.x - 50) continue;
            if (portal.hitbox.left() > state.x + 100) break;
            
            if (playerHitbox.intersects(portal.hitbox)) {
                switch (portal.portalType) {
                    case PortalType::Cube: state.gameMode = GameMode::Cube; break;
                    case PortalType::Ship: state.gameMode = GameMode::Ship; break;
                    case PortalType::Ball: state.gameMode = GameMode::Ball; break;
                    case PortalType::UFO: state.gameMode = GameMode::UFO; break;
                    case PortalType::Wave: state.gameMode = GameMode::Wave; break;
                    case PortalType::Robot: state.gameMode = GameMode::Robot; break;
                    case PortalType::Spider: state.gameMode = GameMode::Spider; break;
                    case PortalType::Swing: state.gameMode = GameMode::Swing; break;
                    
                    case PortalType::GravityFlip: state.isGravityFlipped = true; break;
                    case PortalType::GravityNormal: state.isGravityFlipped = false; break;
                    
                    case PortalType::SizeMini: state.isMini = true; break;
                    case PortalType::SizeNormal: state.isMini = false; break;
                    
                    case PortalType::SpeedSlow: 
                        state.speed = SpeedType::Slow; 
                        state.velX = getXVelocity(state.speed);
                        break;
                    case PortalType::SpeedNormal: 
                        state.speed = SpeedType::Normal; 
                        state.velX = getXVelocity(state.speed);
                        break;
                    case PortalType::SpeedFast: 
                        state.speed = SpeedType::Fast; 
                        state.velX = getXVelocity(state.speed);
                        break;
                    case PortalType::SpeedFaster: 
                        state.speed = SpeedType::Faster; 
                        state.velX = getXVelocity(state.speed);
                        break;
                    case PortalType::SpeedFastest: 
                        state.speed = SpeedType::Fastest; 
                        state.velX = getXVelocity(state.speed);
                        break;
                        
                    case PortalType::Teleport:
                        state.x = portal.teleportX;
                        state.y = portal.teleportY;
                        break;
                        
                    default:
                        break;
                }
                
                LOG_DEBUGF("Portal activated at x=%.2f, type=%d", state.x, (int)portal.portalType);
            }
        }
    }
    
    const LevelData& getLevelData() const { return m_level; }
    double getLevelLength() const { return m_level.levelLength; }
};

// ============================================================================
// PATHFINDER - Beam Search / BFS Algorithm
// ============================================================================

struct PathState {
    PlayerState state;
    std::vector<bool> inputs;  // Input history
    
    double fitness() const {
        if (state.hasWon) return 1000000.0 + 100000.0 - inputs.size();
        if (state.isDead) return state.x;
        return state.x;
    }
};

class Pathfinder {
private:
    std::atomic<bool> m_isRunning;
    std::atomic<bool> m_foundPath;
    std::atomic<double> m_progress;
    std::vector<bool> m_solution;
    std::mutex m_solutionMutex;
    std::thread m_thread;
    
    static constexpr int BEAM_WIDTH = 5000;
    static constexpr int MAX_FRAMES = 50000;
    static constexpr int PHYSICS_SUBSTEPS = 4;  // 240fps / 4 = 60fps input resolution
    
public:
    static Pathfinder& get() {
        static Pathfinder instance;
        return instance;
    }
    
    Pathfinder() : m_isRunning(false), m_foundPath(false), m_progress(0) {}
    
    ~Pathfinder() {
        stop();
    }
    
    void start() {
        if (m_isRunning) return;
        
        m_isRunning = true;
        m_foundPath = false;
        m_progress = 0;
        m_solution.clear();
        
        m_thread = std::thread(&Pathfinder::findPath, this);
    }
    
    void stop() {
        m_isRunning = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    
    bool isRunning() const { return m_isRunning; }
    bool foundPath() const { return m_foundPath; }
    double getProgress() const { return m_progress; }
    
    std::vector<bool> getSolution() {
        std::lock_guard<std::mutex> lock(m_solutionMutex);
        return m_solution;
    }
    
private:
    void findPath() {
        LOG_INFO("Pathfinder started");
        auto startTime = std::chrono::steady_clock::now();
        
        GDSimulator& sim = GDSimulator::get();
        double levelLength = sim.getLevelLength();
        
        // Initialize beam with both press and no-press states
        std::vector<PathState> beam;
        
        PathState initial;
        initial.state = sim.createInitialState();
        initial.inputs.clear();
        
        PathState initialPress = initial;
        
        beam.push_back(initial);
        beam.push_back(initialPress);
        
        int frameCount = 0;
        double bestProgress = 0;
        
        while (m_isRunning && frameCount < MAX_FRAMES) {
            std::vector<PathState> nextBeam;
            nextBeam.reserve(beam.size() * 2);
            
            for (auto& pathState : beam) {
                if (pathState.state.isDead) continue;
                
                if (pathState.state.hasWon) {
                    // Found solution!
                    std::lock_guard<std::mutex> lock(m_solutionMutex);
                    m_solution = pathState.inputs;
                    m_foundPath = true;
                    m_isRunning = false;
                    
                    auto endTime = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
                    LOG_INFOF("Path found! Frames: %zu, Time: %lldms", m_solution.size(), duration);
                    return;
                }
                
                // Try both input states
                for (int inputChoice = 0; inputChoice < 2; inputChoice++) {
                    bool input = (inputChoice == 1);
                    
                    PathState newState = pathState;
                    
                    // Simulate physics substeps
                    for (int substep = 0; substep < PHYSICS_SUBSTEPS; substep++) {
                        sim.simulateFrame(newState.state, input);
                        if (newState.state.isDead || newState.state.hasWon) break;
                    }
                    
                    newState.inputs.push_back(input);
                    
                    if (!newState.state.isDead || newState.state.x > bestProgress * 0.9) {
                        nextBeam.push_back(std::move(newState));
                    }
                }
            }
            
            if (nextBeam.empty()) {
                LOG_ERROR("No valid states remaining!");
                break;
            }
            
            // Sort by fitness and keep best
            std::sort(nextBeam.begin(), nextBeam.end(),
                     [](const PathState& a, const PathState& b) {
                         return a.fitness() > b.fitness();
                     });
            
            if (nextBeam.size() > BEAM_WIDTH) {
                nextBeam.resize(BEAM_WIDTH);
            }
            
            beam = std::move(nextBeam);
            
            // Update progress
            if (!beam.empty()) {
                double currentBest = beam[0].state.x;
                if (currentBest > bestProgress) {
                    bestProgress = currentBest;
                    m_progress = bestProgress / levelLength;
                    LOG_DEBUGF("Progress: %.2f%% (x=%.2f)", m_progress * 100.0, bestProgress);
                }
            }
            
            frameCount++;
            
            // Log every 1000 frames
            if (frameCount % 1000 == 0) {
                LOG_INFOF("Frame %d, beam size: %zu, best x: %.2f", 
                         frameCount, beam.size(), bestProgress);
            }
        }
        
        // No perfect solution found, use best attempt
        if (!beam.empty()) {
            std::lock_guard<std::mutex> lock(m_solutionMutex);
            m_solution = beam[0].inputs;
            LOG_WARNF("Pathfinder finished without winning. Best progress: %.2f%%", m_progress * 100.0);
        } else {
            LOG_ERROR("Pathfinder failed - no valid states");
        }
        
        m_isRunning = false;
    }
};

// ============================================================================
// REPLAY SYSTEM
// ============================================================================

class ReplaySystem {
private:
    std::vector<bool> m_inputs;
    size_t m_currentFrame;
    bool m_isPlaying;
    int m_subframe;
    
public:
    static ReplaySystem& get() {
        static ReplaySystem instance;
        return instance;
    }
    
    ReplaySystem() : m_currentFrame(0), m_isPlaying(false), m_subframe(0) {}
    
    void loadReplay(const std::vector<bool>& inputs) {
        m_inputs = inputs;
        m_currentFrame = 0;
        m_subframe = 0;
        LOG_INFOF("Replay loaded with %zu inputs", inputs.size());
    }
    
    void start() {
        m_isPlaying = true;
        m_currentFrame = 0;
        m_subframe = 0;
        LOG_INFO("Replay started");
    }
    
    void stop() {
        m_isPlaying = false;
        LOG_INFO("Replay stopped");
    }
    
    bool isPlaying() const { return m_isPlaying; }
    
    bool getCurrentInput() {
        if (!m_isPlaying || m_currentFrame >= m_inputs.size()) {
            return false;
        }
        
        return m_inputs[m_currentFrame];
    }
    
    void advanceFrame() {
        if (!m_isPlaying) return;
        
        m_subframe++;
        if (m_subframe >= 4) {  // 4 substeps per input frame
            m_subframe = 0;
            m_currentFrame++;
            
            if (m_currentFrame >= m_inputs.size()) {
                m_isPlaying = false;
                LOG_INFO("Replay finished");
            }
        }
    }
    
    float getProgress() const {
        if (m_inputs.empty()) return 0;
        return static_cast<float>(m_currentFrame) / m_inputs.size();
    }
};

// ============================================================================
// UI POPUP
// ============================================================================

class PathfinderPopup : public Popup<> {
protected:
    bool setup() override {
        setTitle("Pathfinder");
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        // Status label
        m_statusLabel = CCLabelBMFont::create("Ready", "bigFont.fnt");
        m_statusLabel->setPosition(ccp(0, 30));
        m_statusLabel->setScale(0.5f);
        m_mainLayer->addChild(m_statusLabel);
        
        // Progress bar background
        auto progressBg = CCSprite::create("GJ_progressBar_001.png");
        progressBg->setPosition(ccp(0, 0));
        progressBg->setScaleX(0.8f);
        m_mainLayer->addChild(progressBg);
        
        // Progress bar fill
        m_progressBar = CCSprite::create("GJ_progressBar_001.png");
        m_progressBar->setColor(ccc3(0, 255, 0));
        m_progressBar->setPosition(ccp(0, 0));
        m_progressBar->setScaleX(0.0f);
        m_progressBar->setScaleY(0.8f);
        m_mainLayer->addChild(m_progressBar);
        
        // Start button
        auto startBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Start", "goldFont.fnt", "GJ_button_01.png"),
            this,
            menu_selector(PathfinderPopup::onStart)
        );
        startBtn->setPosition(ccp(-60, -50));
        
        // Stop button
        auto stopBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Stop", "goldFont.fnt", "GJ_button_02.png"),
            this,
            menu_selector(PathfinderPopup::onStop)
        );
        stopBtn->setPosition(ccp(60, -50));
        
        auto menu = CCMenu::create();
        menu->addChild(startBtn);
        menu->addChild(stopBtn);
        menu->setPosition(ccp(0, 0));
        m_mainLayer->addChild(menu);
        
        // Schedule update
        schedule(schedule_selector(PathfinderPopup::update), 0.1f);
        
        return true;
    }
    
    void update(float dt) {
        Pathfinder& pf = Pathfinder::get();
        ReplaySystem& replay = ReplaySystem::get();
        
        if (pf.isRunning()) {
            m_statusLabel->setString(fmt::format("Finding path... {:.1f}%", pf.getProgress() * 100).c_str());
            m_progressBar->setScaleX(0.8f * pf.getProgress());
        } else if (pf.foundPath()) {
            if (replay.isPlaying()) {
                m_statusLabel->setString(fmt::format("Playing... {:.1f}%", replay.getProgress() * 100).c_str());
                m_progressBar->setScaleX(0.8f * replay.getProgress());
            } else {
                m_statusLabel->setString("Path found! Click Start to play.");
                m_progressBar->setScaleX(0.8f);
            }
        } else {
            m_statusLabel->setString("Ready");
            m_progressBar->setScaleX(0.0f);
        }
    }
    
    void onStart(CCObject*) {
        Pathfinder& pf = Pathfinder::get();
        ReplaySystem& replay = ReplaySystem::get();
        
        if (pf.foundPath() && !replay.isPlaying()) {
            // Load and start replay
            replay.loadReplay(pf.getSolution());
            
            // Start the level
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                replay.start();
                this->onClose(nullptr);
            }
        } else if (!pf.isRunning()) {
            // Load level data and start pathfinding
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                GDSimulator::get().loadLevel(playLayer);
                pf.start();
            } else {
                FLAlertLayer::create("Error", "Please enter a level first!", "OK")->show();
            }
        }
    }
    
    void onStop(CCObject*) {
        Pathfinder::get().stop();
        ReplaySystem::get().stop();
    }
    
public:
    static PathfinderPopup* create() {
        auto ret = new PathfinderPopup();
        if (ret && ret->initAnchored(300, 200)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
    
private:
    CCLabelBMFont* m_statusLabel;
    CCSprite* m_progressBar;
};

// ============================================================================
// HOOKS
// ============================================================================

class $modify(PathfinderPlayLayer, PlayLayer) {
    struct Fields {
        bool m_pathfinderActive = false;
    };
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        
        LOG_INFO("PlayLayer initialized");
        
        // Load level for simulator
        GDSimulator::get().loadLevel(this);
        
        return true;
    }
    
    void update(float dt) {
        ReplaySystem& replay = ReplaySystem::get();
        
        if (replay.isPlaying()) {
            bool input = replay.getCurrentInput();
            
            // Simulate input
            if (input) {
                if (!m_fields->m_pathfinderActive) {
                    // Press
                    this->pushButton(1, true);
                    m_fields->m_pathfinderActive = true;
                }
            } else {
                if (m_fields->m_pathfinderActive) {
                    // Release
                    this->releaseButton(1, true);
                    m_fields->m_pathfinderActive = false;
                }
            }
            
            replay.advanceFrame();
        }
        
        PlayLayer::update(dt);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        ReplaySystem& replay = ReplaySystem::get();
        if (replay.isPlaying()) {
            replay.stop();
            replay.start();
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        ReplaySystem::get().stop();
        LOG_INFO("Level completed!");
    }
    
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!ReplaySystem::get().isPlaying()) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        
        // Log death during replay
        LOG_WARNF("Player died during replay at x=%.2f", m_player1->getPositionX());
        
        // Don't actually die during pathfinder replay testing
        // Comment this out to allow deaths:
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(PathfinderLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) {
            return false;
        }
        
        // Find the play button
        auto menu = this->getChildByID("left-side-menu");
        if (!menu) menu = this->getChildByID("play-menu");
        
        // Create pathfinder button
        auto sprite = CCSprite::createWithSpriteFrameName("GJ_pathBtn_001.png");
        if (!sprite) {
            sprite = CCSprite::create("GJ_button_01.png");
            auto label = CCLabelBMFont::create("PF", "bigFont.fnt");
            label->setScale(0.5f);
            label->setPosition(sprite->getContentSize() / 2);
            sprite->addChild(label);
        }
        
        auto btn = CCMenuItemSpriteExtra::create(
            sprite,
            this,
            menu_selector(PathfinderLevelInfoLayer::onPathfinder)
        );
        btn->setID("pathfinder-button"_spr);
        
        if (menu) {
            btn->setPosition(ccp(50, 0));
            static_cast<CCMenu*>(menu)->addChild(btn);
        } else {
            // Fallback: Create new menu
            auto newMenu = CCMenu::create();
            newMenu->addChild(btn);
            newMenu->setPosition(ccp(100, 200));
            this->addChild(newMenu, 100);
        }
        
        LOG_INFO("Pathfinder button added to LevelInfoLayer");
        
        return true;
    }
    
    void onPathfinder(CCObject*) {
        LOG_INFO("Pathfinder button clicked");
        
        // Start the level first to load it
        auto level = this->m_level;
        if (level) {
            // Show pathfinder popup
            PathfinderPopup::create()->show();
        }
    }
};

// ============================================================================
// MOD INITIALIZATION
// ============================================================================

$on_mod(Loaded) {
    PathfinderLogger::get().init();
    LOG_INFO("=================================");
    LOG_INFO("GD Pathfinder Mod Loaded");
    LOG_INFO("=================================");
    LOG_INFOF("Mod version: %s", Mod::get()->getVersion().toVString().c_str());
}

$on_mod(Unloaded) {
    Pathfinder::get().stop();
    PathfinderLogger::get().close();
}
