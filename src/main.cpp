#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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
// GLOBAL STATE
// ============================================================================

static bool g_autoPlayerEnabled = false;
static bool g_debugEnabled = false;
static bool g_isClicking = false;
static bool g_levelAnalyzed = false;
static int g_frameCounter = 0;

// ============================================================================
// ENUMS AND CONSTANTS
// ============================================================================

enum class GameMode {
    Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing
};

enum class SpeedType {
    Slow, Normal, Fast, Faster, Fastest, SuperFast
};

// ============================================================================
// PLAYER STATE
// ============================================================================

struct PlayerState {
    float x = 0;
    float y = 105.0f;
    float yVel = 0;
    
    GameMode gameMode = GameMode::Cube;
    SpeedType speed = SpeedType::Normal;
    
    bool isUpsideDown = false;
    bool isMini = false;
    bool isOnGround = true;
    bool canJump = true;
    
    float orbCooldown = 0;
    int lastOrbID = -1;
};

// ============================================================================
// LEVEL OBJECT
// ============================================================================

struct LevelObject {
    int id;
    float x, y;
    float width, height;
    
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
// PHYSICS ENGINE - SIMPLIFIED AND ACCURATE
// ============================================================================

class PhysicsEngine {
public:
    static constexpr float GROUND_Y = 105.0f;
    static constexpr float CEILING_Y = 2085.0f;
    
    static float getSpeed(SpeedType speed) {
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
    
    static void simulate(PlayerState& state, bool holding, float dt = 1.0f/240.0f) {
        float xSpeed = getSpeed(state.speed) * dt;
        
        if (state.orbCooldown > 0) state.orbCooldown -= dt;
        
        float gravity = state.isMini ? 0.8f : 0.958199f;
        if (state.isUpsideDown) gravity = -gravity;
        
        float groundY = state.isUpsideDown ? CEILING_Y : GROUND_Y;
        
        switch (state.gameMode) {
            case GameMode::Cube: {
                // Apply gravity
                state.yVel -= gravity;
                state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
                state.y += state.yVel;
                
                // Ground check
                if ((!state.isUpsideDown && state.y <= groundY) ||
                    (state.isUpsideDown && state.y >= groundY)) {
                    state.y = groundY;
                    state.yVel = 0;
                    state.isOnGround = true;
                } else {
                    state.isOnGround = false;
                }
                
                // Jump when holding and on ground
                if (holding && state.isOnGround && state.canJump) {
                    float jumpVel = state.isMini ? 9.4f : 11.18f;
                    state.yVel = state.isUpsideDown ? -jumpVel : jumpVel;
                    state.isOnGround = false;
                    state.canJump = false;
                }
                
                // Reset jump ability when released
                if (!holding) state.canJump = true;
                break;
            }
            
            case GameMode::Ship: {
                float accel = state.isMini ? 0.6f : 0.8f;
                float maxVel = state.isMini ? 6.0f : 8.0f;
                
                if (holding) {
                    state.yVel += state.isUpsideDown ? -accel : accel;
                } else {
                    state.yVel += state.isUpsideDown ? accel : -accel;
                }
                
                state.yVel = std::clamp(state.yVel, -maxVel, maxVel);
                state.y += state.yVel;
                state.isOnGround = false;
                break;
            }
            
            case GameMode::Ball: {
                state.yVel -= gravity * 0.6f;
                state.yVel = std::clamp(state.yVel, -12.0f, 12.0f);
                state.y += state.yVel;
                
                if ((!state.isUpsideDown && state.y <= groundY) ||
                    (state.isUpsideDown && state.y >= groundY)) {
                    state.y = groundY;
                    state.yVel = 0;
                    state.isOnGround = true;
                } else {
                    state.isOnGround = false;
                }
                
                if (holding && state.isOnGround && state.canJump) {
                    state.isUpsideDown = !state.isUpsideDown;
                    state.yVel = state.isUpsideDown ? -6.0f : 6.0f;
                    state.canJump = false;
                }
                
                if (!holding) state.canJump = true;
                break;
            }
            
            case GameMode::UFO: {
                state.yVel -= gravity * 0.5f;
                state.yVel = std::clamp(state.yVel, -8.0f, 8.0f);
                state.y += state.yVel;
                
                if ((!state.isUpsideDown && state.y <= groundY) ||
                    (state.isUpsideDown && state.y >= groundY)) {
                    state.y = groundY;
                    state.yVel = 0;
                    state.isOnGround = true;
                } else {
                    state.isOnGround = false;
                }
                
                if (holding && state.canJump) {
                    float boost = state.isMini ? 5.5f : 7.0f;
                    state.yVel = state.isUpsideDown ? -boost : boost;
                    state.canJump = false;
                }
                
                if (!holding) state.canJump = true;
                break;
            }
            
            case GameMode::Wave: {
                float waveSpeed = xSpeed * (state.isMini ? 0.7f : 1.0f);
                if (holding) {
                    state.y += state.isUpsideDown ? -waveSpeed : waveSpeed;
                } else {
                    state.y += state.isUpsideDown ? waveSpeed : -waveSpeed;
                }
                state.isOnGround = false;
                break;
            }
            
            default: {
                // Default cube-like behavior for robot/spider/swing
                state.yVel -= gravity;
                state.yVel = std::clamp(state.yVel, -15.0f, 15.0f);
                state.y += state.yVel;
                
                if (state.y <= GROUND_Y) {
                    state.y = GROUND_Y;
                    state.yVel = 0;
                    state.isOnGround = true;
                } else {
                    state.isOnGround = false;
                }
                
                if (holding && state.isOnGround && state.canJump) {
                    state.yVel = 11.18f;
                    state.canJump = false;
                }
                if (!holding) state.canJump = true;
                break;
            }
        }
        
        state.x += xSpeed;
        
        // Clamp to bounds
        state.y = std::clamp(state.y, GROUND_Y - 10.0f, CEILING_Y + 10.0f);
    }
};

// ============================================================================
// LEVEL ANALYZER
// ============================================================================

class LevelAnalyzer {
private:
    std::vector<LevelObject> m_objects;
    
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

public:
    static LevelAnalyzer* get() {
        static LevelAnalyzer instance;
        return &instance;
    }
    
