#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <vector>
#include <queue>
#include <set>
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

// ============================================================================
// ENUMS AND CONSTANTS
// ============================================================================

enum class GameMode {
    Cube,
    Ship,
    Ball,
    UFO,
    Wave,
    Robot,
    Spider,
    Swing
};

enum class SpeedType {
    Slow,      // 0.7x
    Normal,    // 0.9x
    Fast,      // 1.0x
    Faster,    // 1.1x
    Fastest,   // 1.3x
    SuperFast  // 1.6x+
};

// ============================================================================
// PLAYER STATE
// ============================================================================

struct PlayerState {
    float x = 0;
    float y = 105.0f;
    float yVel = 0;
    float rotation = 0;
    
    GameMode gameMode = GameMode::Cube;
    SpeedType speed = SpeedType::Normal;
    
    bool isUpsideDown = false;
    bool isMini = false;
    bool isDual = false;
    bool isOnGround = true;
    bool isHolding = false;
    bool canJump = true;
    bool isDashing = false;
    
    float orbCooldown = 0;
    int lastOrbID = -1;
    
    // Robot specific
    bool isRobotJumping = false;
    float robotJumpTime = 0;
    
    // Spider specific
    bool hasSpiderTeleported = false;
    
    PlayerState copy() const {
        return *this;
    }
};

// ============================================================================
// LEVEL OBJECT
// ============================================================================

struct LevelObject {
    int id;
    float x, y;
    float width, height;
    float rotation;
    
    bool isHazard = false;
    bool isSolid = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;
    
    int orbType = 0;
    GameMode portalGameMode = GameMode::Cube;
    SpeedType portalSpeed = SpeedType::Normal;
    bool isGravityPortal = false;
    bool isGravityUp = false;
    bool isMiniPortal = false;
    bool isMiniOn = false;
    bool isSpeedPortal = false;
};

// ============================================================================
// PHYSICS ENGINE
// ============================================================================

class PhysicsEngine {
public:
    static constexpr float GRAVITY_CUBE = 0.958199f;
    static constexpr float GRAVITY_SHIP = 0.8f;
    static constexpr float GRAVITY_BALL = 0.6f;
    static constexpr float GRAVITY_UFO = 0.5f;
    static constexpr float GRAVITY_ROBOT = 0.958199f;
    static constexpr float GRAVITY_SPIDER = 0.6f;
    static constexpr float GRAVITY_SWING = 0.7f;
    
    static constexpr float JUMP_VELOCITY = 11.180032f;
    static constexpr float MINI_JUMP_VELOCITY = 9.4f;
    static constexpr float UFO_BOOST = 7.0f;
    
    static constexpr float GROUND_Y = 105.0f;
    static constexpr float CEILING_Y = 2085.0f;
    
    static float getSpeedMultiplier(SpeedType speed) {
        switch (speed) {
            case SpeedType::Slow:      return 251.16f;
            case SpeedType::Normal:    return 311.58f;
            case SpeedType::Fast:      return 387.42f;
            case SpeedType::Faster:    return 468.0f;
            case SpeedType::Fastest:   return 576.0f;
            case SpeedType::SuperFast: return 700.0f;
        }
        return 311.58f;
    }
    
    static float getGravity(GameMode mode, bool mini) {
        float g;
        switch (mode) {
            case GameMode::Cube:   g = GRAVITY_CUBE; break;
            case GameMode::Ship:   g = GRAVITY_SHIP; break;
            case GameMode::Ball:   g = GRAVITY_BALL; break;
            case GameMode::UFO:    g = GRAVITY_UFO; break;
            case GameMode::Wave:   g = 0.0f; break;
            case GameMode::Robot:  g = GRAVITY_ROBOT; break;
            case GameMode::Spider: g = GRAVITY_SPIDER; break;
            case GameMode::Swing:  g = GRAVITY_SWING; break;
            default: g = GRAVITY_CUBE;
        }
        return mini ? g * 0.8f : g;
    }
    
    static float getJumpVelocity(GameMode mode, bool mini) {
        if (mini) {
            return mode == GameMode::Cube ? 9.4f : 7.5f;
        }
        return mode == GameMode::Robot ? 6.0f : JUMP_VELOCITY;
    }
    
