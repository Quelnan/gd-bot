#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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
    bool m_initialized = false;
    
public:
    static PathfinderLogger& get() {
        static PathfinderLogger instance;
        return instance;
    }
    
    void init() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized) return;
        
        auto path = Mod::get()->getSaveDir() / "pathfinder.log";
        m_logFile.open(path, std::ios::out | std::ios::trunc);
        m_startTime = std::chrono::steady_clock::now();
        m_initialized = true;
        
        if (m_logFile.is_open()) {
            m_logFile << "=== Pathfinder Log Started ===" << std::endl;
            m_logFile.flush();
        }
    }
    
    void log(const std::string& level, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count();
        
        std::stringstream ss;
        ss << "[" << std::setw(8) << elapsed << "ms] [" << level << "] " << message;
        std::string fullMsg = ss.str();
        
        if (m_logFile.is_open()) {
            m_logFile << fullMsg << std::endl;
            m_logFile.flush();
        }
        
        if (level == "ERROR") {
            log::error("{}", message);
        } else if (level == "WARN") {
            log::warn("{}", message);
        } else if (level == "DEBUG") {
            log::debug("{}", message);
        } else {
            log::info("{}", message);
        }
    }
    
    template<typename... Args>
    void logFmt(const std::string& level, const std::string& fmtStr, Args&&... args) {
        try {
            log(level, fmt::format(fmt::runtime(fmtStr), std::forward<Args>(args)...));
        } catch (...) {
            log(level, fmtStr);
        }
    }
    
    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logFile.is_open()) {
            m_logFile << "=== Log Closed ===" << std::endl;
            m_logFile.close();
        }
        m_initialized = false;
    }
};

#define LOG_INFO(msg) PathfinderLogger::get().log("INFO", msg)
#define LOG_DEBUG(msg) PathfinderLogger::get().log("DEBUG", msg)
#define LOG_WARN(msg) PathfinderLogger::get().log("WARN", msg)
#define LOG_ERROR(msg) PathfinderLogger::get().log("ERROR", msg)
#define LOG_INFOF(fmtStr, ...) PathfinderLogger::get().logFmt("INFO", fmtStr, __VA_ARGS__)
#define LOG_DEBUGF(fmtStr, ...) PathfinderLogger::get().logFmt("DEBUG", fmtStr, __VA_ARGS__)
#define LOG_WARNF(fmtStr, ...) PathfinderLogger::get().logFmt("WARN", fmtStr, __VA_ARGS__)
#define LOG_ERRORF(fmtStr, ...) PathfinderLogger::get().logFmt("ERROR", fmtStr, __VA_ARGS__)

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

namespace GDConst {
    constexpr double PHYSICS_FPS = 240.0;
    constexpr double PHYSICS_DT = 1.0 / PHYSICS_FPS;
    constexpr double BLOCK_SIZE = 30.0;
    
    constexpr double GRAVITY = 0.958199;
    
    constexpr double XVEL_SLOW = 5.770002 * 0.7;
    constexpr double XVEL_NORMAL = 5.770002;
    constexpr double XVEL_FAST = 5.770002 * 1.243;
    constexpr double XVEL_FASTER = 5.770002 * 1.502;
    constexpr double XVEL_FASTEST = 5.770002 * 1.849;
    
    constexpr double CUBE_JUMP = 11.180032;
    constexpr double SHIP_ACCEL = 0.8;
    constexpr double SHIP_GRAVITY = 0.5;
    constexpr double SHIP_MAX_VEL = 8.0;
    constexpr double UFO_JUMP = 7.0;
    constexpr double WAVE_SPEED = 1.0;
    constexpr double ROBOT_JUMP_MIN = 7.0;
    constexpr double ROBOT_JUMP_MAX = 14.0;
    constexpr double SWING_ACCEL = 0.9;
    constexpr double SWING_GRAVITY = 0.6;
    
    constexpr double CUBE_SIZE = 25.0;
    constexpr double SHIP_WIDTH = 25.0;
    constexpr double SHIP_HEIGHT = 20.0;
    constexpr double BALL_SIZE = 20.0;
    constexpr double UFO_WIDTH = 25.0;
    constexpr double UFO_HEIGHT = 22.0;
    constexpr double WAVE_SIZE = 14.0;
    constexpr double ROBOT_WIDTH = 25.0;
    constexpr double ROBOT_HEIGHT = 30.0;
    constexpr double SPIDER_SIZE = 25.0;
    constexpr double SWING_WIDTH = 25.0;
    constexpr double SWING_HEIGHT = 20.0;
}