    void analyze(PlayLayer* pl) {
        m_objects.clear();
        g_levelAnalyzed = false;
        
        if (!pl || !pl->m_objects) return;
        
        int hazardCount = 0;
        
        for (int i = 0; i < pl->m_objects->count(); i++) {
            auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!obj) continue;
            
            LevelObject lo;
            lo.id = obj->m_objectID;
            lo.x = obj->getPositionX();
            lo.y = obj->getPositionY();
            
            auto cs = obj->getContentSize();
            float scale = obj->getScale();
            lo.width = cs.width * scale * 0.8f;
            lo.height = cs.height * scale * 0.8f;
            
            // Categorize
            if (m_hazardIDs.count(lo.id)) {
                lo.isHazard = true;
                hazardCount++;
            }
            else if (m_orbTypes.count(lo.id)) {
                lo.isOrb = true;
                lo.orbType = m_orbTypes[lo.id];
            }
            else if (m_padTypes.count(lo.id)) {
                lo.isPad = true;
                lo.orbType = m_padTypes[lo.id];
            }
            else if (lo.id == 10) { lo.isPortal = true; lo.isGravityPortal = true; lo.isGravityUp = false; }
            else if (lo.id == 11) { lo.isPortal = true; lo.isGravityPortal = true; lo.isGravityUp = true; }
            else if (lo.id == 12) { lo.isPortal = true; lo.portalGameMode = GameMode::Cube; }
            else if (lo.id == 13) { lo.isPortal = true; lo.portalGameMode = GameMode::Ship; }
            else if (lo.id == 47) { lo.isPortal = true; lo.portalGameMode = GameMode::Ball; }
            else if (lo.id == 111) { lo.isPortal = true; lo.portalGameMode = GameMode::UFO; }
            else if (lo.id == 660) { lo.isPortal = true; lo.portalGameMode = GameMode::Wave; }
            else if (lo.id == 745) { lo.isPortal = true; lo.portalGameMode = GameMode::Robot; }
            else if (lo.id == 1331) { lo.isPortal = true; lo.portalGameMode = GameMode::Spider; }
            else if (lo.id == 99) { lo.isPortal = true; lo.isMiniPortal = true; lo.isMiniOn = false; }
            else if (lo.id == 101) { lo.isPortal = true; lo.isMiniPortal = true; lo.isMiniOn = true; }
            else if (lo.id == 200) { lo.isPortal = true; lo.isSpeedPortal = true; lo.portalSpeed = SpeedType::Slow; }
            else if (lo.id == 201) { lo.isPortal = true; lo.isSpeedPortal = true; lo.portalSpeed = SpeedType::Normal; }
            else if (lo.id == 202) { lo.isPortal = true; lo.isSpeedPortal = true; lo.portalSpeed = SpeedType::Fast; }
            else if (lo.id == 203) { lo.isPortal = true; lo.isSpeedPortal = true; lo.portalSpeed = SpeedType::Faster; }
            else if (lo.id == 1334) { lo.isPortal = true; lo.isSpeedPortal = true; lo.portalSpeed = SpeedType::Fastest; }
            else continue; // Skip non-important objects
            
            m_objects.push_back(lo);
        }
        
