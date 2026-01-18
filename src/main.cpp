#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
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
static bool g_debugDraw = false;
static bool g_isHolding = false;
static bool g_levelAnalyzed = false;
static int g_frameCounter = 0;
static int g_totalClicks = 0;
static int g_attempts = 0;
static float g_bestProgress = 0.0f;

// ============================================================================
// SIMPLE HAZARD STORAGE
// ============================================================================

struct SimpleHazard {
    float x;
    float y;
    float width;
    float height;
};

static std::vector<SimpleHazard> g_hazards;

// ============================================================================
// HAZARD IDS
// ============================================================================

static const std::set<int> HAZARD_IDS = {
    8, 39, 103, 392, 9, 61,
    243, 244, 245, 246, 247, 248, 249,
    363, 364, 365, 366, 367, 368,
    446, 447, 678, 679, 680,
    1705, 1706, 1707, 1708, 1709, 1710,
    1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718
};

// ============================================================================
// ANALYZE LEVEL - Just get hazards
// ============================================================================

void analyzeLevel(PlayLayer* pl) {
    g_hazards.clear();
    g_levelAnalyzed = false;
    
    if (!pl || !pl->m_objects) {
        log::error("Bot: No level objects!");
        return;
    }
    
    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;
        
        if (HAZARD_IDS.count(obj->m_objectID)) {
            SimpleHazard h;
            h.x = obj->getPositionX();
            h.y = obj->getPositionY();
            auto size = obj->getContentSize();
            float scale = obj->getScale();
            h.width = size.width * scale * 0.8f;
            h.height = size.height * scale * 0.8f;
            g_hazards.push_back(h);
        }
    }
    
    // Sort by X
    std::sort(g_hazards.begin(), g_hazards.end(), [](const SimpleHazard& a, const SimpleHazard& b) {
        return a.x < b.x;
    });
    
    log::info("Bot: Found {} hazards", g_hazards.size());
    g_levelAnalyzed = true;
}

// ============================================================================
// CHECK COLLISION
// ============================================================================

bool checkHazardCollision(float px, float py, float playerSize) {
    float half = playerSize / 2.0f;
    
    for (const auto& h : g_hazards) {
        // Skip if too far
        if (h.x < px - 100 || h.x > px + 100) continue;
        
        // AABB collision
        if (px + half > h.x - h.width/2 &&
            px - half < h.x + h.width/2 &&
            py + half > h.y - h.height/2 &&
            py - half < h.y + h.height/2) {
            return true;
        }
    }
    
    // Check bounds
    if (py < 80 || py > 2050) return true;
    
    return false;
}

// ============================================================================
// SIMPLE PHYSICS SIMULATION
// ============================================================================

struct SimState {
    float x;
    float y;
    float yVel;
    bool onGround;
    bool canJump;
};

void simulateFrame(SimState& s, bool holding, float xSpeed, float gravity, bool upsideDown, bool isCube) {
    if (isCube) {
        // Apply gravity
        float g = upsideDown ? -gravity : gravity;
        s.yVel -= g;
        s.yVel = std::clamp(s.yVel, -15.0f, 15.0f);
        s.y += s.yVel;
        
        // Ground check
        float groundY = upsideDown ? 2085.0f : 105.0f;
        bool hitGround = upsideDown ? (s.y >= groundY) : (s.y <= groundY);
        
        if (hitGround) {
            s.y = groundY;
            s.yVel = 0;
            s.onGround = true;
            s.canJump = true;
        } else {
            s.onGround = false;
        }
        
        // Jump
        if (holding && s.onGround && s.canJump) {
            s.yVel = upsideDown ? -11.18f : 11.18f;
            s.onGround = false;
            s.canJump = false;
        }
        
        if (!holding) {
            s.canJump = true;
        }
    } else {
        // Ship-like: just go up/down based on holding
        if (holding) {
            s.yVel += upsideDown ? -0.8f : 0.8f;
        } else {
            s.yVel += upsideDown ? 0.8f : -0.8f;
        }
        s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
        s.y += s.yVel;
    }
    
    s.x += xSpeed;
}

// ============================================================================
// BOT DECISION
// ============================================================================