    static void simulateFrame(PlayerState& state, bool clicking, float dt = 1.0f/240.0f) {
        float gravity = getGravity(state.gameMode, state.isMini);
        if (state.isUpsideDown) gravity = -gravity;
        
        float xSpeed = getSpeedMultiplier(state.speed) * dt;
        
        if (state.orbCooldown > 0) state.orbCooldown -= dt;
        
        switch (state.gameMode) {
            case GameMode::Cube:
                simulateCube(state, clicking, gravity, dt);
                break;
            case GameMode::Ship:
                simulateShip(state, clicking, gravity, dt);
                break;
            case GameMode::Ball:
                simulateBall(state, clicking, gravity, dt);
                break;
            case GameMode::UFO:
                simulateUFO(state, clicking, gravity, dt);
                break;
            case GameMode::Wave:
                simulateWave(state, clicking, dt);
                break;
            case GameMode::Robot:
                simulateRobot(state, clicking, gravity, dt);
                break;
            case GameMode::Spider:
                simulateSpider(state, clicking, gravity, dt);
                break;
            case GameMode::Swing:
                simulateSwing(state, clicking, gravity, dt);
                break;
        }
        
        state.x += xSpeed;
        handleBoundaries(state);
    }
    
private:
    static void simulateCube(PlayerState& state, bool clicking, float gravity, float dt) {
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
        state.y += state.yVel;
        
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        if ((!state.isUpsideDown && state.y <= groundY) ||
            (state.isUpsideDown && state.y >= groundY)) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        if (clicking && state.canJump && state.isOnGround) {
            float jumpVel = getJumpVelocity(GameMode::Cube, state.isMini);
            state.yVel = state.isUpsideDown ? -jumpVel : jumpVel;
            state.isOnGround = false;
            state.canJump = false;
        }
        
        if (!clicking) {
            state.canJump = true;
        }
        
        if (!state.isOnGround) {
            state.rotation += state.isUpsideDown ? -7.5f : 7.5f;
        } else {
            state.rotation = std::round(state.rotation / 90.0f) * 90.0f;
        }
    }
    
    static void simulateShip(PlayerState& state, bool clicking, float gravity, float dt) {
        if (clicking) {
            state.yVel += (state.isUpsideDown ? -0.8f : 0.8f);
        } else {
            state.yVel -= (state.isUpsideDown ? -0.8f : 0.8f);
        }
        
        float maxVel = state.isMini ? 6.0f : 8.0f;
        state.yVel = std::clamp(state.yVel, -maxVel, maxVel);
        state.y += state.yVel;
        state.rotation = state.yVel * 2.0f;
        state.isOnGround = false;
    }
    
    static void simulateBall(PlayerState& state, bool clicking, float gravity, float dt) {
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -12.0f, 12.0f);
        state.y += state.yVel;
        
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        if ((!state.isUpsideDown && state.y <= groundY) ||
            (state.isUpsideDown && state.y >= groundY)) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
        } else {
            state.isOnGround = false;
        }
        
        if (clicking && state.canJump && state.isOnGround) {
            state.isUpsideDown = !state.isUpsideDown;
            state.yVel = state.isUpsideDown ? -6.0f : 6.0f;
            state.canJump = false;
        }
        
        if (!clicking) state.canJump = true;
        state.rotation += state.isUpsideDown ? -10.0f : 10.0f;
    }
    
    static void simulateUFO(PlayerState& state, bool clicking, float gravity, float dt) {
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -8.0f, 8.0f);
        state.y += state.yVel;
        
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        if ((!state.isUpsideDown && state.y <= groundY) ||
            (state.isUpsideDown && state.y >= groundY)) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
        } else {
            state.isOnGround = false;
        }
        
        if (clicking && state.canJump) {
            float boost = state.isMini ? 5.5f : UFO_BOOST;
            state.yVel = state.isUpsideDown ? -boost : boost;
            state.canJump = false;
        }
        
        if (!clicking) state.canJump = true;
    }
    
    static void simulateWave(PlayerState& state, bool clicking, float dt) {
        float waveSpeed = state.isMini ? 0.8f : 1.0f;
        float diagonalSpeed = getSpeedMultiplier(state.speed) * dt * waveSpeed;
        
        if (clicking) {
            state.y += state.isUpsideDown ? -diagonalSpeed : diagonalSpeed;
        } else {
            state.y += state.isUpsideDown ? diagonalSpeed : -diagonalSpeed;
        }
        
        state.rotation = clicking ? 
            (state.isUpsideDown ? -45.0f : 45.0f) : 
            (state.isUpsideDown ? 45.0f : -45.0f);
        
        state.isOnGround = false;
    }
    
    static void simulateRobot(PlayerState& state, bool clicking, float gravity, float dt) {
        if (state.isRobotJumping && clicking) {
            state.robotJumpTime += dt;
            if (state.robotJumpTime < 0.25f) {
                state.yVel += 0.5f * (state.isUpsideDown ? -1 : 1);
            }
        }
        
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
        state.y += state.yVel;
        
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        if ((!state.isUpsideDown && state.y <= groundY) ||
            (state.isUpsideDown && state.y >= groundY)) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
            state.isRobotJumping = false;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        if (clicking && state.canJump && state.isOnGround) {
            float jumpVel = state.isMini ? 7.5f : 10.0f;
            state.yVel = state.isUpsideDown ? -jumpVel : jumpVel;
            state.isRobotJumping = true;
            state.robotJumpTime = 0;
            state.canJump = false;
            state.isOnGround = false;
        }
        
        if (!clicking) {
            state.isRobotJumping = false;
            state.canJump = true;
        }
    }
    
    static void simulateSpider(PlayerState& state, bool clicking, float gravity, float dt) {
        state.yVel -= gravity;
        state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
        state.y += state.yVel;
        
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        if ((!state.isUpsideDown && state.y <= groundY) ||
            (state.isUpsideDown && state.y >= groundY)) {
            state.y = groundY;
            state.yVel = 0;
            state.isOnGround = true;
            state.hasSpiderTeleported = false;
            state.canJump = true;
        } else {
            state.isOnGround = false;
        }
        
        if (clicking && state.canJump && state.isOnGround && !state.hasSpiderTeleported) {
            state.isUpsideDown = !state.isUpsideDown;
            state.y = state.isUpsideDown ? CEILING_Y : GROUND_Y;
            state.yVel = 0;
            state.hasSpiderTeleported = true;
            state.canJump = false;
        }
        
        if (!clicking) state.canJump = true;
    }
    
    static void simulateSwing(PlayerState& state, bool clicking, float gravity, float dt) {
        float swingGravity = clicking ? -gravity : gravity;
        
        state.yVel += swingGravity * 0.8f;
        state.yVel = std::clamp(state.yVel, -8.0f, 8.0f);
        state.y += state.yVel;
        state.rotation = state.yVel * 3.0f;
        state.isOnGround = false;
    }
    
    static void handleBoundaries(PlayerState& state) {
        if (state.y < GROUND_Y) {
            state.y = GROUND_Y;
            state.yVel = 0;
            if (!state.isUpsideDown) state.isOnGround = true;
        }
        if (state.y > CEILING_Y) {
            state.y = CEILING_Y;
            state.yVel = 0;
            if (state.isUpsideDown) state.isOnGround = true;
        }
    }
};

