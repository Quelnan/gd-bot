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

static bool g_enabled = false;
static bool g_debugDraw = false;
static bool g_isHolding = false;
static bool g_analyzed = false;
static int g_frameCount = 0;
static int g_clickCount = 0;
static float g_lastX = 0;

// ============================================================================
// ENUMS
// ============================================================================

enum class GameMode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };
enum class Speed { Slow, Normal, Fast, Faster, Fastest };

// ============================================================================
// PLAYER STATE
// ============================================================================

struct PlayerState {
    float x = 0, y = 105, yVel = 0;
    GameMode mode = GameMode::Cube;
    Speed speed = Speed::Normal;
    bool upsideDown = false;
    bool mini = false;
    bool onGround = true;
    bool canJump = true;
};

// ============================================================================
// LEVEL OBJECT
// ============================================================================

struct Obj {
    float x, y, w, h;
    bool hazard = false;
    bool orb = false;
    bool pad = false;
    int type = 0;
};

// ============================================================================
// GLOBAL LEVEL DATA
// ============================================================================

static std::vector<Obj> g_objects;
static PlayerState g_state;

// ============================================================================
// PHYSICS
// ============================================================================

float getSpeedMult(Speed s) {
    switch(s) {
        case Speed::Slow: return 251.16f;
        case Speed::Normal: return 311.58f;
        case Speed::Fast: return 387.42f;
        case Speed::Faster: return 468.0f;
        case Speed::Fastest: return 576.0f;
    }
    return 311.58f;
}

void simulate(PlayerState& s, bool hold, float dt = 1.0f/240.0f) {
    float gravity = s.mini ? 0.77f : 0.958f;
    if (s.upsideDown) gravity = -gravity;
    float groundY = s.upsideDown ? 2085.0f : 105.0f;
    float xSpeed = getSpeedMult(s.speed) * dt;
    
    if (s.mode == GameMode::Cube || s.mode == GameMode::Robot || s.mode == GameMode::Spider) {
        s.yVel -= gravity;
        s.yVel = std::clamp(s.yVel, -15.0f, 15.0f);
        s.y += s.yVel;
        
        bool hitGround = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
        if (hitGround) {
            s.y = groundY;
            s.yVel = 0;
            s.onGround = true;
        } else {
            s.onGround = false;
        }
        
        if (hold && s.onGround && s.canJump) {
            float jump = s.mini ? 9.4f : 11.18f;
            s.yVel = s.upsideDown ? -jump : jump;
            s.onGround = false;
            s.canJump = false;
        }
        if (!hold) s.canJump = true;
    }
    else if (s.mode == GameMode::Ship) {
        float acc = s.mini ? 0.6f : 0.8f;
        float maxV = s.mini ? 6.0f : 8.0f;
        s.yVel += hold ? (s.upsideDown ? -acc : acc) : (s.upsideDown ? acc : -acc);
        s.yVel = std::clamp(s.yVel, -maxV, maxV);
        s.y += s.yVel;
    }
    else if (s.mode == GameMode::Ball) {
        s.yVel -= gravity * 0.6f;
        s.yVel = std::clamp(s.yVel, -12.0f, 12.0f);
        s.y += s.yVel;
        
        bool hitGround = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
        if (hitGround) {
            s.y = groundY;
            s.yVel = 0;
            s.onGround = true;
        } else {
            s.onGround = false;
        }
        
        if (hold && s.onGround && s.canJump) {
            s.upsideDown = !s.upsideDown;
            s.yVel = s.upsideDown ? -6.0f : 6.0f;
            s.canJump = false;
        }
        if (!hold) s.canJump = true;
    }
    else if (s.mode == GameMode::UFO) {
        s.yVel -= gravity * 0.5f;
        s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
        s.y += s.yVel;
        
        if (hold && s.canJump) {
            s.yVel = s.upsideDown ? -7.0f : 7.0f;
            s.canJump = false;
        }
        if (!hold) s.canJump = true;
    }
    else if (s.mode == GameMode::Wave) {
        float ws = xSpeed * (s.mini ? 0.7f : 1.0f);
        s.y += hold ? (s.upsideDown ? -ws : ws) : (s.upsideDown ? ws : -ws);
    }
    
    s.x += xSpeed;
    s.y = std::clamp(s.y, 50.0f, 2100.0f);
}

// ============================================================================
// COLLISION
// ============================================================================