bool shouldBotClick(float px, float py, float yVel, bool onGround, bool upsideDown, bool isCube, float xSpeed) {
    float gravity = 0.958f;
    float playerSize = 30.0f;
    
    // Simulate NOT clicking
    SimState simNo;
    simNo.x = px;
    simNo.y = py;
    simNo.yVel = yVel;
    simNo.onGround = onGround;
    simNo.canJump = true;
    
    int surviveNo = 0;
    for (int i = 0; i < 40; i++) {
        simulateFrame(simNo, false, xSpeed, gravity, upsideDown, isCube);
        if (checkHazardCollision(simNo.x, simNo.y, playerSize)) break;
        surviveNo++;
    }
    
    // Simulate clicking
    SimState simYes;
    simYes.x = px;
    simYes.y = py;
    simYes.yVel = yVel;
    simYes.onGround = onGround;
    simYes.canJump = true;
    
    int surviveYes = 0;
    for (int i = 0; i < 40; i++) {
        bool hold = (i < 10); // Hold for 10 frames
        simulateFrame(simYes, hold, xSpeed, gravity, upsideDown, isCube);
        if (checkHazardCollision(simYes.x, simYes.y, playerSize)) break;
        surviveYes++;
    }
    
    // Click if it helps us survive longer
    return surviveYes > surviveNo;
}

// ============================================================================
// OVERLAY
// ============================================================================

class BotOverlay : public CCNode {
public:
    CCLabelBMFont* m_label1 = nullptr;
    CCLabelBMFont* m_label2 = nullptr;
    CCLabelBMFont* m_label3 = nullptr;
    CCDrawNode* m_draw = nullptr;
    
    // Store current player info for display
    float m_px = 0, m_py = 0, m_pyVel = 0;
    bool m_onGround = false;
    bool m_isCube = true;
    
    static BotOverlay* create() {
        auto* ret = new BotOverlay();
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
        
        m_label1 = CCLabelBMFont::create("Bot: OFF", "bigFont.fnt");
        m_label1->setScale(0.4f);
        m_label1->setAnchorPoint({0, 1});
        m_label1->setPosition({5, 320});
        addChild(m_label1, 100);
        
        m_label2 = CCLabelBMFont::create("", "chatFont.fnt");
        m_label2->setScale(0.5f);
        m_label2->setAnchorPoint({0, 1});
        m_label2->setPosition({5, 295});
        addChild(m_label2, 100);
        
        m_label3 = CCLabelBMFont::create("", "chatFont.fnt");
        m_label3->setScale(0.5f);
        m_label3->setAnchorPoint({0, 1});
        m_label3->setPosition({5, 275});
        addChild(m_label3, 100);
        
        scheduleUpdate();
        return true;
    }
    
    void updateInfo(float px, float py, float yVel, bool onGround, bool isCube) {
        m_px = px;
        m_py = py;
        m_pyVel = yVel;
        m_onGround = onGround;
        m_isCube = isCube;
    }
    