// ============================================================================
// ENUMS
// ============================================================================

enum class GameMode : int {
    Cube = 0, Ship = 1, Ball = 2, UFO = 3,
    Wave = 4, Robot = 5, Spider = 6, Swing = 7
};

enum class SpeedType : int {
    Slow = 0, Normal = 1, Fast = 2, Faster = 3, Fastest = 4
};

enum class ObjType : int {
    Unknown = 0, Solid = 1, Hazard = 2, Orb = 3, 
    Pad = 4, Portal = 5, Slope = 6
};

// ============================================================================
// GEOMETRY
// ============================================================================

struct Rect {
    double x, y, w, h;
    
    Rect() : x(0), y(0), w(0), h(0) {}
    Rect(double x_, double y_, double w_, double h_) : x(x_), y(y_), w(w_), h(h_) {}
    
    double left() const { return x; }
    double right() const { return x + w; }
    double bottom() const { return y; }
    double top() const { return y + h; }
    
    bool intersects(const Rect& o) const {
        return !(right() <= o.left() || o.right() <= left() ||
                 top() <= o.bottom() || o.top() <= bottom());
    }
};

struct Tri {
    double x1, y1, x2, y2, x3, y3;
    
    Tri() : x1(0), y1(0), x2(0), y2(0), x3(0), y3(0) {}
    Tri(double ax, double ay, double bx, double by, double cx, double cy)
        : x1(ax), y1(ay), x2(bx), y2(by), x3(cx), y3(cy) {}
    
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
    
    bool intersectsRect(const Rect& r) const {
        if (containsPoint(r.left(), r.bottom()) ||
            containsPoint(r.right(), r.bottom()) ||
            containsPoint(r.left(), r.top()) ||
            containsPoint(r.right(), r.top())) {
            return true;
        }
        if (r.left() <= x1 && x1 <= r.right() && r.bottom() <= y1 && y1 <= r.top()) return true;
        if (r.left() <= x2 && x2 <= r.right() && r.bottom() <= y2 && y2 <= r.top()) return true;
        if (r.left() <= x3 && x3 <= r.right() && r.bottom() <= y3 && y3 <= r.top()) return true;
        return false;
    }
};

// ============================================================================
// SIMULATED OBJECT
// ============================================================================

struct SimObj {
    int id = 0;
    ObjType type = ObjType::Unknown;
    Rect hitbox;
    Tri triHitbox;
    bool hasTri = false;
    bool flipX = false;
    bool flipY = false;
    double rot = 0;
    int subType = 0;
    bool active = true;
};

// ============================================================================
// PLAYER STATE
// ============================================================================

struct PlayerState {
    double x = 0;
    double y = 105;
    double velX = GDConst::XVEL_NORMAL;
    double velY = 0;
    GameMode mode = GameMode::Cube;
    SpeedType speed = SpeedType::Normal;
    bool gravFlip = false;
    bool mini = false;
    bool onGround = true;
    bool dead = false;
    bool won = false;
    bool holding = false;
    bool wasHolding = false;
    int frame = 0;
    int lastOrbFrame = -100;
    double robotJumpTime = 0;
    bool robotJumping = false;
    
    Rect getHitbox() const {
        double w, h;
        double scale = mini ? 0.6 : 1.0;
        
        switch (mode) {
            case GameMode::Cube:
                w = h = GDConst::CUBE_SIZE * scale;
                break;
            case GameMode::Ship:
                w = GDConst::SHIP_WIDTH * scale;
                h = GDConst::SHIP_HEIGHT * scale;
                break;
            case GameMode::Ball:
                w = h = GDConst::BALL_SIZE * scale;
                break;
            case GameMode::UFO:
                w = GDConst::UFO_WIDTH * scale;
                h = GDConst::UFO_HEIGHT * scale;
                break;
            case GameMode::Wave:
                w = h = GDConst::WAVE_SIZE * scale;
                break;
            case GameMode::Robot:
                w = GDConst::ROBOT_WIDTH * scale;
                h = GDConst::ROBOT_HEIGHT * scale;
                break;
            case GameMode::Spider:
                w = h = GDConst::SPIDER_SIZE * scale;
                break;
            case GameMode::Swing:
                w = GDConst::SWING_WIDTH * scale;
                h = GDConst::SWING_HEIGHT * scale;
                break;
            default:
                w = h = GDConst::CUBE_SIZE;
        }
        return Rect(x - w/2, y - h/2, w, h);
    }
    
    double getFitness() const {
        if (won) return 1e9 - frame;
        if (dead) return x;
        return x;
    }
};