bool collides(float px, float py, float size, const Obj& o) {
    float h = size / 2;
    return !(px + h < o.x - o.w/2 || px - h > o.x + o.w/2 ||
             py + h < o.y - o.h/2 || py - h > o.y + o.h/2);
}

bool willDie(const PlayerState& s) {
    float size = s.mini ? 18.0f : 30.0f;
    if (s.mode == GameMode::Wave) size *= 0.6f;
    
    for (auto& o : g_objects) {
        if (o.hazard && o.x > s.x - 50 && o.x < s.x + 50) {
            if (collides(s.x, s.y, size, o)) return true;
        }
    }
    if (s.y < 60 || s.y > 2080) return true;
    return false;
}

// ============================================================================
// BOT LOGIC
// ============================================================================

bool botDecide(const PlayerState& current) {
    // Simulate both options
    PlayerState simHold = current;
    PlayerState simNoHold = current;
    
    int surviveHold = 0;
    int surviveNoHold = 0;
    
    // Simulate 40 frames ahead
    for (int i = 0; i < 40; i++) {
        simulate(simNoHold, false);
        if (!willDie(simNoHold)) surviveNoHold++;
        else break;
    }
    
    for (int i = 0; i < 40; i++) {
        bool hold = (i < 12); // Hold for 12 frames then release
        simulate(simHold, hold);
        if (!willDie(simHold)) surviveHold++;
        else break;
    }
    
    // Click if holding survives longer, or if we're about to die
    return surviveHold > surviveNoHold || (surviveNoHold < 5 && surviveHold >= surviveNoHold);
}

// ============================================================================
// LEVEL ANALYZER
// ============================================================================

void analyzeLevel(PlayLayer* pl) {
    g_objects.clear();
    g_analyzed = false;
    
    if (!pl || !pl->m_objects) {
        log::error("AutoPlayer: No level objects!");
        return;
    }
    
    static std::set<int> hazardIDs = {
        8, 39, 103, 392, 9, 61, 243, 244, 245, 246, 247, 248, 249,
        363, 364, 365, 366, 367, 368, 446, 447, 678, 679, 680,
        1705, 1706, 1707, 1708, 1709, 1710, 1711, 1712, 1713, 1714
    };
    
    static std::map<int, int> orbIDs = {{36,0},{84,1},{141,2},{1022,3},{1330,4},{1333,5}};
    static std::map<int, int> padIDs = {{35,0},{67,1},{140,2},{1332,3},{452,4}};
    
    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* go = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!go) continue;
        
        int id = go->m_objectID;
        bool isHaz = hazardIDs.count(id) > 0;
        bool isOrb = orbIDs.count(id) > 0;
        bool isPad = padIDs.count(id) > 0;
        
        if (!isHaz && !isOrb && !isPad) continue;
        
        Obj o;
        o.x = go->getPositionX();
        o.y = go->getPositionY();
        auto cs = go->getContentSize();
        float sc = go->getScale();
        o.w = cs.width * sc * 0.8f;
        o.h = cs.height * sc * 0.8f;
        o.hazard = isHaz;
        o.orb = isOrb;
        o.pad = isPad;
        if (isOrb) o.type = orbIDs[id];
        if (isPad) o.type = padIDs[id];
        
        g_objects.push_back(o);
    }
    
    std::sort(g_objects.begin(), g_objects.end(), [](const Obj& a, const Obj& b) {
        return a.x < b.x;
    });
    
    log::info("AutoPlayer: Analyzed {} objects", g_objects.size());
    g_analyzed = true;
}

// ============================================================================
// UPDATE PLAYER STATE FROM GAME
// ============================================================================

void syncState(PlayerObject* p) {
    if (!p) return;
    
    g_state.x = p->getPositionX();
    g_state.y = p->getPositionY();
    g_state.yVel = p->m_yVelocity;
    g_state.upsideDown = p->m_isUpsideDown;
    g_state.mini = p->m_vehicleSize != 1.0f;
    g_state.onGround = p->m_isOnGround;
    
    if (p->m_isShip) g_state.mode = GameMode::Ship;
    else if (p->m_isBall) g_state.mode = GameMode::Ball;
    else if (p->m_isBird) g_state.mode = GameMode::UFO;
    else if (p->m_isDart) g_state.mode = GameMode::Wave;
    else if (p->m_isRobot) g_state.mode = GameMode::Robot;
    else if (p->m_isSpider) g_state.mode = GameMode::Spider;
    else g_state.mode = GameMode::Cube;
    
    float spd = p->m_playerSpeed;
    if (spd <= 0.8f) g_state.speed = Speed::Slow;
    else if (spd <= 0.95f) g_state.speed = Speed::Normal;
    else if (spd <= 1.05f) g_state.speed = Speed::Fast;
    else if (spd <= 1.15f) g_state.speed = Speed::Faster;
    else g_state.speed = Speed::Fastest;
}