// ============================================================================
// LEVEL ANALYZER
// ============================================================================

class LevelAnalyzer {
private:
    std::vector<LevelObject> m_allObjects;
    std::map<int, std::vector<LevelObject*>> m_spatialGrid;
    static constexpr float GRID_SIZE = 150.0f;
    
    // Object ID sets
    std::set<int> m_hazardIDs = {
        8, 39, 103, 392, 9, 61, 243, 244, 245, 246, 247, 248, 249,
        363, 364, 365, 366, 367, 368, 446, 447, 678, 679, 680,
        1705, 1706, 1707, 1708, 1709, 1710, 1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718
    };
    
    std::map<int, int> m_orbTypes = {
        {36, 0}, {84, 1}, {141, 2}, {1022, 3}, {1330, 4}, {1333, 5}, {1704, 6}, {1751, 7}
    };
    
    std::map<int, int> m_padTypes = {
        {35, 0}, {67, 1}, {140, 2}, {1332, 3}, {452, 4}
    };
    
    std::map<int, GameMode> m_gamemodePortals = {
        {12, GameMode::Cube}, {13, GameMode::Ship}, {47, GameMode::Ball},
        {111, GameMode::UFO}, {660, GameMode::Wave}, {745, GameMode::Robot},
        {1331, GameMode::Spider}, {1933, GameMode::Swing}
    };
    
    std::map<int, SpeedType> m_speedPortals = {
        {200, SpeedType::Slow}, {201, SpeedType::Normal}, {202, SpeedType::Fast},
        {203, SpeedType::Faster}, {1334, SpeedType::Fastest}
    };
    
public:
    static LevelAnalyzer* get() {
        static LevelAnalyzer instance;
        return &instance;
    }
    
    void analyze(PlayLayer* pl) {
        clear();
        
        auto* objects = pl->m_objects;
        if (!objects) return;
        
        for (int i = 0; i < objects->count(); i++) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!obj) continue;
            
            LevelObject lo;
            lo.id = obj->m_objectID;
            lo.x = obj->getPositionX();
            lo.y = obj->getPositionY();
            lo.rotation = obj->getRotation();
            
            auto contentSize = obj->getContentSize();
            float scale = obj->getScale();
            lo.width = contentSize.width * scale * 0.85f;
            lo.height = contentSize.height * scale * 0.85f;
            
            categorizeObject(lo);
            
