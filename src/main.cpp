#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <vector>
#include <queue>
#include <set>
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

// ============================================================================
// GLOBAL STATE - Simple globals for reliability
// ============================================================================

static bool g_autoPlayerEnabled = false;
static bool g_debugEnabled = false;
static bool g_isClicking = false;
static bool g_levelAnalyzed = false;

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
    Slow,
    Normal,
    Fast,
    Faster,
    Fastest,
    SuperFast
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
    bool isOnGround = true;
    bool canJump = true;
    
    float orbCooldown = 0;
    int lastOrbID = -1;
    
    bool isRobotJumping = false;
    float robotJumpTime = 0;
    bool hasSpiderTeleported = false;
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
    
    static void simulateFrame(PlayerState& state, bool clicking, float dt = 1.0f/240.0f) {
        float gravity = getGravity(state.gameMode, state.isMini);
        if (state.isUpsideDown) gravity = -gravity;
        
        float xSpeed = getSpeedMultiplier(state.speed) * dt;
        
        if (state.orbCooldown > 0) state.orbCooldown -= dt;
        
        switch (state.gameMode) {
            case GameMode::Cube:
                simulateCube(state, clicking, gravity);
                break;
            case GameMode::Ship:
                simulateShip(state, clicking);
                break;
            case GameMode::Ball:
                simulateBall(state, clicking, gravity);
                break;
            case GameMode::UFO:
                simulateUFO(state, clicking, gravity);
                break;
            case GameMode::Wave:
                simulateWave(state, clicking, xSpeed);
                break;
            case GameMode::Robot:
                simulateRobot(state, clicking, gravity, dt);
                break;
            case GameMode::Spider:
                simulateSpider(state, clicking, gravity);
                break;
            case GameMode::Swing:
                simulateSwing(state, clicking, gravity);
                break;
        }
        
        state.x += xSpeed;
        handleBoundaries(state);
    }
    