// ============================================================================
// DEBUG OVERLAY
// ============================================================================

class DebugOverlay : public CCNode {
public:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_infoLabel = nullptr;
    CCDrawNode* m_drawNode = nullptr;
    
    static DebugOverlay* create() {
        auto* ret = new DebugOverlay();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
    
    bool init() override {
        if (!CCNode::init()) return false;
        
        m_drawNode = CCDrawNode::create();
        addChild(m_drawNode, 100);
        
        m_statusLabel = CCLabelBMFont::create("AutoPlayer: OFF", "bigFont.fnt");
        m_statusLabel->setScale(0.4f);
        m_statusLabel->setAnchorPoint({0, 1});
        m_statusLabel->setPosition({5, 315});
        addChild(m_statusLabel, 101);
        
        m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_infoLabel->setScale(0.6f);
        m_infoLabel->setAnchorPoint({0, 1});
        m_infoLabel->setPosition({5, 290});
        addChild(m_infoLabel, 101);
        
        scheduleUpdate();
        return true;
    }
    
    void update(float dt) override {
        // Update status
        if (g_enabled) {
            m_statusLabel->setString("AutoPlayer: ON");
            m_statusLabel->setColor({0, 255, 0});
        } else {
            m_statusLabel->setString("AutoPlayer: OFF");
            m_statusLabel->setColor({255, 100, 100});
        }
        
        // Update info
        char buf[256];
        snprintf(buf, sizeof(buf), 
            "X: %.0f  Y: %.0f  Clicks: %d\nMode: %d  OnGround: %s",
            g_state.x, g_state.y, g_clickCount,
            (int)g_state.mode, g_state.onGround ? "Y" : "N"
        );
        m_infoLabel->setString(buf);
        
        // Draw debug visualization
        m_drawNode->clear();
        
        if (!g_debugDraw) return;
        
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) return;
        
        // Draw predicted trajectories
        PlayerState simNoClick = g_state;
        PlayerState simClick = g_state;
        
        // No click path (red)
        for (int i = 0; i < 60; i++) {
            CCPoint from = ccp(simNoClick.x, simNoClick.y);
            simulate(simNoClick, false);
            CCPoint to = ccp(simNoClick.x, simNoClick.y);
            float alpha = 1.0f - (float)i / 60.0f;
            m_drawNode->drawSegment(from, to, 1.5f, ccc4f(1, 0.3f, 0.3f, alpha * 0.8f));
        }
        
        // Click path (green)
        for (int i = 0; i < 60; i++) {
            CCPoint from = ccp(simClick.x, simClick.y);
            simulate(simClick, i < 12);
            CCPoint to = ccp(simClick.x, simClick.y);
            float alpha = 1.0f - (float)i / 60.0f;
            m_drawNode->drawSegment(from, to, 1.5f, ccc4f(0.3f, 1, 0.3f, alpha * 0.8f));
        }
        
        // Draw nearby hazards
        for (auto& o : g_objects) {
            if (o.hazard && o.x > g_state.x - 100 && o.x < g_state.x + 400) {
                m_drawNode->drawDot(ccp(o.x, o.y), o.w / 3, ccc4f(1, 0, 0, 0.5f));
            }
        }
        
        // Draw player position
        m_drawNode->drawDot(ccp(g_state.x, g_state.y), 8, ccc4f(1, 1, 1, 0.9f));
    }
};

static DebugOverlay* g_overlay = nullptr;