// ============================================================================
// OBJECT ID TABLES
// ============================================================================

namespace ObjIDs {
    inline bool isSpike(int id) {
        static const std::unordered_set<int> ids = {
            8, 39, 103, 392, 140, 1332, 1333, 1334, 
            1711, 1712, 1713, 1714, 1715, 1716, 1717
        };
        return ids.count(id) > 0;
    }
    
    inline bool isSaw(int id) {
        static const std::unordered_set<int> ids = {
            88, 89, 98, 397, 398, 399, 1705, 1706, 1707, 1708
        };
        return ids.count(id) > 0;
    }
    
    inline bool isHazard(int id) {
        return isSpike(id) || isSaw(id);
    }
    
    inline bool isSolid(int id) {
        if (id >= 1 && id <= 7) return true;
        if (id >= 40 && id <= 50) return true;
        if (id >= 289 && id <= 308) return true;
        return false;
    }
    
    inline bool isSlope(int id) {
        return id >= 289 && id <= 308;
    }
    
    inline int getOrbType(int id) {
        if (id == 36) return 0;
        if (id == 141) return 1;
        if (id == 1022) return 2;
        if (id == 1333) return 3;
        if (id == 1704) return 4;
        if (id == 1594) return 5;
        return -1;
    }
    
    inline int getPadType(int id) {
        if (id == 35) return 0;
        if (id == 140) return 1;
        if (id == 1332) return 2;
        return -1;
    }
    
    inline int getPortalType(int id) {
        if (id == 12) return 0;
        if (id == 13) return 1;
        if (id == 47) return 2;
        if (id == 111) return 3;
        if (id == 660) return 4;
        if (id == 745) return 5;
        if (id == 1331) return 6;
        if (id == 1933) return 7;
        if (id == 10) return 10;
        if (id == 11) return 11;
        if (id == 99) return 20;
        if (id == 101) return 21;
        if (id == 200) return 30;
        if (id == 201) return 31;
        if (id == 202) return 32;
        if (id == 203) return 33;
        if (id == 1334) return 34;
        return -1;
    }
}

// ============================================================================
// LEVEL DATA
// ============================================================================

struct LevelData {
    std::vector<SimObj> hazards;
    std::vector<SimObj> solids;
    std::vector<SimObj> orbs;
    std::vector<SimObj> pads;
    std::vector<SimObj> portals;
    double length = 0;
    
    void clear() {
        hazards.clear();
        solids.clear();
        orbs.clear();
        pads.clear();
        portals.clear();
        length = 0;
    }
    
    void sortByX() {
        auto cmp = [](const SimObj& a, const SimObj& b) {
            return a.hitbox.x < b.hitbox.x;
        };
        std::sort(hazards.begin(), hazards.end(), cmp);
        std::sort(solids.begin(), solids.end(), cmp);
        std::sort(orbs.begin(), orbs.end(), cmp);
        std::sort(pads.begin(), pads.end(), cmp);
        std::sort(portals.begin(), portals.end(), cmp);
    }
};

// ============================================================================
// SIMULATOR
// ============================================================================

class Simulator {
private:
    LevelData m_level;
    std::mutex m_mutex;
    
    double getXVel(SpeedType s) const {
        switch (s) {
            case SpeedType::Slow: return GDConst::XVEL_SLOW;
            case SpeedType::Normal: return GDConst::XVEL_NORMAL;
            case SpeedType::Fast: return GDConst::XVEL_FAST;
            case SpeedType::Faster: return GDConst::XVEL_FASTER;
            case SpeedType::Fastest: return GDConst::XVEL_FASTEST;
        }
        return GDConst::XVEL_NORMAL;
    }
    
    double getGravity(GameMode m, bool mini) const {
        double g = GDConst::GRAVITY;
        if (mini) g *= 0.8;
        
        switch (m) {
            case GameMode::Ship: return GDConst::SHIP_GRAVITY * (mini ? 0.8 : 1.0);
            case GameMode::Wave: return 0;
            case GameMode::Swing: return GDConst::SWING_GRAVITY * (mini ? 0.8 : 1.0);
            default: return g;
        }
    }
    
public:
    static Simulator& get() {
        static Simulator inst;
        return inst;
    }
    
    void loadLevel(GJBaseGameLayer* layer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_level.clear();
        
        LOG_INFO("Loading level...");
        
        if (!layer || !layer->m_objects) {
            LOG_ERROR("Invalid layer or objects!");
            return;
        }
        
        auto* objects = layer->m_objects;
        int count = objects->count();
        LOG_INFOF("Processing {} objects", count);
        
        double maxX = 0;
        
        for (int i = 0; i < count; i++) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!obj) continue;
            