        // Sort by X position for faster lookup
        std::sort(m_objects.begin(), m_objects.end(), [](const LevelObject& a, const LevelObject& b) {
            return a.x < b.x;
        });
        
        log::info("AutoPlayer: {} hazards, {} total tracked", hazardCount, m_objects.size());
        g_levelAnalyzed = true;
    }
    
    // Get objects near a position
    std::vector<LevelObject*> getNearby(float x, float range = 100.0f) {
        std::vector<LevelObject*> result;
        for (auto& obj : m_objects) {
            if (obj.x >= x - range && obj.x <= x + range) {
                result.push_back(&obj);
            }
            if (obj.x > x + range) break; // Objects are sorted by X
        }
        return result;
    }
    
    // Check if there's a hazard ahead that we need to jump over
    LevelObject* getNextHazard(float x) {
        for (auto& obj : m_objects) {
            if (obj.isHazard && obj.x > x && obj.x < x + 300) {
                return &obj;
            }
        }
        return nullptr;
    }
};

// ============================================================================
// COLLISION CHECKER
// ============================================================================

class CollisionChecker {
public:
    static bool collides(float px, float py, float psize, const LevelObject& obj) {
        float half = psize / 2;
        return !(px + half < obj.x - obj.width/2 || 
                 px - half > obj.x + obj.width/2 ||
                 py + half < obj.y - obj.height/2 || 
                 py - half > obj.y + obj.height/2);
    }
    
    static bool willDie(const PlayerState& state, LevelAnalyzer* analyzer) {
        float psize = state.isMini ? 18.0f : 30.0f;
        if (state.gameMode == GameMode::Wave) psize *= 0.6f;
        
        auto nearby = analyzer->getNearby(state.x, 50);
        for (auto* obj : nearby) {
            if (obj->isHazard && collides(state.x, state.y, psize, *obj)) {
                return true;
            }
        }
        
        // Check bounds
        if (state.y < 80 || state.y > 2100) return true;
        
        return false;
    }
};

// ============================================================================
// SIMPLE REACTIVE BOT - Direct hazard avoidance
// ============================================================================

class ReactiveBot {
private:
    bool m_wasHolding = false;
    float m_holdStartX = 0;
    
public:
    static ReactiveBot* get() {
        static ReactiveBot instance;
        return &instance;
    }
    
    bool shouldClick(const PlayerState& state, LevelAnalyzer* analyzer) {
        // Simple approach: simulate both clicking and not clicking
        // Choose the one that survives longer
        
        PlayerState simNoClick = state;
        PlayerState simClick = state;
        
        int surviveNoClick = 0;
        int surviveClick = 0;
        
        const int lookAhead = 60; // frames to simulate
        
        // Simulate NOT clicking
        for (int i = 0; i < lookAhead; i++) {
            PhysicsEngine::simulate(simNoClick, false);
            handlePortalsAndPads(simNoClick, analyzer);
            if (CollisionChecker::willDie(simNoClick, analyzer)) break;
            surviveNoClick++;
        }
        
        // Simulate clicking
        for (int i = 0; i < lookAhead; i++) {
            // Only hold for a bit, then release
            bool hold = (i < 15);
            PhysicsEngine::simulate(simClick, hold);
            handlePortalsAndPads(simClick, analyzer);
            if (hold) handleOrbs(simClick, analyzer);
            if (CollisionChecker::willDie(simClick, analyzer)) break;
            surviveClick++;
        }
        
        if (g_debugEnabled && g_frameCounter % 30 == 0) {
            log::info("Bot: x={:.0f} y={:.0f} noClick={} click={} onGround={}", 
                state.x, state.y, surviveNoClick, surviveClick, state.isOnGround);
        }
        
        // If clicking survives longer, click
        // Also click if we're about to die either way (try to escape)
        if (surviveClick > surviveNoClick) {
            return true;
        }
        
        // If both survive equally, check if there's a hazard we need to jump over
        if (surviveClick == surviveNoClick && surviveClick == lookAhead) {
            auto* hazard = analyzer->getNextHazard(state.x);
            if (hazard && state.isOnGround) {
                float dist = hazard->x - state.x;
                // Jump if hazard is close and at ground level
                if (dist < 150 && dist > 50 && hazard->y < 200) {
                    return true;
                }
            }
        }
        
        return false;
    }
    