            if (lo.isHazard || lo.isSolid || lo.isOrb || lo.isPad || lo.isPortal) {
                m_allObjects.push_back(lo);
                
                int gridX = static_cast<int>(lo.x / GRID_SIZE);
                m_spatialGrid[gridX].push_back(&m_allObjects.back());
            }
        }
        
        log::info("AutoPlayer: Analyzed level - {} objects tracked", m_allObjects.size());
    }
    
    void clear() {
        m_allObjects.clear();
        m_spatialGrid.clear();
    }
    
    void categorizeObject(LevelObject& obj) {
        int id = obj.id;
        
        if (m_hazardIDs.count(id)) {
            obj.isHazard = true;
        }
        else if (m_orbTypes.count(id)) {
            obj.isOrb = true;
            obj.orbType = m_orbTypes[id];
        }
        else if (m_padTypes.count(id)) {
            obj.isPad = true;
            obj.orbType = m_padTypes[id];
        }
        else if (m_gamemodePortals.count(id)) {
            obj.isPortal = true;
            obj.portalGameMode = m_gamemodePortals[id];
        }
        else if (m_speedPortals.count(id)) {
            obj.isPortal = true;
            obj.isSpeedPortal = true;
            obj.portalSpeed = m_speedPortals[id];
        }
        else if (id == 10) { // Gravity down
            obj.isPortal = true;
            obj.isGravityPortal = true;
            obj.isGravityUp = false;
        }
        else if (id == 11) { // Gravity up
            obj.isPortal = true;
            obj.isGravityPortal = true;
            obj.isGravityUp = true;
        }
        else if (id == 99) { // Normal size
            obj.isPortal = true;
            obj.isMiniPortal = true;
            obj.isMiniOn = false;
        }
        else if (id == 101) { // Mini
            obj.isPortal = true;
            obj.isMiniPortal = true;
            obj.isMiniOn = true;
        }
    }
    
    std::vector<LevelObject*> getObjectsInRange(float startX, float endX) {
        std::vector<LevelObject*> result;
        
        int startGrid = static_cast<int>(startX / GRID_SIZE) - 1;
        int endGrid = static_cast<int>(endX / GRID_SIZE) + 1;
        
        for (int i = startGrid; i <= endGrid; i++) {
            if (m_spatialGrid.count(i)) {
                for (auto* obj : m_spatialGrid[i]) {
                    if (obj->x >= startX - 50 && obj->x <= endX + 50) {
                        result.push_back(obj);
                    }
                }
            }
        }
        
        return result;
    }
    
    const std::vector<LevelObject>& getAllObjects() const { return m_allObjects; }
};

// ============================================================================
// COLLISION CHECKER
// ============================================================================

class CollisionChecker {
public:
    static constexpr float PLAYER_WIDTH = 30.0f;
    static constexpr float PLAYER_HEIGHT = 30.0f;
    static constexpr float MINI_SCALE = 0.6f;
    
    static bool checkCollision(const PlayerState& state, const LevelObject& obj) {
        float playerW = state.isMini ? PLAYER_WIDTH * MINI_SCALE : PLAYER_WIDTH;
        float playerH = state.isMini ? PLAYER_HEIGHT * MINI_SCALE : PLAYER_HEIGHT;
        
        if (state.gameMode == GameMode::Ship) {
            playerH *= 0.8f;
        } else if (state.gameMode == GameMode::Wave) {
            playerW *= 0.6f;
            playerH *= 0.6f;
        }
        
        float playerLeft = state.x - playerW / 2;
        float playerRight = state.x + playerW / 2;
        float playerBottom = state.y - playerH / 2;
        float playerTop = state.y + playerH / 2;
        
        float objLeft = obj.x - obj.width / 2;
        float objRight = obj.x + obj.width / 2;
        float objBottom = obj.y - obj.height / 2;
        float objTop = obj.y + obj.height / 2;
        
        if (obj.isHazard && std::abs(obj.rotation) > 1) {
            float radius = std::min(obj.width, obj.height) / 2 * 0.7f;
            float dx = state.x - obj.x;
            float dy = state.y - obj.y;
            float playerRadius = std::min(playerW, playerH) / 2;
            return (dx * dx + dy * dy) < (radius + playerRadius) * (radius + playerRadius);
        }
        
        return !(playerRight < objLeft || playerLeft > objRight ||
                 playerTop < objBottom || playerBottom > objTop);
    }
    
    static bool checkDeath(const PlayerState& state, LevelAnalyzer* analyzer) {
        auto objects = analyzer->getObjectsInRange(state.x - 50, state.x + 50);
        
        for (auto* obj : objects) {
            if (obj->isHazard && checkCollision(state, *obj)) {
                return true;
            }
        }
        
        if (state.y < 50 || state.y > 2100) {
            return true;
        }
        
        return false;
    }
    
    static LevelObject* checkOrbCollision(const PlayerState& state, LevelAnalyzer* analyzer) {
        auto objects = analyzer->getObjectsInRange(state.x - 40, state.x + 40);
        
        for (auto* obj : objects) {
            if (obj->isOrb && checkCollision(state, *obj)) {
                return obj;
            }
        }
        return nullptr;
    }
    
    static LevelObject* checkPadCollision(const PlayerState& state, LevelAnalyzer* analyzer) {
        auto objects = analyzer->getObjectsInRange(state.x - 40, state.x + 40);
        
        for (auto* obj : objects) {
            if (obj->isPad && checkCollision(state, *obj)) {
                return obj;
            }
        }
        return nullptr;
    }
    
    static LevelObject* checkPortalCollision(const PlayerState& state, LevelAnalyzer* analyzer) {
        auto objects = analyzer->getObjectsInRange(state.x - 40, state.x + 40);
        
        for (auto* obj : objects) {
            if (obj->isPortal && checkCollision(state, *obj)) {
                return obj;
            }
        }
        return nullptr;
    }
};

// ============================================================================
// SEARCH NODE FOR PATHFINDING
// ============================================================================