            int objID = obj->m_objectID;
            auto pos = obj->getPosition();
            auto size = obj->getContentSize();
            double scaleX = std::abs(obj->getScaleX());
            double scaleY = std::abs(obj->getScaleY());
            
            SimObj simObj;
            simObj.id = objID;
            
            simObj.flipX = obj->isFlipX();
            simObj.flipY = obj->isFlipY();
            simObj.rot = obj->getRotation();
            
            double w = size.width * scaleX;
            double h = size.height * scaleY;
            simObj.hitbox = Rect(pos.x - w/2, pos.y - h/2, w, h);
            
            if (ObjIDs::isHazard(objID)) {
                simObj.type = ObjType::Hazard;
                
                if (ObjIDs::isSpike(objID)) {
                    double sw = GDConst::BLOCK_SIZE * 0.8;
                    double sh = GDConst::BLOCK_SIZE * 0.8;
                    
                    bool upsideDown = simObj.flipY || std::abs(std::fmod(simObj.rot, 360.0) - 180.0) < 1.0;
                    
                    if (upsideDown) {
                        simObj.triHitbox = Tri(
                            pos.x - sw/2, pos.y + sh/2,
                            pos.x + sw/2, pos.y + sh/2,
                            pos.x, pos.y - sh/2
                        );
                    } else {
                        simObj.triHitbox = Tri(
                            pos.x - sw/2, pos.y - sh/2,
                            pos.x + sw/2, pos.y - sh/2,
                            pos.x, pos.y + sh/2
                        );
                    }
                    simObj.hasTri = true;
                }
                
                m_level.hazards.push_back(simObj);
            }
            else if (ObjIDs::isSolid(objID)) {
                simObj.type = ObjIDs::isSlope(objID) ? ObjType::Slope : ObjType::Solid;
                m_level.solids.push_back(simObj);
            }
            else {
                int orbType = ObjIDs::getOrbType(objID);
                int padType = ObjIDs::getPadType(objID);
                int portalType = ObjIDs::getPortalType(objID);
                
                if (orbType >= 0) {
                    simObj.type = ObjType::Orb;
                    simObj.subType = orbType;
                    m_level.orbs.push_back(simObj);
                }
                else if (padType >= 0) {
                    simObj.type = ObjType::Pad;
                    simObj.subType = padType;
                    m_level.pads.push_back(simObj);
                }
                else if (portalType >= 0) {
                    simObj.type = ObjType::Portal;
                    simObj.subType = portalType;
                    m_level.portals.push_back(simObj);
                }
            }
            