// ============================================================================
// PAUSE MENU BUTTON
// ============================================================================

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        // Create menu for bot controls
        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 100);
        
        // Toggle button
        auto* toggleBtn = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(BotPauseLayer::onToggleBot),
            g_enabled ? 1.0f : 0.5f
        );
        toggleBtn->setPosition({winSize.width - 50, winSize.height - 50});
        toggleBtn->toggle(g_enabled);
        menu->addChild(toggleBtn);
        
        // Label
        auto* label = CCLabelBMFont::create("AutoBot", "bigFont.fnt");
        label->setScale(0.4f);
        label->setPosition({winSize.width - 50, winSize.height - 75});
        this->addChild(label, 100);
        
        // Debug toggle
        auto* debugBtn = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(BotPauseLayer::onToggleDebug),
            g_debugDraw ? 1.0f : 0.5f
        );
        debugBtn->setPosition({winSize.width - 50, winSize.height - 110});
        debugBtn->toggle(g_debugDraw);
        debugBtn->setScale(0.7f);
        menu->addChild(debugBtn);
        
        auto* debugLabel = CCLabelBMFont::create("Debug", "bigFont.fnt");
        debugLabel->setScale(0.3f);
        debugLabel->setPosition({winSize.width - 50, winSize.height - 130});
        this->addChild(debugLabel, 100);
    }
    
    void onToggleBot(CCObject* sender) {
        g_enabled = !g_enabled;
        log::info("AutoPlayer: {} via menu", g_enabled ? "ENABLED" : "DISABLED");
        
        if (!g_enabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
    }
    
    void onToggleDebug(CCObject* sender) {
        g_debugDraw = !g_debugDraw;
        log::info("Debug draw: {}", g_debugDraw ? "ON" : "OFF");
    }
};

// ============================================================================
// PLAY LAYER HOOKS
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        log::info("AutoPlayer: Level init");
        
        g_analyzed = false;
        g_isHolding = false;
        g_frameCount = 0;
        g_clickCount = 0;
        
        // Add overlay
        g_overlay = DebugOverlay::create();
        g_overlay->setZOrder(1000);
        this->addChild(g_overlay);
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        analyzeLevel(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        log::info("AutoPlayer: Reset (enabled={}, analyzed={})", g_enabled, g_analyzed);
        
        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        g_frameCount = 0;
        g_clickCount = 0;
        
        if (!g_analyzed) {
            analyzeLevel(this);
        }
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        
        g_frameCount++;
        
        // Early exit checks with logging
        if (!g_enabled) return;
        
        if (!m_player1) {
            if (g_frameCount % 60 == 0) log::warn("No player1!");
            return;
        }
        
        if (m_isPaused) return;
        if (m_hasCompletedLevel) return;
        if (m_player1->m_isDead) return;
        
        if (!g_analyzed) {
            if (g_frameCount % 60 == 0) log::warn("Level not analyzed!");
            return;
        }
        
        // Sync state from game
        syncState(m_player1);
        
        // Log state periodically
        if (g_frameCount % 120 == 1) {
            log::info("Frame {}: x={:.0f} y={:.0f} ground={} mode={}", 
                g_frameCount, g_state.x, g_state.y, g_state.onGround, (int)g_state.mode);
        }
        
        // Get bot decision
        bool shouldClick = botDecide(g_state);
        
        // Apply input if changed
        if (shouldClick != g_isHolding) {
            auto* gj = GJBaseGameLayer::get();
            if (gj) {
                gj->handleButton(shouldClick, 1, true);
                g_isHolding = shouldClick;
                
                if (shouldClick) {
                    g_clickCount++;
                    log::info("CLICK #{} at x={:.0f} y={:.0f}", g_clickCount, g_state.x, g_state.y);
                }
            } else {
                log::error("GJBaseGameLayer is null!");
            }
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        log::info("AutoPlayer: Level complete! Clicks: {}", g_clickCount);
        Notification::create(
            fmt::format("AutoPlayer Complete!\n{} clicks", g_clickCount),
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
        g_analyzed = false;
        g_overlay = nullptr;
        PlayLayer::onQuit();
    }
};

// ============================================================================
// KEYBOARD (backup)
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            if (key == KEY_F8) {
                g_enabled = !g_enabled;
                log::info("AutoPlayer: {} (F8)", g_enabled ? "ON" : "OFF");
                
                if (!g_enabled && g_isHolding) {
                    if (auto* gj = GJBaseGameLayer::get()) {
                        gj->handleButton(false, 1, true);
                    }
                    g_isHolding = false;
                }
                
                Notification::create(
                    g_enabled ? "AutoPlayer: ON" : "AutoPlayer: OFF",
                    g_enabled ? NotificationIcon::Success : NotificationIcon::Info
                )->show();
                return true;
            }
            
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
// MOD INIT
// ============================================================================

$on_mod(Loaded) {
    log::info("========================================");
    log::info("   AutoPlayer Bot Loaded!");
    log::info("   F8 = Toggle Bot");
    log::info("   F9 = Toggle Debug Draw");
    log::info("   Or use Pause Menu buttons");
    log::info("========================================");
}
