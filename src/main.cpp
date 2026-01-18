#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
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
static float g_playerX = 0;
static float g_playerY = 0;
static float g_playerYVel = 0;
static bool g_playerOnGround = false;
static bool g_isCube = true;
static bool g_isUpsideDown = false;
static float g_xSpeed = 311.58f / 240.0f;

// ============================================================================
// HAZARD STORAGE
// ============================================================================

struct Hazard {
    float x, y, w, h;
};

static std::vector<Hazard> g_hazards;

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
// ANALYZE LEVEL
// ============================================================================

void analyzeLevel(PlayLayer* pl) {
    g_hazards.clear();
    g_levelAnalyzed = false;
    
    if (!pl || !pl->m_objects) {
        log::error("Bot: No objects!");
        return;
    }
    
    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;
        
        if (HAZARD_IDS.count(obj->m_objectID)) {
            Hazard h;
            h.x = obj->getPositionX();
            h.y = obj->getPositionY();
            auto size = obj->getContentSize();
            float scale = obj->getScale();
            h.w = size.width * scale * 0.8f;
            h.h = size.height * scale * 0.8f;
            g_hazards.push_back(h);
        }
    }
    
    std::sort(g_hazards.begin(), g_hazards.end(), [](const Hazard& a, const Hazard& b) {
        return a.x < b.x;
    });
    
    log::info("Bot: Analyzed {} hazards", g_hazards.size());
    g_levelAnalyzed = true;
}

// ============================================================================
// COLLISION CHECK
// ============================================================================

bool willHitHazard(float px, float py, float size = 30.0f) {
    float half = size / 2.0f;
    
    for (const auto& h : g_hazards) {
        if (h.x < px - 100 || h.x > px + 100) continue;
        
        if (px + half > h.x - h.w/2 &&
            px - half < h.x + h.w/2 &&
            py + half > h.y - h.h/2 &&
            py - half < h.y + h.h/2) {
            return true;
        }
    }
    
    if (py < 80 || py > 2050) return true;
    return false;
}

// ============================================================================
// SIMPLE SIMULATION
// ============================================================================

struct SimState {
    float x, y, yVel;
    bool onGround, canJump;
};

void simFrame(SimState& s, bool hold, float xSpd, bool upsideDown, bool cube) {
    if (cube) {
        float g = upsideDown ? -0.958f : 0.958f;
        s.yVel -= g;
        s.yVel = std::clamp(s.yVel, -15.0f, 15.0f);
        s.y += s.yVel;
        
        float groundY = upsideDown ? 2085.0f : 105.0f;
        bool hit = upsideDown ? (s.y >= groundY) : (s.y <= groundY);
        
        if (hit) {
            s.y = groundY;
            s.yVel = 0;
            s.onGround = true;
            s.canJump = true;
        } else {
            s.onGround = false;
        }
        
        if (hold && s.onGround && s.canJump) {
            s.yVel = upsideDown ? -11.18f : 11.18f;
            s.onGround = false;
            s.canJump = false;
        }
        if (!hold) s.canJump = true;
    } else {
        // Ship-like
        s.yVel += hold ? (upsideDown ? -0.8f : 0.8f) : (upsideDown ? 0.8f : -0.8f);
        s.yVel = std::clamp(s.yVel, -8.0f, 8.0f);
        s.y += s.yVel;
    }
    s.x += xSpd;
}

// ============================================================================
// BOT LOGIC
// ============================================================================

bool shouldClick() {
    // Simulate no click
    SimState sNo = {g_playerX, g_playerY, g_playerYVel, g_playerOnGround, true};
    int survNo = 0;
    for (int i = 0; i < 40; i++) {
        simFrame(sNo, false, g_xSpeed, g_isUpsideDown, g_isCube);
        if (willHitHazard(sNo.x, sNo.y)) break;
        survNo++;
    }
    
    // Simulate click
    SimState sYes = {g_playerX, g_playerY, g_playerYVel, g_playerOnGround, true};
    int survYes = 0;
    for (int i = 0; i < 40; i++) {
        simFrame(sYes, i < 10, g_xSpeed, g_isUpsideDown, g_isCube);
        if (willHitHazard(sYes.x, sYes.y)) break;
        survYes++;
    }
    
    return survYes > survNo;
}

// ============================================================================
// OVERLAY
// ============================================================================

class Overlay : public CCNode {
public:
    CCLabelBMFont* m_lbl1 = nullptr;
    CCLabelBMFont* m_lbl2 = nullptr;
    CCLabelBMFont* m_lbl3 = nullptr;
    CCDrawNode* m_draw = nullptr;
    
    static Overlay* create() {
        auto* r = new Overlay();
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
        // Status
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
        
        snprintf(buf, 128, "Ground:%s Clicks:%d Cube:%s", 
            g_playerOnGround?"Y":"N", g_totalClicks, g_isCube?"Y":"N");
        m_lbl3->setString(buf);
        
        // Debug draw
        m_draw->clear();
        if (!g_debugDraw || !g_levelAnalyzed) return;
        
        // Player dot
        m_draw->drawDot(ccp(g_playerX, g_playerY), 10, ccc4f(1,1,1,0.9f));
        
        // Hazards
        for (auto& h : g_hazards) {
            if (h.x > g_playerX - 100 && h.x < g_playerX + 400) {
                m_draw->drawDot(ccp(h.x, h.y), h.w/3, ccc4f(1,0,0,0.5f));
            }
        }
        
        // Paths
        SimState sNo = {g_playerX, g_playerY, g_playerYVel, g_playerOnGround, true};
        for (int i = 0; i < 50; i++) {
            float ox = sNo.x, oy = sNo.y;
            simFrame(sNo, false, g_xSpeed, g_isUpsideDown, g_isCube);
            m_draw->drawSegment(ccp(ox,oy), ccp(sNo.x,sNo.y), 1.5f, ccc4f(1,0.3f,0.3f,0.7f*(1-i/50.0f)));
            if (willHitHazard(sNo.x, sNo.y)) break;
        }
        
        SimState sYes = {g_playerX, g_playerY, g_playerYVel, g_playerOnGround, true};
        for (int i = 0; i < 50; i++) {
            float ox = sYes.x, oy = sYes.y;
            simFrame(sYes, i<10, g_xSpeed, g_isUpsideDown, g_isCube);
            m_draw->drawSegment(ccp(ox,oy), ccp(sYes.x,sYes.y), 1.5f, ccc4f(0.3f,1,0.3f,0.7f*(1-i/50.0f)));
            if (willHitHazard(sYes.x, sYes.y)) break;
        }
    }
};