            if (pos.x > maxX) maxX = pos.x;
        }
        
        m_level.length = maxX + 100;
        m_level.sortByX();
        
        LOG_INFOF("Level loaded: {} hazards, {} solids, {} orbs, {} pads, {} portals",
                  m_level.hazards.size(), m_level.solids.size(),
                  m_level.orbs.size(), m_level.pads.size(), m_level.portals.size());
        LOG_INFOF("Level length: {:.2f}", m_level.length);
    }
    
    PlayerState createInitialState() const {
        PlayerState s;
        s.x = 0;
        s.y = 105;
        s.velX = getXVel(SpeedType::Normal);
        s.velY = 0;
        s.mode = GameMode::Cube;
        s.speed = SpeedType::Normal;
        return s;
    }
    
    void simulateFrame(PlayerState& s, bool input) {
        if (s.dead || s.won) return;
        
        bool clicked = input && !s.wasHolding;
        s.wasHolding = s.holding;
        s.holding = input;
        
        double dt = GDConst::PHYSICS_DT;
        double gravity = getGravity(s.mode, s.mini);
        int gravDir = s.gravFlip ? 1 : -1;
        
        switch (s.mode) {
            case GameMode::Cube:
                s.velY += gravity * gravDir * dt * 60.0;
                if (s.onGround && clicked) {
                    double jv = GDConst::CUBE_JUMP * (s.mini ? 0.8 : 1.0);
                    s.velY = jv * -gravDir;
                    s.onGround = false;
                }
                break;
                
            case GameMode::Ship:
                if (input) {
                    s.velY += GDConst::SHIP_ACCEL * -gravDir * dt * 60.0;
                } else {
                    s.velY += gravity * gravDir * dt * 60.0;
                }
                if (std::abs(s.velY) > GDConst::SHIP_MAX_VEL) {
                    s.velY = GDConst::SHIP_MAX_VEL * (s.velY > 0 ? 1 : -1);
                }
                break;
                
            case GameMode::Ball:
                s.velY += gravity * gravDir * dt * 60.0;
                if (s.onGround && clicked) {
                    s.gravFlip = !s.gravFlip;
                }
                break;
                
            case GameMode::UFO:
                s.velY += gravity * gravDir * dt * 60.0;
                if (clicked) {
                    double jv = GDConst::UFO_JUMP * (s.mini ? 0.8 : 1.0);
                    s.velY = jv * -gravDir;
                }
                break;
                
            case GameMode::Wave: {
                double ws = GDConst::WAVE_SPEED * (s.mini ? 0.8 : 1.0);
                s.velY = input ? (ws * -gravDir * 50) : (ws * gravDir * 50);
                break;
            }
                
            case GameMode::Robot:
                s.velY += gravity * gravDir * dt * 60.0;
                if (s.onGround && clicked) {
                    s.robotJumping = true;
                    s.robotJumpTime = 0;
                    s.velY = GDConst::ROBOT_JUMP_MIN * -gravDir;
                    s.onGround = false;
                }
                if (s.robotJumping && input && s.robotJumpTime < 0.3) {
                    s.robotJumpTime += dt;
                    s.velY += 0.5 * -gravDir;
                    if (std::abs(s.velY) > GDConst::ROBOT_JUMP_MAX) {
                        s.velY = GDConst::ROBOT_JUMP_MAX * -gravDir;
                    }
                }
                if (!input || s.robotJumpTime >= 0.3) {
                    s.robotJumping = false;
                }
                break;
                
            case GameMode::Spider:
                s.velY += gravity * gravDir * dt * 60.0;
                if (s.onGround && clicked) {
                    s.gravFlip = !s.gravFlip;
                    s.velY = 0;
                }
                break;
                
            case GameMode::Swing:
                if (input) {
                    s.velY += GDConst::SWING_ACCEL * -gravDir * dt * 60.0;
                } else {
                    s.velY += GDConst::SWING_GRAVITY * gravDir * dt * 60.0;
                }
                if (std::abs(s.velY) > GDConst::SHIP_MAX_VEL) {
                    s.velY = GDConst::SHIP_MAX_VEL * (s.velY > 0 ? 1 : -1);
                }
                break;
        }
        
        s.y += s.velY * dt * 60.0;
        s.x += s.velX * dt;
        
        if (std::abs(s.velY) > 20) {
            s.velY = 20 * (s.velY > 0 ? 1 : -1);
        }
        
        checkCollisions(s, input);
        
        if (s.x >= m_level.length - 50) {
            s.won = true;
        }
        
        s.frame++;
    }
    
    void checkCollisions(PlayerState& s, bool input) {
        Rect ph = s.getHitbox();
        s.onGround = false;
        
        for (const auto& h : m_level.hazards) {
            if (h.hitbox.right() < s.x - 50) continue;
            if (h.hitbox.left() > s.x + 100) break;
            
            bool col = h.hasTri ? h.triHitbox.intersectsRect(ph) : ph.intersects(h.hitbox);
            if (col) {
                s.dead = true;
                return;
            }
        }
        
        for (const auto& solid : m_level.solids) {
            if (solid.hitbox.right() < s.x - 50) continue;
            if (solid.hitbox.left() > s.x + 100) break;
            
            if (ph.intersects(solid.hitbox)) {
                double overlapTop = solid.hitbox.top() - ph.bottom();
                double overlapBot = ph.top() - solid.hitbox.bottom();
                double overlapL = ph.right() - solid.hitbox.left();
                double overlapR = solid.hitbox.right() - ph.left();
                
                double minOver = std::min({overlapTop, overlapBot, overlapL, overlapR});
                
                if (minOver == overlapTop && s.velY < 0 && !s.gravFlip) {
                    s.y = solid.hitbox.top() + ph.h/2 + 0.1;
                    s.velY = 0;
                    s.onGround = true;
                }
                else if (minOver == overlapBot && s.velY > 0 && s.gravFlip) {
                    s.y = solid.hitbox.bottom() - ph.h/2 - 0.1;
                    s.velY = 0;
                    s.onGround = true;
                }
                else if (minOver == overlapL || minOver == overlapR) {
                    s.dead = true;
                    return;
                }
            }
        }
        
        double floor = 90;
        double ceil = 500;
        
        if (s.y - ph.h/2 <= floor) {
            s.y = floor + ph.h/2;
            if (!s.gravFlip) {
                s.onGround = true;
                s.velY = 0;
            }
        }
        if (s.y + ph.h/2 >= ceil) {
            s.y = ceil - ph.h/2;
            if (s.gravFlip) {
                s.onGround = true;
                s.velY = 0;
            }
        }
        
        if (input) {
            for (const auto& orb : m_level.orbs) {
                if (orb.hitbox.right() < s.x - 50) continue;
                if (orb.hitbox.left() > s.x + 100) break;
                
                if (ph.intersects(orb.hitbox) && s.frame - s.lastOrbFrame > 5) {
                    double jv = GDConst::CUBE_JUMP;
                    int dir = s.gravFlip ? 1 : -1;
                    
                    switch (orb.subType) {
                        case 0: s.velY = jv * -dir; break;
                        case 1: s.velY = jv * dir; s.gravFlip = !s.gravFlip; break;
                        case 2: s.velY = jv * 0.7 * -dir; break;
                        case 3: s.velY = jv * 1.3 * -dir; break;
                        case 4: s.velY = -s.velY * 1.1; s.gravFlip = !s.gravFlip; break;
                        case 5: s.velY = -jv * -dir; break;
                    }
                    s.lastOrbFrame = s.frame;
                    s.onGround = false;
                }
            }
        }
        
        for (const auto& pad : m_level.pads) {
            if (pad.hitbox.right() < s.x - 50) continue;
            if (pad.hitbox.left() > s.x + 100) break;
            
            if (ph.intersects(pad.hitbox)) {
                double jv = GDConst::CUBE_JUMP;
                int dir = s.gravFlip ? 1 : -1;
                
                switch (pad.subType) {
                    case 0: s.velY = jv * 1.1 * -dir; break;
                    case 1: s.velY = jv * 1.1 * dir; s.gravFlip = !s.gravFlip; break;
                    case 2: s.velY = jv * 0.8 * -dir; break;
                }
                s.onGround = false;
            }
        }
        
        for (const auto& p : m_level.portals) {
            if (p.hitbox.right() < s.x - 50) continue;
            if (p.hitbox.left() > s.x + 100) break;
            
            if (ph.intersects(p.hitbox)) {
                int pt = p.subType;
                
                if (pt >= 0 && pt <= 7) {
                    s.mode = static_cast<GameMode>(pt);
                }
                else if (pt == 10) s.gravFlip = true;
                else if (pt == 11) s.gravFlip = false;
                else if (pt == 20) s.mini = true;
                else if (pt == 21) s.mini = false;
                else if (pt == 30) { s.speed = SpeedType::Slow; s.velX = getXVel(s.speed); }
                else if (pt == 31) { s.speed = SpeedType::Normal; s.velX = getXVel(s.speed); }
                else if (pt == 32) { s.speed = SpeedType::Fast; s.velX = getXVel(s.speed); }
                else if (pt == 33) { s.speed = SpeedType::Faster; s.velX = getXVel(s.speed); }
                else if (pt == 34) { s.speed = SpeedType::Fastest; s.velX = getXVel(s.speed); }
            }
        }
    }
    
    double getLevelLength() const { return m_level.length; }
};