struct SearchNode {
    PlayerState state;
    std::vector<bool> inputs;
    float score = 0;
    
    bool operator<(const SearchNode& other) const {
        return score < other.score;
    }
};

// ============================================================================
// PATHFINDER
// ============================================================================

class Pathfinder {
private:
    int m_lookaheadFrames = 180;
    int m_searchDepth = 30;
    int m_maxIterations = 5000;
    int m_timeLimitMs = 8;
    
    std::vector<bool> m_bestPath;
    bool m_pathFound = false;
    size_t m_usedPathIndex = 0;
    
public:
    static Pathfinder* get() {
        static Pathfinder instance;
        return &instance;
    }
    
    void setLookahead(int frames) { m_lookaheadFrames = frames; }
    void setSearchDepth(int depth) { m_searchDepth = depth; }
    void setMaxIterations(int iters) { m_maxIterations = iters; }
    void setTimeLimit(int ms) { m_timeLimitMs = ms; }
    
    bool getNextInput(const PlayerState& currentState, LevelAnalyzer* analyzer) {
        if (m_pathFound && m_usedPathIndex < m_bestPath.size()) {
            bool input = m_bestPath[m_usedPathIndex++];
            
            if (m_usedPathIndex % 8 == 0) {
                findPath(currentState, analyzer);
            }
            
            return input;
        }
        
        findPath(currentState, analyzer);
        
        if (m_pathFound && !m_bestPath.empty()) {
            m_usedPathIndex = 1;
            return m_bestPath[0];
        }
        
        return false;
    }
    
    void findPath(const PlayerState& startState, LevelAnalyzer* analyzer) {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        m_bestPath.clear();
        m_pathFound = false;
        m_usedPathIndex = 0;
        
        std::priority_queue<SearchNode> openSet;
        
        SearchNode start;
        start.state = startState;
        start.score = 0;
        openSet.push(start);
        
        float bestX = startState.x;
        std::vector<bool> bestInputs;
        
        int iterations = 0;
        
        while (!openSet.empty() && iterations < m_maxIterations) {
            iterations++;
            
            SearchNode current = openSet.top();
            openSet.pop();
            
            if (current.state.x > bestX) {
                bestX = current.state.x;
                bestInputs = current.inputs;
            }
            
            if (current.inputs.size() >= static_cast<size_t>(m_searchDepth)) {
                if (current.state.x > startState.x + 100) {
                    m_bestPath = current.inputs;
                    m_pathFound = true;
                    break;
                }
                continue;
            }
            
            for (bool clicking : {false, true}) {
                SearchNode next;
                next.state = current.state;
                next.inputs = current.inputs;
                next.inputs.push_back(clicking);
                
                bool died = false;
                for (int f = 0; f < 4 && !died; f++) {
                    PhysicsEngine::simulateFrame(next.state, clicking);
                    
                    handlePortals(next.state, analyzer);
                    handlePads(next.state, analyzer);
                    
                    if (clicking) {
                        handleOrbs(next.state, analyzer);
                    }
                    
                    if (CollisionChecker::checkDeath(next.state, analyzer)) {
                        died = true;
                    }
                }
                
                if (!died) {
                    next.score = calculateScore(next.state, startState, analyzer);
                    openSet.push(next);
                }
            }
            
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
            if (elapsed.count() > m_timeLimitMs) break;
        }
        
        if (!m_pathFound && !bestInputs.empty()) {
            m_bestPath = bestInputs;
            m_pathFound = true;
        }
        
        if (!m_pathFound) {
            m_bestPath = {false};
            m_pathFound = true;
        }
    }
    
    float calculateScore(const PlayerState& state, const PlayerState& start, LevelAnalyzer* analyzer) {
        float score = 0;
        
        score += (state.x - start.x) * 10;
        
        float safeY = 200;
        score -= std::abs(state.y - safeY) * 0.05f;
        
        auto nearbyObjects = analyzer->getObjectsInRange(state.x, state.x + 200);
        for (auto* obj : nearbyObjects) {
            if (obj->isHazard) {
                float dx = obj->x - state.x;
                float dy = obj->y - state.y;
                float dist = std::sqrt(dx*dx + dy*dy);
                if (dist < 100) {
                    score -= (100 - dist) * 0.3f;
                }
            }
        }
        
        return score;
    }
    
    void handlePortals(PlayerState& state, LevelAnalyzer* analyzer) {
        auto* portal = CollisionChecker::checkPortalCollision(state, analyzer);
        if (!portal) return;
        
        if (portal->isGravityPortal) {
            state.isUpsideDown = portal->isGravityUp;
        }
        else if (portal->isMiniPortal) {
            state.isMini = portal->isMiniOn;
        }
        else if (portal->isSpeedPortal) {
            state.speed = portal->portalSpeed;
        }
        else if (portal->isPortal) {
            state.gameMode = portal->portalGameMode;
            state.yVel *= 0.5f;
        }
    }
    