    void handlePortalsAndPads(PlayerState& state, LevelAnalyzer* analyzer) {
        auto nearby = analyzer->getNearby(state.x, 30);
        
        for (auto* obj : nearby) {
            float psize = state.isMini ? 18.0f : 30.0f;
            if (!CollisionChecker::collides(state.x, state.y, psize, *obj)) continue;
            
            if (obj->isPortal) {
                if (obj->isGravityPortal) state.isUpsideDown = obj->isGravityUp;
                else if (obj->isMiniPortal) state.isMini = obj->isMiniOn;
                else if (obj->isSpeedPortal) state.speed = obj->portalSpeed;
                else state.gameMode = obj->portalGameMode;
            }
            else if (obj->isPad) {
                float boost = 0;
                switch (obj->orbType) {
                    case 0: boost = 12.0f; break;
                    case 1: boost = 16.0f; break;
                    case 2: boost = 20.0f; break;
                    case 3: boost = -12.0f; break;
                    case 4: state.isUpsideDown = !state.isUpsideDown; continue;
                }
                if (state.isUpsideDown) boost = -boost;
                state.yVel = boost;
            }
        }
    }
    
    void handleOrbs(PlayerState& state, LevelAnalyzer* analyzer) {
        if (state.orbCooldown > 0) return;
        
        auto nearby = analyzer->getNearby(state.x, 30);
        
        for (auto* obj : nearby) {
            if (!obj->isOrb) continue;
            
            float psize = state.isMini ? 18.0f : 30.0f;
            if (!CollisionChecker::collides(state.x, state.y, psize, *obj)) continue;
            if (obj->id == state.lastOrbID) continue;
            
            float boost = 0;
            switch (obj->orbType) {
                case 0: boost = 11.2f; break;
                case 1: boost = 14.0f; break;
                case 2: boost = 18.0f; break;
                case 3: state.isUpsideDown = !state.isUpsideDown; boost = 8.0f; break;
                case 4: state.isUpsideDown = !state.isUpsideDown; boost = 11.2f; break;
                case 5: return;
                case 6: case 7: boost = 15.0f; break;
            }
            
            if (state.isUpsideDown && obj->orbType != 3 && obj->orbType != 4) {
                boost = -boost;
            }
            
            state.yVel = boost;
            state.orbCooldown = 0.1f;
            state.lastOrbID = obj->id;
            break;
        }
    }
    
    void reset() {
        m_wasHolding = false;
        m_holdStartX = 0;
    }
};

// ============================================================================
// CURRENT STATE
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
    
    float spd = player->m_playerSpeed;
    if (spd <= 0.8f) g_currentState.speed = SpeedType::Slow;
    else if (spd <= 0.95f) g_currentState.speed = SpeedType::Normal;
    else if (spd <= 1.05f) g_currentState.speed = SpeedType::Fast;
    else if (spd <= 1.15f) g_currentState.speed = SpeedType::Faster;
    else g_currentState.speed = SpeedType::Fastest;
}

// ============================================================================
// PLAY LAYER HOOKS
// ============================================================================

class $modify(AutoPlayLayer, PlayLayer) {
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        log::info("AutoPlayer: PlayLayer init");
        g_levelAnalyzed = false;
        g_isClicking = false;
        g_frameCounter = 0;
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        LevelAnalyzer::get()->analyze(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        if (g_isClicking) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isClicking = false;
        }
        
        ReactiveBot::get()->reset();
        g_frameCounter = 0;
        
        if (!g_levelAnalyzed) {
            LevelAnalyzer::get()->analyze(this);
        }
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        
        g_frameCounter++;
        
        if (!g_autoPlayerEnabled) return;
        if (!m_player1) return;
        if (m_isPaused || m_hasCompletedLevel || m_player1->m_isDead) return;
        if (!g_levelAnalyzed) return;
        
        updatePlayerState(m_player1);
        
        // Get bot decision
        bool shouldClick = ReactiveBot::get()->shouldClick(g_currentState, LevelAnalyzer::get());
        
        // Apply input
        if (shouldClick != g_isClicking) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(shouldClick, 1, true);
                g_isClicking = shouldClick;
                
                if (g_debugEnabled) {
                    log::info(">>> {} at x={:.1f} y={:.1f}", 
                        shouldClick ? "CLICK" : "RELEASE", 
                        g_currentState.x, g_currentState.y);
                }
            }
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        log::info("AutoPlayer: Level completed!");
        Notification::create("AutoPlayer: Level Complete!", NotificationIcon::Success)->show();
    }
    
    void onQuit() {
        if (g_isClicking) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isClicking = false;
        }
        g_levelAnalyzed = false;
        ReactiveBot::get()->reset();
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
            
            log::info("AutoPlayer: {}", g_autoPlayerEnabled ? "ENABLED" : "DISABLED");
            
            if (!g_autoPlayerEnabled && g_isClicking) {
                if (auto* gj = GJBaseGameLayer::get()) {
                    gj->handleButton(false, 1, true);
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
            log::info("Debug: {}", g_debugEnabled ? "ON" : "OFF");
            Notification::create(g_debugEnabled ? "Debug: ON" : "Debug: OFF", NotificationIcon::Info)->show();
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
    log::info("Press F9 to toggle Debug");
    log::info("========================================");
}