    void update(float dt) override {
        // Update labels
        if (g_botEnabled) {
            m_label1->setString("Bot: ON");
            m_label1->setColor(ccc3(0, 255, 0));
        } else {
            m_label1->setString("Bot: OFF");
            m_label1->setColor(ccc3(255, 100, 100));
        }
        
        char buf[128];
        snprintf(buf, sizeof(buf), "X:%.0f Y:%.0f Vel:%.1f", m_px, m_py, m_pyVel);
        m_label2->setString(buf);
        
        snprintf(buf, sizeof(buf), "Ground:%s Clicks:%d Mode:%s", 
            m_onGround ? "Y" : "N", 
            g_totalClicks,
            m_isCube ? "Cube" : "Other");
        m_label3->setString(buf);
        
        // Draw debug
        m_draw->clear();
        if (!g_debugDraw) return;
        
        // Draw player
        m_draw->drawDot(ccp(m_px, m_py), 10, ccc4f(1, 1, 1, 0.8f));
        
        // Draw nearby hazards
        for (const auto& h : g_hazards) {
            if (h.x > m_px - 100 && h.x < m_px + 400) {
                m_draw->drawDot(ccp(h.x, h.y), h.width/3, ccc4f(1, 0, 0, 0.5f));
            }
        }
        
        // Draw predicted paths
        float xSpeed = 311.58f / 240.0f; // Normal speed per frame
        float gravity = 0.958f;
        
        // No-click path (red)
        SimState simNo = {m_px, m_py, m_pyVel, m_onGround, true};
        for (int i = 0; i < 50; i++) {
            float oldX = simNo.x, oldY = simNo.y;
            simulateFrame(simNo, false, xSpeed, gravity, false, m_isCube);
            float alpha = 1.0f - (float)i/50.0f;
            m_draw->drawSegment(ccp(oldX, oldY), ccp(simNo.x, simNo.y), 1.5f, ccc4f(1, 0.3f, 0.3f, alpha*0.7f));
            if (checkHazardCollision(simNo.x, simNo.y, 30)) break;
        }
        
        // Click path (green)
        SimState simYes = {m_px, m_py, m_pyVel, m_onGround, true};
        for (int i = 0; i < 50; i++) {
            float oldX = simYes.x, oldY = simYes.y;
            simulateFrame(simYes, i < 10, xSpeed, gravity, false, m_isCube);
            float alpha = 1.0f - (float)i/50.0f;
            m_draw->drawSegment(ccp(oldX, oldY), ccp(simYes.x, simYes.y), 1.5f, ccc4f(0.3f, 1, 0.3f, alpha*0.7f));
            if (checkHazardCollision(simYes.x, simYes.y, 30)) break;
        }
    }
};

static BotOverlay* g_overlay = nullptr;