private:
    static void simulateCube(PlayerState& state, bool clicking, float gravity) {
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
            float jumpVel = state.isMini ? 9.4f : JUMP_VELOCITY;
            state.yVel = state.isUpsideDown ? -jumpVel : jumpVel;
            state.isOnGround = false;
            state.canJump = false;
        }
        
        if (!clicking) state.canJump = true;
    }
    
    static void simulateShip(PlayerState& state, bool clicking) {
        if (clicking) {
            state.yVel += (state.isUpsideDown ? -0.8f : 0.8f);
        } else {
            state.yVel -= (state.isUpsideDown ? -0.8f : 0.8f);
        }
        
        float maxVel = state.isMini ? 6.0f : 8.0f;
        state.yVel = std::clamp(state.yVel, -maxVel, maxVel);
        state.y += state.yVel;
        state.isOnGround = false;
    }
    
    static void simulateBall(PlayerState& state, bool clicking, float gravity) {
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
    }
    
    static void simulateUFO(PlayerState& state, bool clicking, float gravity) {
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
    
    static void simulateWave(PlayerState& state, bool clicking, float xSpeed) {
        float waveSpeed = state.isMini ? 0.8f : 1.0f;
        float diagonalSpeed = xSpeed * waveSpeed;
        
        if (clicking) {
            state.y += state.isUpsideDown ? -diagonalSpeed : diagonalSpeed;
        } else {
            state.y += state.isUpsideDown ? diagonalSpeed : -diagonalSpeed;
        }
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
    
    static void simulateSpider(PlayerState& state, bool clicking, float gravity) {
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
    
    static void simulateSwing(PlayerState& state, bool clicking, float gravity) {
        float swingGravity = clicking ? -gravity : gravity;
        
        state.yVel += swingGravity * 0.8f;
        state.yVel = std::clamp(state.yVel, -8.0f, 8.0f);
        state.y += state.yVel;
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
        
        if (!pl) {
            log::error("AutoPlayer: PlayLayer is null!");
            return;
        }
        
        auto* objects = pl->m_objects;
        if (!objects) {
            log::error("AutoPlayer: No objects in level!");
            return;
        }
        
        log::info("AutoPlayer: Analyzing {} objects...", objects->count());
        
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
            
            if (lo.isHazard || lo.isOrb || lo.isPad || lo.isPortal) {
                m_allObjects.push_back(lo);
                
                int gridX = static_cast<int>(lo.x / GRID_SIZE);
                m_spatialGrid[gridX].push_back(&m_allObjects.back());
            }
        }
        
        log::info("AutoPlayer: Tracked {} important objects", m_allObjects.size());
        g_levelAnalyzed = true;
    }
    
    void clear() {
        m_allObjects.clear();
        m_spatialGrid.clear();
        g_levelAnalyzed = false;
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
        else if (id == 10) {
            obj.isPortal = true;
            obj.isGravityPortal = true;
            obj.isGravityUp = false;
        }
        else if (id == 11) {
            obj.isPortal = true;
            obj.isGravityPortal = true;
            obj.isGravityUp = true;
        }
        else if (id == 99) {
            obj.isPortal = true;
            obj.isMiniPortal = true;
            obj.isMiniOn = false;
        }
        else if (id == 101) {
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
    
    bool isAnalyzed() const { return g_levelAnalyzed; }
};

// ============================================================================
// COLLISION CHECKER
// ============================================================================

class CollisionChecker {
public:
    static constexpr float PLAYER_SIZE = 30.0f;
    static constexpr float MINI_SCALE = 0.6f;
    
    static bool checkCollision(const PlayerState& state, const LevelObject& obj) {
        float playerSize = state.isMini ? PLAYER_SIZE * MINI_SCALE : PLAYER_SIZE;
        
        if (state.gameMode == GameMode::Wave) {
            playerSize *= 0.6f;
        }
        
        float halfPlayer = playerSize / 2;
        
        float playerLeft = state.x - halfPlayer;
        float playerRight = state.x + halfPlayer;
        float playerBottom = state.y - halfPlayer;
        float playerTop = state.y + halfPlayer;
        
        float objLeft = obj.x - obj.width / 2;
        float objRight = obj.x + obj.width / 2;
        float objBottom = obj.y - obj.height / 2;
        float objTop = obj.y + obj.height / 2;
        
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
// SEARCH NODE
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
                if (current.state.x > startState.x + 50) {
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
                    if (clicking) handleOrbs(next.state, analyzer);
                    if (CollisionChecker::checkDeath(next.state, analyzer)) died = true;
                }
                
                if (!died) {
                    next.score = (next.state.x - startState.x) * 10;
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
                return;
        }
        
        if (state.isUpsideDown) boost = -boost;
        state.yVel = boost;
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
                boost = 15.0f;
                break;
        }
        
        if (state.isUpsideDown && orb->orbType != 3 && orb->orbType != 4) {
            boost = -boost;
        }
        
        state.yVel = boost;
        state.orbCooldown = 0.15f;
        state.lastOrbID = orb->id;
    }
    
    void reset() {
        m_bestPath.clear();
        m_pathFound = false;
        m_usedPathIndex = 0;
    }
};

// ============================================================================
// CURRENT STATE TRACKER
// ============================================================================

static PlayerState g_currentState;

void updatePlayerState(PlayerObject* player) {
    if (!player) return;
    
    g_currentState.x = player->getPositionX();
    g_currentState.y = player->getPositionY();
    g_currentState.yVel = player->m_yVelocity;
    
    g_currentState.isUpsideDown = player->m_isUpsideDown;
    g_currentState.isMini = player->m_vehicleSize != 1.0f;
    g_currentState.isOnGround = player->m_isOnGround;
    
    if (player->m_isShip) g_currentState.gameMode = GameMode::Ship;
    else if (player->m_isBall) g_currentState.gameMode = GameMode::Ball;
    else if (player->m_isBird) g_currentState.gameMode = GameMode::UFO;
    else if (player->m_isDart) g_currentState.gameMode = GameMode::Wave;
    else if (player->m_isRobot) g_currentState.gameMode = GameMode::Robot;
    else if (player->m_isSpider) g_currentState.gameMode = GameMode::Spider;
    else if (player->m_isSwing) g_currentState.gameMode = GameMode::Swing;
    else g_currentState.gameMode = GameMode::Cube;
    
    float speed = player->m_playerSpeed;
    if (speed <= 0.8f) g_currentState.speed = SpeedType::Slow;
    else if (speed <= 0.95f) g_currentState.speed = SpeedType::Normal;
    else if (speed <= 1.05f) g_currentState.speed = SpeedType::Fast;
    else if (speed <= 1.15f) g_currentState.speed = SpeedType::Faster;
    else g_currentState.speed = SpeedType::Fastest;
}

// ============================================================================
// PLAY LAYER HOOKS
// ============================================================================

class $modify(AutoPlayLayer, PlayLayer) {
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        
        log::info("AutoPlayer: PlayLayer init");
        g_levelAnalyzed = false;
        g_isClicking = false;
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        
        log::info("AutoPlayer: Setup completed, analyzing level...");
        LevelAnalyzer::get()->analyze(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        log::info("AutoPlayer: Level reset");
        g_isClicking = false;
        Pathfinder::get()->reset();
        
        if (!g_levelAnalyzed) {
            LevelAnalyzer::get()->analyze(this);
        }
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        
        if (!g_autoPlayerEnabled) return;
        if (!m_player1) return;
        if (m_isPaused || m_hasCompletedLevel || m_player1->m_isDead) return;
        
        updatePlayerState(m_player1);
        
        bool shouldClick = Pathfinder::get()->getNextInput(g_currentState, LevelAnalyzer::get());
        
        if (shouldClick && !g_isClicking) {
            m_player1->pushButton(PlayerButton::Jump);
            g_isClicking = true;
        }
        else if (!shouldClick && g_isClicking) {
            m_player1->releaseButton(PlayerButton::Jump);
            g_isClicking = false;
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        
        log::info("AutoPlayer: Level completed!");
        Notification::create("AutoPlayer: Level Complete!", NotificationIcon::Success)->show();
    }
    
    void onQuit() {
        g_isClicking = false;
        g_levelAnalyzed = false;
        Pathfinder::get()->reset();
        PlayLayer::onQuit();
    }
};

// ============================================================================
// KEYBOARD HANDLER
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        
        if (down && !repeat && key == KEY_F8) {
            g_autoPlayerEnabled = !g_autoPlayerEnabled;
            
            log::info("AutoPlayer: Toggled to {}", g_autoPlayerEnabled ? "ON" : "OFF");
            
            if (!g_autoPlayerEnabled) {
                // Release button when disabling
                auto* pl = PlayLayer::get();
                if (pl && pl->m_player1 && g_isClicking) {
                    pl->m_player1->releaseButton(PlayerButton::Jump);
                    g_isClicking = false;
                }
            }
            
            Notification::create(
                g_autoPlayerEnabled ? "AutoPlayer: ON" : "AutoPlayer: OFF",
                g_autoPlayerEnabled ? NotificationIcon::Success : NotificationIcon::Info
            )->show();
            
            return true;
        }
        
        if (down && !repeat && key == KEY_F9) {
            g_debugEnabled = !g_debugEnabled;
            
            log::info("AutoPlayer: Debug toggled to {}", g_debugEnabled ? "ON" : "OFF");
            
            Notification::create(
                g_debugEnabled ? "Debug: ON" : "Debug: OFF",
                NotificationIcon::Info
            )->show();
            
            return true;
        }
        
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// ============================================================================
// MOD LOAD
// ============================================================================

$on_mod(Loaded) {
    log::info("========================================");
    log::info("AutoPlayer Mod Loaded!");
    log::info("Press F8 to toggle AutoPlayer");
    log::info("Press F9 to toggle Debug View");
    log::info("========================================");
}