// ============================================================================
// PATH STATE
// ============================================================================

struct PathState {
    PlayerState state;
    std::vector<bool> inputs;
    
    double fitness() const {
        return state.getFitness();
    }
};

// ============================================================================
// PATHFINDER
// ============================================================================

class Pathfinder {
private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_found{false};
    std::atomic<double> m_progress{0};
    std::vector<bool> m_solution;
    std::mutex m_mutex;
    std::thread m_thread;
    
    static constexpr int BEAM_WIDTH = 3000;
    static constexpr int MAX_FRAMES = 100000;
    static constexpr int SUBSTEPS = 4;
    
public:
    static Pathfinder& get() {
        static Pathfinder inst;
        return inst;
    }
    
    ~Pathfinder() { stop(); }
    
    void start() {
        if (m_running) return;
        
        m_running = true;
        m_found = false;
        m_progress = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_solution.clear();
        }
        
        m_thread = std::thread(&Pathfinder::findPath, this);
    }
    
    void stop() {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    
    bool isRunning() const { return m_running; }
    bool found() const { return m_found; }
    double progress() const { return m_progress; }
    
    std::vector<bool> getSolution() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_solution;
    }
    
private:
    void findPath() {
        LOG_INFO("Pathfinder started");
        auto startTime = std::chrono::steady_clock::now();
        
        Simulator& sim = Simulator::get();
        double levelLen = sim.getLevelLength();
        
        std::vector<PathState> beam;
        
        PathState init;
        init.state = sim.createInitialState();
        beam.push_back(init);
        
        int frameCount = 0;
        double bestX = 0;
        
        while (m_running && frameCount < MAX_FRAMES) {
            std::vector<PathState> next;
            next.reserve(beam.size() * 2);
            
            for (auto& ps : beam) {
                if (ps.state.dead) continue;
                
                if (ps.state.won) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_solution = ps.inputs;
                    m_found = true;
                    m_running = false;
                    
                    auto endTime = std::chrono::steady_clock::now();
                    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
                    LOG_INFOF("Path found! {} inputs, {}ms", m_solution.size(), dur);
                    return;
                }
                
                for (int inp = 0; inp < 2; inp++) {
                    bool input = (inp == 1);
                    
                    PathState ns = ps;
                    for (int sub = 0; sub < SUBSTEPS; sub++) {
                        sim.simulateFrame(ns.state, input);
                        if (ns.state.dead || ns.state.won) break;
                    }
                    ns.inputs.push_back(input);
                    
                    if (!ns.state.dead || ns.state.x > bestX * 0.8) {
                        next.push_back(std::move(ns));
                    }
                }
            }
            
            if (next.empty()) {
                LOG_ERROR("No valid states!");
                break;
            }
            
            std::sort(next.begin(), next.end(),
                [](const PathState& a, const PathState& b) {
                    return a.fitness() > b.fitness();
                });
            
            if (next.size() > BEAM_WIDTH) {
                next.resize(BEAM_WIDTH);
            }
            
            beam = std::move(next);
            
            if (!beam.empty()) {
                double cur = beam[0].state.x;
                if (cur > bestX) {
                    bestX = cur;
                    m_progress = bestX / levelLen;
                }
            }
            
            frameCount++;
            
            if (frameCount % 500 == 0) {
                LOG_INFOF("Frame {}, beam {}, best x {:.2f}", frameCount, beam.size(), bestX);
            }
        }
        
        if (!beam.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_solution = beam[0].inputs;
            LOG_WARNF("Pathfinder finished. Best: {:.1f}%", m_progress * 100);
        }
        
        m_running = false;
    }
};