    void handlePads(PlayerState& state, LevelAnalyzer* analyzer) {
        auto* pad = CollisionChecker::checkPadCollision(state, analyzer);
        if (!pad) return;
        
        float boost = 0;
        switch (pad->orbType) {
            case 0: boost = 12.0f; break;
            case 1: boost = 16.0f; break;
            case 2: boost = 20.0f; break;
            case 3: boost = -12.0f; break;
            case 4:
                state.isUpsideDown = !state.isUpsideDown;
                boost = 0;
                break;
        }
        
        if (state.isUpsideDown && pad->orbType != 4) boost = -boost;
        state.yVel = boost;
        state.isOnGround = false;
    }
    
    void handleOrbs(PlayerState& state, LevelAnalyzer* analyzer) {
        if (state.orbCooldown > 0) return;
        
        auto* orb = CollisionChecker::checkOrbCollision(state, analyzer);
        if (!orb || orb->id == state.lastOrbID) return;
        
        float boost = 0;
        switch (orb->orbType) {
            case 0: boost = 11.2f; break;
            case 1: boost = 14.0f; break;
            case 2: boost = 18.0f; break;
            case 3:
                state.isUpsideDown = !state.isUpsideDown;
                boost = 8.0f;
                break;
            case 4:
                boost = 11.2f;
                state.isUpsideDown = !state.isUpsideDown;
                break;
            case 5: return;
            case 6:
            case 7:
                state.isDashing = true;
                boost = 15.0f;
                break;
        }
        
        if (state.isUpsideDown && orb->orbType != 3 && orb->orbType != 4) {
            boost = -boost;
        }
        
        state.yVel = boost;
        state.orbCooldown = 0.15f;
        state.lastOrbID = orb->id;
        state.isOnGround = false;
    }
    
    void reset() {
        m_bestPath.clear();
        m_pathFound = false;
        m_usedPathIndex = 0;
    }
    
    const std::vector<bool>& getCurrentPath() const { return m_bestPath; }
    bool hasPath() const { return m_pathFound; }
};

// ============================================================================
// DEBUG VISUALIZER
// ============================================================================

class DebugVisualizer : public CCNode {
private:
    CCDrawNode* m_drawNode = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    PlayerState m_lastState;
    bool m_enabled = false;
    
public:
    static DebugVisualizer* create() {
        auto ret = new DebugVisualizer();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
    
    bool init() {
        if (!CCNode::init()) return false;
        
        m_drawNode = CCDrawNode::create();
        m_drawNode->setZOrder(1000);
        this->addChild(m_drawNode);
        
        m_statusLabel = CCLabelBMFont::create("AutoPlayer: OFF", "bigFont.fnt");
        m_statusLabel->setScale(0.3f);
        m_statusLabel->setAnchorPoint({0, 1});
        m_statusLabel->setPosition({10, 310});
        m_statusLabel->setZOrder(1001);
        this->addChild(m_statusLabel);
        
        this->scheduleUpdate();
        
        return true;
    }
    
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    
    void updateState(const PlayerState& state) {
        m_lastState = state;
    }
    
    void update(float dt) override {
        m_drawNode->clear();
        
        auto* pl = PlayLayer::get();
        if (!pl) return;
        
        bool autoEnabled = Mod::get()->getSettingValue<bool>("enabled");
        bool debugEnabled = Mod::get()->getSettingValue<bool>("show-debug");
        
        // Update status label
        std::string status = autoEnabled ? "AutoPlayer: ON" : "AutoPlayer: OFF";
        m_statusLabel->setString(status.c_str());
        m_statusLabel->setColor(autoEnabled ? ccc3(0, 255, 0) : ccc3(255, 100, 100));
        
        if (!debugEnabled) return;
        
        auto* analyzer = LevelAnalyzer::get();
        
        // Draw predicted path (no click trajectory)
        PlayerState simState = m_lastState;
        std::vector<CCPoint> noClickPath;
        noClickPath.push_back(ccp(simState.x, simState.y));
        
        for (int i = 0; i < 90; i++) {
            PhysicsEngine::simulateFrame(simState, false);
            noClickPath.push_back(ccp(simState.x, simState.y));
        }
        
        for (size_t i = 1; i < noClickPath.size(); i++) {
            float alpha = 1.0f - (float)i / noClickPath.size();
            m_drawNode->drawSegment(
                noClickPath[i-1], 
                noClickPath[i],
                1.5f,
                ccc4f(0.2f, 0.6f, 1.0f, alpha * 0.7f)
            );
        }
        
        // Draw predicted path (with click trajectory)
        simState = m_lastState;
        std::vector<CCPoint> clickPath;
        clickPath.push_back(ccp(simState.x, simState.y));
        
        for (int i = 0; i < 90; i++) {
            PhysicsEngine::simulateFrame(simState, true);
            clickPath.push_back(ccp(simState.x, simState.y));
        }
        
        for (size_t i = 1; i < clickPath.size(); i++) {
            float alpha = 1.0f - (float)i / clickPath.size();
            m_drawNode->drawSegment(
                clickPath[i-1], 
                clickPath[i],
                1.5f,
                ccc4f(0.2f, 1.0f, 0.2f, alpha * 0.7f)
            );
        }
        
        // Draw nearby hazards
        auto hazards = analyzer->getObjectsInRange(m_lastState.x - 50, m_lastState.x + 400);
        for (auto* obj : hazards) {
            if (obj->isHazard) {
                m_drawNode->drawDot(
                    ccp(obj->x, obj->y),
                    obj->width / 3,
                    ccc4f(1.0f, 0.2f, 0.2f, 0.6f)
                );
            }
            else if (obj->isOrb) {
                m_drawNode->drawDot(
                    ccp(obj->x, obj->y),
                    12.0f,
                    ccc4f(1.0f, 1.0f, 0.2f, 0.6f)
                );
            }
            else if (obj->isPad) {
                m_drawNode->drawDot(
                    ccp(obj->x, obj->y),
                    10.0f,
                    ccc4f(1.0f, 0.5f, 0.8f, 0.6f)
                );
            }
            else if (obj->isPortal) {
                m_drawNode->drawDot(
                    ccp(obj->x, obj->y),
                    15.0f,
                    ccc4f(0.2f, 0.8f, 1.0f, 0.6f)
                );
            }
        }
        
        // Draw player position
        m_drawNode->drawDot(
            ccp(m_lastState.x, m_lastState.y),
            8.0f,
            ccc4f(1.0f, 1.0f, 1.0f, 0.9f)
        );
        
        // Draw ground line
        m_drawNode->drawSegment(
            ccp(m_lastState.x - 100, PhysicsEngine::GROUND_Y),
            ccp(m_lastState.x + 400, PhysicsEngine::GROUND_Y),
            1.0f,
            ccc4f(0.5f, 0.5f, 0.5f, 0.3f)
        );
    }
};

// ============================================================================
// AUTO PLAYER CONTROLLER
// ============================================================================

class AutoPlayer {
private:
    bool m_enabled = false;
    PlayerState m_currentState;
    bool m_isClicking = false;
    DebugVisualizer* m_visualizer = nullptr;
    
public:
    static AutoPlayer* get() {
        static AutoPlayer instance;
        return &instance;
    }
    