// ============================================================================
// PLAYLAYER HOOK
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        float lastX = 0;
        float lastY = 0;
    };
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        log::info("Bot: PlayLayer::init");
        
        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;
        g_totalClicks = 0;
        m_fields->lastX = 0;
        m_fields->lastY = 0;
        
        // Create overlay
        g_overlay = BotOverlay::create();
        g_overlay->setZOrder(9999);
        addChild(g_overlay);
        
        return true;
    }
    
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        log::info("Bot: setupHasCompleted");
        analyzeLevel(this);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        g_attempts++;
        g_frameCounter = 0;
        g_totalClicks = 0;
        m_fields->lastX = 0;
        m_fields->lastY = 0;
        
        if (g_isHolding) {
            if (auto gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        if (!g_levelAnalyzed) {
            analyzeLevel(this);
        }
        
        log::info("Bot: Reset (attempt {})", g_attempts);
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        
        g_frameCounter++;
        
        // === CRITICAL DEBUG: Check if we even get here ===
        if (g_frameCounter % 120 == 1) {
            log::info("Bot: Frame {} | enabled={} | player={} | paused={} | dead={} | analyzed={}",
                g_frameCounter,
                g_botEnabled,
                m_player1 != nullptr,
                m_isPaused,
                m_player1 ? m_player1->m_isDead : true,
                g_levelAnalyzed);
        }
        
        if (!g_botEnabled) return;
        if (!m_player1) return;
        if (m_isPaused) return;
        if (m_hasCompletedLevel) return;
        if (m_player1->m_isDead) return;
        if (!g_levelAnalyzed) return;
        
        // === GET PLAYER STATE ===
        float px = m_player1->getPositionX();
        float py = m_player1->getPositionY();
        float yVel = m_player1->m_yVelocity;
        bool onGround = m_player1->m_isOnGround;
        bool upsideDown = m_player1->m_isUpsideDown;
        
        // Determine gamemode
        bool isCube = !m_player1->m_isShip && !m_player1->m_isBall && 
                      !m_player1->m_isBird && !m_player1->m_isDart &&
                      !m_player1->m_isRobot && !m_player1->m_isSpider && !m_player1->m_isSwing;
        
        // Get speed
        float speedMult = 311.58f; // Normal speed
        float playerSpeed = m_player1->m_playerSpeed;
        if (playerSpeed <= 0.8f) speedMult = 251.16f;
        else if (playerSpeed <= 0.95f) speedMult = 311.58f;
        else if (playerSpeed <= 1.05f) speedMult = 387.42f;
        else if (playerSpeed <= 1.15f) speedMult = 468.0f;
        else speedMult = 576.0f;
        
        float xSpeed = speedMult / 240.0f;
        
        // === DEBUG: Log position changes ===
        if (std::abs(px - m_fields->lastX) > 50 || g_frameCounter % 60 == 1) {
            log::info("Bot: pos=({:.0f},{:.0f}) vel={:.1f} ground={} cube={}", 
                px, py, yVel, onGround, isCube);
            m_fields->lastX = px;
            m_fields->lastY = py;
        }
        
        // Update overlay
        if (g_overlay) {
            g_overlay->updateInfo(px, py, yVel, onGround, isCube);
        }
        
        // === BOT DECISION ===
        bool shouldClick = shouldBotClick(px, py, yVel, onGround, upsideDown, isCube, xSpeed);
        
        // === APPLY INPUT ===
        if (shouldClick != g_isHolding) {
            auto* gj = GJBaseGameLayer::get();
            if (gj) {
                gj->handleButton(shouldClick, 1, true);
                g_isHolding = shouldClick;
                
                if (shouldClick) {
                    g_totalClicks++;
                    log::info("Bot: CLICK #{} at ({:.0f}, {:.0f})", g_totalClicks, px, py);
                } else {
                    log::info("Bot: RELEASE at ({:.0f}, {:.0f})", px, py);
                }
            } else {
                log::error("Bot: GJBaseGameLayer is NULL!");
            }
        }
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        log::info("Bot: LEVEL COMPLETE! Clicks: {}", g_totalClicks);
        Notification::create("Bot: Level Complete!", NotificationIcon::Success)->show();
    }
    
    void onQuit() {
        if (g_isHolding) {
            if (auto gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
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
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        addChild(menu, 100);
        
        // Bot toggle
        auto* onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* toggle = CCMenuItemToggler::create(offSpr, onSpr, this, menu_selector(BotPauseLayer::onToggleBot));
        toggle->setPosition({winSize.width - 30, winSize.height - 30});
        toggle->toggle(g_botEnabled);
        menu->addChild(toggle);
        
        auto* label = CCLabelBMFont::create("Bot", "bigFont.fnt");
        label->setScale(0.35f);
        label->setPosition({winSize.width - 30, winSize.height - 50});
        addChild(label, 100);
        
        // Debug toggle
        auto* onSpr2 = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* offSpr2 = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        onSpr2->setScale(0.7f);
        offSpr2->setScale(0.7f);
        auto* toggle2 = CCMenuItemToggler::create(offSpr2, onSpr2, this, menu_selector(BotPauseLayer::onToggleDebug));
        toggle2->setPosition({winSize.width - 30, winSize.height - 75});
        toggle2->toggle(g_debugDraw);
        menu->addChild(toggle2);
        
        auto* label2 = CCLabelBMFont::create("Debug", "bigFont.fnt");
        label2->setScale(0.25f);
        label2->setPosition({winSize.width - 30, winSize.height - 92});
        addChild(label2, 100);
    }
    
    void onToggleBot(CCObject*) {
        g_botEnabled = !g_botEnabled;
        log::info("Bot: {} (menu)", g_botEnabled ? "ON" : "OFF");
        
        if (!g_botEnabled && g_isHolding) {
            if (auto gj = GJBaseGameLayer::get()) {
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
// KEYBOARD
// ============================================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            if (key == KEY_F8) {
                g_botEnabled = !g_botEnabled;
                log::info("Bot: {} (F8)", g_botEnabled ? "ON" : "OFF");
                
                if (!g_botEnabled && g_isHolding) {
                    if (auto gj = GJBaseGameLayer::get()) {
                        gj->handleButton(false, 1, true);
                    }
                    g_isHolding = false;
                }
                
                Notification::create(
                    g_botEnabled ? "Bot: ON" : "Bot: OFF",
                    g_botEnabled ? NotificationIcon::Success : NotificationIcon::Info
                )->show();
                return true;
            }
            
            if (key == KEY_F9) {
                g_debugDraw = !g_debugDraw;
                log::info("Debug: {}", g_debugDraw ? "ON" : "OFF");
                Notification::create(g_debugDraw ? "Debug: ON" : "Debug: OFF", NotificationIcon::Info)->show();
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
    log::info("===========================================");
    log::info("  Bot Mod Loaded!");
    log::info("  F8 = Toggle Bot");
    log::info("  F9 = Toggle Debug");
    log::info("===========================================");
}