// ============================================================================
// REPLAY SYSTEM
// ============================================================================

class Replay {
private:
    std::vector<bool> m_inputs;
    size_t m_frame = 0;
    bool m_playing = false;
    int m_sub = 0;
    
public:
    static Replay& get() {
        static Replay inst;
        return inst;
    }
    
    void load(const std::vector<bool>& inputs) {
        m_inputs = inputs;
        m_frame = 0;
        m_sub = 0;
        LOG_INFOF("Replay loaded: {} inputs", inputs.size());
    }
    
    void start() {
        m_playing = true;
        m_frame = 0;
        m_sub = 0;
        LOG_INFO("Replay started");
    }
    
    void stop() {
        m_playing = false;
        LOG_INFO("Replay stopped");
    }
    
    bool isPlaying() const { return m_playing; }
    
    bool getInput() {
        if (!m_playing || m_frame >= m_inputs.size()) return false;
        return m_inputs[m_frame];
    }
    
    void advance() {
        if (!m_playing) return;
        
        m_sub++;
        if (m_sub >= 4) {
            m_sub = 0;
            m_frame++;
            if (m_frame >= m_inputs.size()) {
                m_playing = false;
                LOG_INFO("Replay finished");
            }
        }
    }
    
    float getProgress() const {
        if (m_inputs.empty()) return 0;
        return static_cast<float>(m_frame) / m_inputs.size();
    }
};

// ============================================================================
// UI POPUP
// ============================================================================

class PathfinderPopup : public Popup<> {
protected:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCSprite* m_progressBar = nullptr;
    
    bool setup() override {
        setTitle("Pathfinder");
        
        m_statusLabel = CCLabelBMFont::create("Ready", "bigFont.fnt");
        m_statusLabel->setPosition(ccp(0, 30));
        m_statusLabel->setScale(0.5f);
        m_mainLayer->addChild(m_statusLabel);
        
        auto progressBg = CCSprite::create("GJ_progressBar_001.png");
        progressBg->setPosition(ccp(0, 0));
        progressBg->setScaleX(0.8f);
        m_mainLayer->addChild(progressBg);
        
        m_progressBar = CCSprite::create("GJ_progressBar_001.png");
        m_progressBar->setColor(ccc3(0, 255, 0));
        m_progressBar->setPosition(ccp(0, 0));
        m_progressBar->setScaleX(0.0f);
        m_progressBar->setScaleY(0.8f);
        m_mainLayer->addChild(m_progressBar);
        
        auto startBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Start", "goldFont.fnt", "GJ_button_01.png"),
            this, menu_selector(PathfinderPopup::onStart)
        );
        startBtn->setPosition(ccp(-60, -50));
        