static Overlay* g_overlay = nullptr;

// ============================================================================
// GJBASEGAMELAYER HOOK - This is where the game actually updates!
// ============================================================================

class $modify(BotGameLayer, GJBaseGameLayer) {
    
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        
        g_frameCounter++;
        
        // Log every 2 seconds to confirm update is running
        if (g_frameCounter % 120 == 1) {
            log::info("Bot: GJBaseGameLayer::update frame {}", g_frameCounter);
        }
        
        // Get PlayLayer
        auto* pl = PlayLayer::get();
        if (!pl) return;
        
        // Check conditions
        if (!g_botEnabled) return;
        if (!m_player1) return;
        if (pl->m_isPaused) return;
        if (pl->m_hasCompletedLevel) return;
        if (m_player1->m_isDead) return;
        if (!g_levelAnalyzed) return;
        
        // === READ PLAYER STATE ===
        g_playerX = m_player1->getPositionX();
        g_playerY = m_player1->getPositionY();
        g_playerYVel = m_player1->m_yVelocity;
        g_playerOnGround = m_player1->m_isOnGround;
        g_isUpsideDown = m_player1->m_isUpsideDown;
        
        g_isCube = !m_player1->m_isShip && !m_player1->m_isBall && 
                   !m_player1->m_isBird && !m_player1->m_isDart &&
                   !m_player1->m_isRobot && !m_player1->m_isSpider && 
                   !m_player1->m_isSwing;
        
        // Speed
        float spd = m_player1->m_playerSpeed;
        if (spd <= 0.8f) g_xSpeed = 251.16f / 240.0f;
        else if (spd <= 0.95f) g_xSpeed = 311.58f / 240.0f;
        else if (spd <= 1.05f) g_xSpeed = 387.42f / 240.0f;
        else if (spd <= 1.15f) g_xSpeed = 468.0f / 240.0f;
        else g_xSpeed = 576.0f / 240.0f;
        
        // Log position periodically
        if (g_frameCounter % 60 == 1) {
            log::info("Bot: pos=({:.0f},{:.0f}) vel={:.1f} ground={} cube={}",
                g_playerX, g_playerY, g_playerYVel, g_playerOnGround, g_isCube);
        }
        
        // === BOT DECISION ===
        bool click = shouldClick();
        
        // === APPLY INPUT ===
        if (click != g_isHolding) {
            this->handleButton(click, 1, true);
            g_isHolding = click;
            
            if (click) {
                g_totalClicks++;
                log::info("Bot: CLICK #{} at ({:.0f},{:.0f})", g_totalClicks, g_playerX, g_playerY);
            } else {
                log::info("Bot: RELEASE at ({:.0f},{:.0f})", g_playerX, g_playerY);
            }
        }
    }
};

// ============================================================================
// PLAYLAYER HOOK - Just for init/reset/analyze
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {
    
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        log::info("Bot: PlayLayer::init");
        
        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;
        g_totalClicks = 0;
        g_playerX = 0;
        g_playerY = 0;
        
        g_overlay = Overlay::create();
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
        
        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
        
        if (!g_levelAnalyzed) {
            analyzeLevel(this);
        }
        
        log::info("Bot: Reset (attempt {})", g_attempts);
    }
    
    void levelComplete() {
        PlayLayer::levelComplete();
        log::info("Bot: COMPLETE! {} clicks", g_totalClicks);
        Notification::create("Bot: Complete!", NotificationIcon::Success)->show();
    }
    
    void onQuit() {
        if (g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
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
        
        auto ws = CCDirector::sharedDirector()->getWinSize();
        auto* menu = CCMenu::create();
        menu->setPosition({0,0});
        addChild(menu, 100);
        
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
        
        auto* on2 = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* off2 = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        on2->setScale(0.7f); off2->setScale(0.7f);
        auto* t2 = CCMenuItemToggler::create(off2, on2, this, menu_selector(BotPauseLayer::onDebug));
        t2->setPosition({ws.width-30, ws.height-75});
        t2->toggle(g_debugDraw);
        menu->addChild(t2);
        
        auto* l2 = CCLabelBMFont::create("Debug", "bigFont.fnt");
        l2->setScale(0.25f);
        l2->setPosition({ws.width-30, ws.height-92});
        addChild(l2, 100);
    }
    
    void onBot(CCObject*) {
        g_botEnabled = !g_botEnabled;
        log::info("Bot: {} (menu)", g_botEnabled ? "ON" : "OFF");
        if (!g_botEnabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
    }
    
    void onDebug(CCObject*) {
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
                    if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
                    g_isHolding = false;
                }
                Notification::create(g_botEnabled ? "Bot: ON" : "Bot: OFF",
                    g_botEnabled ? NotificationIcon::Success : NotificationIcon::Info)->show();
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
    log::info("=====================================");
    log::info("  Bot Loaded! F8=Toggle F9=Debug");
    log::info("=====================================");
}