    void enable() { 
        m_enabled = true;
        log::info("AutoPlayer: Enabled");
    }
    
    void disable() { 
        m_enabled = false;
        log::info("AutoPlayer: Disabled");
    }
    
    void toggle() {
        if (m_enabled) disable();
        else enable();
        
        Mod::get()->setSettingValue("enabled", m_enabled);
    }
    
    bool isEnabled() const { return m_enabled; }
    
    void setVisualizer(DebugVisualizer* viz) { m_visualizer = viz; }
    DebugVisualizer* getVisualizer() { return m_visualizer; }
    
    void syncSettings() {
        m_enabled = Mod::get()->getSettingValue<bool>("enabled");
        
        int lookahead = Mod::get()->getSettingValue<int64_t>("lookahead-frames");
        int depth = Mod::get()->getSettingValue<int64_t>("search-depth");
        int maxIter = Mod::get()->getSettingValue<int64_t>("max-iterations");
        int timeLimit = Mod::get()->getSettingValue<int64_t>("time-limit-ms");
        
        Pathfinder::get()->setLookahead(lookahead);
        Pathfinder::get()->setSearchDepth(depth);
        Pathfinder::get()->setMaxIterations(maxIter);
        Pathfinder::get()->setTimeLimit(timeLimit);
    }
    
    void updateState(PlayerObject* player) {
        if (!player) return;
        
        m_currentState.x = player->getPositionX();
        m_currentState.y = player->getPositionY();
        m_currentState.yVel = player->m_yVelocity;
        m_currentState.rotation = player->getRotation();
        
        m_currentState.isUpsideDown = player->m_isUpsideDown;
        m_currentState.isMini = player->m_vehicleSize != 1.0f;
        m_currentState.isOnGround = player->m_isOnGround;
        
        if (player->m_isShip) m_currentState.gameMode = GameMode::Ship;
        else if (player->m_isBall) m_currentState.gameMode = GameMode::Ball;
        else if (player->m_isBird) m_currentState.gameMode = GameMode::UFO;
        else if (player->m_isDart) m_currentState.gameMode = GameMode::Wave;
        else if (player->m_isRobot) m_currentState.gameMode = GameMode::Robot;
        else if (player->m_isSpider) m_currentState.gameMode = GameMode::Spider;
        else if (player->m_isSwing) m_currentState.gameMode = GameMode::Swing;
        else m_currentState.gameMode = GameMode::Cube;
        
        float speed = player->m_playerSpeed;
        if (speed <= 0.8f) m_currentState.speed = SpeedType::Slow;
        else if (speed <= 0.95f) m_currentState.speed = SpeedType::Normal;
        else if (speed <= 1.05f) m_currentState.speed = SpeedType::Fast;
        else if (speed <= 1.15f) m_currentState.speed = SpeedType::Faster;
        else if (speed <= 1.4f) m_currentState.speed = SpeedType::Fastest;
        else m_currentState.speed = SpeedType::SuperFast;
        
        if (m_visualizer) {
            m_visualizer->updateState(m_currentState);
        }
    }
    