        auto stopBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Stop", "goldFont.fnt", "GJ_button_02.png"),
            this, menu_selector(PathfinderPopup::onStop)
        );
        stopBtn->setPosition(ccp(60, -50));
        
        auto menu = CCMenu::create();
        menu->addChild(startBtn);
        menu->addChild(stopBtn);
        menu->setPosition(ccp(0, 0));
        m_mainLayer->addChild(menu);
        
        schedule(schedule_selector(PathfinderPopup::onUpdate), 0.1f);
        
        return true;
    }
    
    void onUpdate(float dt) {
        Pathfinder& pf = Pathfinder::get();
        Replay& rp = Replay::get();
        
        std::string status;
        float prog = 0;
        
        if (pf.isRunning()) {
            status = fmt::format("Finding... {:.1f}%", pf.progress() * 100);
            prog = static_cast<float>(pf.progress());
        } else if (pf.found()) {
            if (rp.isPlaying()) {
                status = fmt::format("Playing... {:.1f}%", rp.getProgress() * 100);
                prog = rp.getProgress();
            } else {
                status = "Path found! Click Start";
                prog = 1.0f;
            }
        } else {
            status = "Ready";
            prog = 0;
        }
        
        m_statusLabel->setString(status.c_str());
        m_progressBar->setScaleX(0.8f * prog);
    }
    
    void onStart(CCObject*) {
        Pathfinder& pf = Pathfinder::get();
        Replay& rp = Replay::get();
        
        if (pf.found() && !rp.isPlaying()) {
            rp.load(pf.getSolution());
            
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                rp.start();
                this->onClose(nullptr);
            }
        } else if (!pf.isRunning()) {
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                Simulator::get().loadLevel(playLayer);
                pf.start();
            } else {
                FLAlertLayer::create("Error", "Enter a level first!", "OK")->show();
            }
        }
    }
    
    void onStop(CCObject*) {
        Pathfinder::get().stop();
        Replay::get().stop();
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
};

// ============================================================================
// HOOKS
// ============================================================================

class $modify(PFPlayLayer, PlayLayer) {
    struct Fields {
        bool m_inputActive = false;
    };
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        
        LOG_INFO("PlayLayer initialized");
        Simulator::get().loadLevel(this);
        
        return true;
    }
    
    void update(float dt) {
        Replay& rp = Replay::get();
        
        if (rp.isPlaying() && m_player1) {
            bool inp = rp.getInput();
            
            if (inp && !m_fields->m_inputActive) {
                // Press - use handleButton through the layer
                this->handleButton(true, 1, true);
                m_fields->m_inputActive = true;
            } else if (!inp && m_fields->m_inputActive) {
                // Release
                this->handleButton(false, 1, true);
                m_fields->m_inputActive = false;
            }
            
            rp.advance();
        }
        
        PlayLayer::update(dt);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        Replay& rp = Replay::get();
        if (rp.isPlaying()) {
            rp.stop();
            rp.start();
        }
        m_fields->m_inputActive = false;
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        Replay::get().stop();
        LOG_INFO("Level completed!");
    }
};

class $modify(PFLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) {
            return false;
        }
        
        auto spr = CCSprite::create("GJ_button_01.png");
        spr->setScale(0.8f);
        
        auto label = CCLabelBMFont::create("PF", "bigFont.fnt");
        label->setScale(0.6f);
        label->setPosition(spr->getContentSize() / 2);
        spr->addChild(label);
        
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(PFLevelInfoLayer::onPathfinder)
        );
        btn->setID("pathfinder-btn"_spr);
        
        if (auto menu = this->getChildByID("left-side-menu")) {
            btn->setPosition(ccp(0, -60));
            static_cast<CCMenu*>(menu)->addChild(btn);
        } else {
            auto newMenu = CCMenu::create();
            newMenu->addChild(btn);
            newMenu->setPosition(ccp(50, 250));
            this->addChild(newMenu, 100);
        }
        
        LOG_INFO("Pathfinder button added");
        
        return true;
    }
    
    void onPathfinder(CCObject*) {
        LOG_INFO("Pathfinder button clicked");
        PathfinderPopup::create()->show();
    }
};

// ============================================================================
// MOD INIT
// ============================================================================

$on_mod(Loaded) {
    PathfinderLogger::get().init();
    LOG_INFO("=== GD Pathfinder Loaded ===");
}