    void update(PlayLayer* pl) {
        if (!m_enabled || !pl || !pl->m_player1) return;
        if (pl->m_isPaused || pl->m_hasCompletedLevel) return;
        
        updateState(pl->m_player1);
        
        bool shouldClick = Pathfinder::get()->getNextInput(m_currentState, LevelAnalyzer::get());
        
        if (shouldClick && !m_isClicking) {
            pl->pushButton(1, true);
            m_isClicking = true;
        }
        else if (!shouldClick && m_isClicking) {
            pl->releaseButton(1, true);
            m_isClicking = false;
        }
    }
    
    void reset() {
        m_isClicking = false;
        Pathfinder::get()->reset();
    }
    
    PlayerState& getState() { return m_currentState; }
};

// ============================================================================
// PLAY LAYER HOOKS
// ============================================================================

class $modify(AutoPlayLayer, PlayLayer) {
    struct Fields {
        bool m_analyzed = false;
        DebugVisualizer* m_visualizer = nullptr;
    };
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        
        m_fields->m_analyzed = false;
        
        AutoPlayer::get()->syncSettings();
        
        m_fields->m_visualizer = DebugVisualizer::create();
        m_fields->m_visualizer->setZOrder(10000);
        this->addChild(m_fields->m_visualizer);
        AutoPlayer::get()->setVisualizer(m_fields->m_visualizer);
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        
        if (!m_fields->m_analyzed) {
            LevelAnalyzer::get()->analyze(this);
            m_fields->m_analyzed = true;
            log::info("AutoPlayer: Level analyzed successfully");
        }
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        AutoPlayer::get()->reset();
        
        if (!m_fields->m_analyzed) {
            LevelAnalyzer::get()->analyze(this);
            m_fields->m_analyzed = true;
        }
        
        AutoPlayer::get()->syncSettings();
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        AutoPlayer::get()->update(this);
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        
        log::info("AutoPlayer: Level completed!");
        
        Notification::create(
            "AutoPlayer completed the level!",
            NotificationIcon::Success
        )->show();
    }
    
    void onQuit() {
        AutoPlayer::get()->disable();
        AutoPlayer::get()->reset();
        AutoPlayer::get()->setVisualizer(nullptr);
        PlayLayer::onQuit();
    }
    
    void pauseGame(bool pause) {
        PlayLayer::pauseGame(pause);
        
        if (pause && AutoPlayer::get()->isEnabled()) {
            this->releaseButton(1, true);
        }
    }
};

// ============================================================================
// KEYBINDS AND INITIALIZATION
// ============================================================================

$on_mod(Loaded) {
    log::info("AutoPlayer mod loaded!");
    
    // Listen for setting changes
    listenForSettingChanges("enabled", [](bool value) {
        if (value) {
            AutoPlayer::get()->enable();
        } else {
            AutoPlayer::get()->disable();
        }
    });
    
    listenForSettingChanges("show-debug", [](bool value) {
        auto* viz = AutoPlayer::get()->getVisualizer();
        if (viz) {
            viz->setEnabled(value);
        }
    });
    
    listenForSettingChanges("lookahead-frames", [](int64_t value) {
        Pathfinder::get()->setLookahead(static_cast<int>(value));
    });
    
    listenForSettingChanges("search-depth", [](int64_t value) {
        Pathfinder::get()->setSearchDepth(static_cast<int>(value));
    });
    
    listenForSettingChanges("max-iterations", [](int64_t value) {
        Pathfinder::get()->setMaxIterations(static_cast<int>(value));
    });
    
    listenForSettingChanges("time-limit-ms", [](int64_t value) {
        Pathfinder::get()->setTimeLimit(static_cast<int>(value));
    });
}

// Keyboard shortcut to toggle
#include <Geode/modify/CCKeyboardDispatcher.hpp>

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            // F8 to toggle AutoPlayer
            if (key == KEY_F8) {
                AutoPlayer::get()->toggle();
                
                Notification::create(
                    AutoPlayer::get()->isEnabled() ? "AutoPlayer: ON" : "AutoPlayer: OFF",
                    AutoPlayer::get()->isEnabled() ? NotificationIcon::Success : NotificationIcon::Info
                )->show();
                
                return true;
            }
            
            // F9 to toggle debug visualization
            if (key == KEY_F9) {
                bool current = Mod::get()->getSettingValue<bool>("show-debug");
                Mod::get()->setSettingValue("show-debug", !current);
                
                Notification::create(
                    !current ? "Debug View: ON" : "Debug View: OFF",
                    NotificationIcon::Info
                )->show();
                
                return true;
            }
        }
        
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
