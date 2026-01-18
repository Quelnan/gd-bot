#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>

// Geode 4 settings popup
// Settings popup header location differs between Geode SDK builds.
// Use __has_include to avoid build errors.
#if __has_include(<Geode/ui/ModSettingsPopup.hpp>)
    #include <Geode/ui/ModSettingsPopup.hpp>
    #define GDBOT_HAS_MOD_SETTINGS_POPUP 1
#elif __has_include(<Geode/loader/ModSettingsPopup.hpp>)
    #include <Geode/loader/ModSettingsPopup.hpp>
    #define GDBOT_HAS_MOD_SETTINGS_POPUP 1
#else
    #define GDBOT_HAS_MOD_SETTINGS_POPUP 0
#endif

#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ============================
// Globals / settings
// ============================

static bool g_botEnabled = false;
static bool g_levelAnalyzed = false;
static bool g_isHolding = false;
static int  g_frameCounter = 0;

static float g_playerX = 0.f;
static float g_playerY = 0.f;
static float g_playerYVel = 0.f;
static bool  g_playerOnGround = false;
static bool  g_isUpsideDown = false;
static bool  g_isMini = false;
static float g_xSpeed = 311.58f / 240.f;

enum class BotGameMode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };
static BotGameMode g_gameMode = BotGameMode::Cube;

struct BotSettings {
    int simFrames = 60;
    int holdDuration = 12;
    float hitboxScale = 0.9f;
    float hazardHitboxScale = 1.0f;
    float safeMargin = 2.0f;
    float reactionDistance = 150.0f;
    bool debugOverlay = true;
    bool debugDraw = false;

    void load() {
        auto* mod = Mod::get();
        simFrames = (int)mod->getSettingValue<int64_t>("sim-frames");
        holdDuration = (int)mod->getSettingValue<int64_t>("hold-duration");
        hitboxScale = (float)mod->getSettingValue<double>("hitbox-scale");
        hazardHitboxScale = (float)mod->getSettingValue<double>("hazard-hitbox-scale");
        safeMargin = (float)mod->getSettingValue<double>("safe-margin");
        reactionDistance = (float)mod->getSettingValue<double>("reaction-distance");
        debugOverlay = mod->getSettingValue<bool>("debug-overlay");
        debugDraw = mod->getSettingValue<bool>("debug-draw");
    }
};

static BotSettings g_settings;

// ============================
// IDs (extend these!)
// ============================

// Hazards (spikes/saws etc) – same as before (trim/extend as needed)
static const std::set<int> HAZARD_IDS = {
    8, 9, 39, 61, 103, 392,
    243,244,245,246,247,248,249,
    363,364,365,366,367,368,
    446,447,
    678,679,680,
    1705,1706,1707,1708,1709,1710,
    1711,1712,1713,1714,1715,1716,1717,1718,
    88,89,98,397,398,399,740,741,742
};

// Very important: solids. This list is incomplete by nature.
// Extend for custom blocks used by the levels you care about.
static const std::set<int> SOLID_IDS = []{
    std::set<int> s;
    // Common “basic blocks” range in many levels (NOT complete).
    for (int id = 1; id <= 34; ++id) s.insert(id);

    // Exclude known non-solids in that range if needed (pads/orbs/portals are >34 typically)
    // Example: if something in 1..34 is actually special in your build, remove it here.

    return s;
}();

// Orbs/pads/portals (as before)
static const std::map<int, int> ORB_IDS = {
    {36, 0}, {84, 1}, {141, 2}, {1022, 3}, {1330, 4}, {1333, 5}, {1704, 6}, {1751, 7}
};
static const std::map<int, int> PAD_IDS = {
    {35, 0}, {67, 1}, {140, 2}, {1332, 3}, {452, 4}
};
static const std::map<int, BotGameMode> PORTAL_IDS = {
    {12, BotGameMode::Cube}, {13, BotGameMode::Ship}, {47, BotGameMode::Ball},
    {111, BotGameMode::UFO}, {660, BotGameMode::Wave}, {745, BotGameMode::Robot},
    {1331, BotGameMode::Spider}, {1933, BotGameMode::Swing}
};

// ============================
// Level object storage
// ============================

struct LevelObj {
    int id = 0;
    float x = 0, y = 0;
    float w = 0, h = 0;
    float rot = 0;

    bool isHazard = false;
    bool isSolid = false;
    bool isOrb = false;
    bool isPad = false;
    bool isPortal = false;

    int subType = 0;              // orb/pad type
    BotGameMode portalMode = BotGameMode::Cube;

    bool gravityPortal = false;
    bool gravityUp = false;

    bool sizePortal = false;
    bool sizeMini = false;

    bool speedPortal = false;
    float speedVal = 1.0f;
};

static std::vector<LevelObj> g_objects;
static std::vector<LevelObj*> g_hazards;
static std::vector<LevelObj*> g_solids;
static std::vector<LevelObj*> g_orbs;
static std::vector<LevelObj*> g_pads;
static std::vector<LevelObj*> g_portals;

// ============================
// Physics constants
// ============================

namespace Phys {
    constexpr float GROUND = 105.0f;
    constexpr float CEILING = 2085.0f;

    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP = 11.180032f;
    constexpr float JUMP_MINI = 9.4f;

    constexpr float SHIP_ACCEL = 0.8f;
    constexpr float SHIP_MAX = 8.0f;

    constexpr float UFO_BOOST = 7.0f;
    constexpr float BALL_VEL = 6.0f;

    constexpr float MAX_VEL = 15.0f;

    constexpr float PLAYER_SIZE = 30.0f;
    constexpr float MINI_SCALE = 0.6f;
    constexpr float WAVE_SCALE = 0.5f;

    constexpr float SPEED_SLOW = 251.16f;
    constexpr float SPEED_NORMAL = 311.58f;
    constexpr float SPEED_FAST = 387.42f;
    constexpr float SPEED_FASTER = 468.0f;
    constexpr float SPEED_FASTEST = 576.0f;
}

// ============================
// Helpers
// ============================

static bool isPlatformMode(BotGameMode m) {
    return m == BotGameMode::Cube || m == BotGameMode::Ball || m == BotGameMode::Robot || m == BotGameMode::Spider;
}

static float getPlayerSizeFor(bool mini, BotGameMode mode) {
    float sz = Phys::PLAYER_SIZE;
    if (mini) sz *= Phys::MINI_SCALE;
    if (mode == BotGameMode::Wave) sz *= Phys::WAVE_SCALE;
    return sz * g_settings.hitboxScale;
}

static bool checkAABB(float px, float py, float psz, const LevelObj& o, float extraMargin = 0.f) {
    float ph = psz / 2.f + g_settings.safeMargin + extraMargin;
    float ow = (o.w * g_settings.hazardHitboxScale) / 2.f;
    float oh = (o.h * g_settings.hazardHitboxScale) / 2.f;

    return !(px + ph < o.x - ow || px - ph > o.x + ow || py + ph < o.y - oh || py - ph > o.y + oh);
}

// ============================
// Analyze level
// ============================

static void analyzeLevel(PlayLayer* pl) {
    g_objects.clear();
    g_hazards.clear();
    g_solids.clear();
    g_orbs.clear();
    g_pads.clear();
    g_portals.clear();
    g_levelAnalyzed = false;

    if (!pl || !pl->m_objects) return;

    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;

        int id = obj->m_objectID;

        LevelObj lo;
        lo.id = id;
        lo.x = obj->getPositionX();
        lo.y = obj->getPositionY();
        lo.rot = obj->getRotation();

        // Use real sprite size as hitbox base (still not perfect, but better than nothing)
        auto sz = obj->getContentSize();
        float sc = obj->getScale();
        lo.w = std::max(5.f, sz.width * sc * 0.9f);
        lo.h = std::max(5.f, sz.height * sc * 0.9f);

        bool important = false;

        if (HAZARD_IDS.count(id)) {
            lo.isHazard = true;
            important = true;
        }
        else if (SOLID_IDS.count(id)) {
            lo.isSolid = true;
            important = true;
        }
        else if (ORB_IDS.count(id)) {
            lo.isOrb = true;
            lo.subType = ORB_IDS.at(id);
            important = true;
        }
        else if (PAD_IDS.count(id)) {
            lo.isPad = true;
            lo.subType = PAD_IDS.at(id);
            important = true;
        }
        else if (PORTAL_IDS.count(id)) {
            lo.isPortal = true;
            lo.portalMode = PORTAL_IDS.at(id);
            important = true;
        }
        else if (id == 10) { lo.isPortal = true; lo.gravityPortal = true; lo.gravityUp = false; important = true; }
        else if (id == 11) { lo.isPortal = true; lo.gravityPortal = true; lo.gravityUp = true; important = true; }
        else if (id == 99) { lo.isPortal = true; lo.sizePortal = true; lo.sizeMini = false; important = true; }
        else if (id == 101) { lo.isPortal = true; lo.sizePortal = true; lo.sizeMini = true; important = true; }
        else if (id == 200) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 0.7f; important = true; }
        else if (id == 201) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 0.9f; important = true; }
        else if (id == 202) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.0f; important = true; }
        else if (id == 203) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.1f; important = true; }
        else if (id == 1334) { lo.isPortal = true; lo.speedPortal = true; lo.speedVal = 1.3f; important = true; }

        if (important) g_objects.push_back(lo);
    }

    std::sort(g_objects.begin(), g_objects.end(), [](const LevelObj& a, const LevelObj& b) {
        return a.x < b.x;
    });

    for (auto& o : g_objects) {
        if (o.isHazard) g_hazards.push_back(&o);
        if (o.isSolid)  g_solids.push_back(&o);
        if (o.isOrb)    g_orbs.push_back(&o);
        if (o.isPad)    g_pads.push_back(&o);
        if (o.isPortal) g_portals.push_back(&o);
    }

    log::info("Bot: {} hazards, {} solids, {} orbs, {} pads, {} portals",
        g_hazards.size(), g_solids.size(), g_orbs.size(), g_pads.size(), g_portals.size());

    g_levelAnalyzed = true;
}

// ============================
// Solid collision resolution (platform modes)
// ============================
// Returns false => dead (hit side/bottom), true => ok (possibly landed on top)
static bool resolveSolidCollisionsPlatform(
    float& px, float& py, float& yVel, bool& onGround, bool upsideDown, bool mini, BotGameMode mode,
    float prevX, float prevY
) {
    float psz = getPlayerSizeFor(mini, mode);
    float pHalf = psz / 2.f;

    // Only check solids near player X
    for (const LevelObj* s : g_solids) {
        float dx = s->x - px;
        if (dx < -120.f || dx > 180.f) continue;

        if (!checkAABB(px, py, psz, *s)) continue;

        float ow = (s->w * g_settings.hazardHitboxScale) / 2.f;
        float oh = (s->h * g_settings.hazardHitboxScale) / 2.f;

        float solidLeft   = s->x - ow;
        float solidRight  = s->x + ow;
        float solidBottom = s->y - oh;
        float solidTop    = s->y + oh;

        float prevBottom = prevY - pHalf;
        float prevTop    = prevY + pHalf;
        float curBottom  = py - pHalf;
        float curTop     = py + pHalf;

        // Landing checks:
        const float eps = 2.0f;

        if (!upsideDown) {
            // From above onto top surface
            if (prevBottom >= solidTop - eps && curBottom < solidTop) {
                py = solidTop + pHalf;
                yVel = 0.f;
                onGround = true;
                continue;
            }
            // Hit underside of a block => die
            if (prevTop <= solidBottom + eps && curTop > solidBottom) {
                return false;
            }
        } else {
            // Upside-down "landing" onto underside surface
            if (prevTop <= solidBottom + eps && curTop > solidBottom) {
                py = solidBottom - pHalf;
                yVel = 0.f;
                onGround = true;
                continue;
            }
            // Hit top side (in upside down) => die
            if (prevBottom >= solidTop - eps && curBottom < solidTop) {
                return false;
            }
        }

        // Otherwise it was a side hit => die
        return false;
    }

    return true;
}

// ============================
// Hazard + solid death checks
// ============================

static bool willDieHazardsOnly(float px, float py, bool mini, BotGameMode mode) {
    float psz = getPlayerSizeFor(mini, mode);

    for (const LevelObj* h : g_hazards) {
        float dx = h->x - px;
        if (dx < -60.f || dx > g_settings.reactionDistance) continue;
        if (checkAABB(px, py, psz, *h)) return true;
    }

    // bounds (loose)
    if (py < Phys::GROUND - 60.f || py > Phys::CEILING + 60.f) return true;
    return false;
}

// ============================
// Simulation
// ============================

struct SimState {
    float x = 0, y = 0, yVel = 0, xSpeed = 0;
    float prevX = 0, prevY = 0;
    bool onGround = false;
    bool canJump = true;
    bool upsideDown = false;
    bool mini = false;
    BotGameMode mode = BotGameMode::Cube;

    float orbCD = 0;
    int lastOrb = -1;

    bool dead = false;
};

static void simFrame(SimState& s, bool hold) {
    s.prevX = s.x;
    s.prevY = s.y;

    if (s.orbCD > 0) s.orbCD -= 1.f / 240.f;

    float g = Phys::GRAVITY * (s.mini ? 0.8f : 1.0f);
    if (s.upsideDown) g = -g;

    float groundY = s.upsideDown ? Phys::CEILING : Phys::GROUND;

    switch (s.mode) {
        case BotGameMode::Cube:
        case BotGameMode::Robot:
        case BotGameMode::Spider: {
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_VEL, Phys::MAX_VEL);
            s.y += s.yVel;

            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
                s.onGround = true;
                s.canJump = true;
            } else {
                s.onGround = false;
            }

            if (hold && s.onGround && s.canJump) {
                float j = s.mini ? Phys::JUMP_MINI : Phys::JUMP;
                s.yVel = s.upsideDown ? -j : j;
                s.onGround = false;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
        } break;

        case BotGameMode::Ship: {
            float acc = s.mini ? 0.6f : Phys::SHIP_ACCEL;
            float maxV = s.mini ? 6.f : Phys::SHIP_MAX;
            s.yVel += hold ? (s.upsideDown ? -acc : acc) : (s.upsideDown ? acc : -acc);
            s.yVel = std::clamp(s.yVel, -maxV, maxV);
            s.y += s.yVel;
            s.onGround = false;
        } break;

        case BotGameMode::Ball: {
            s.yVel -= g * 0.6f;
            s.yVel = std::clamp(s.yVel, -12.f, 12.f);
            s.y += s.yVel;

            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0;
                s.onGround = true;
            } else {
                s.onGround = false;
            }

            if (hold && s.onGround && s.canJump) {
                s.upsideDown = !s.upsideDown;
                s.yVel = s.upsideDown ? -Phys::BALL_VEL : Phys::BALL_VEL;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
        } break;

        case BotGameMode::UFO: {
            s.yVel -= g * 0.5f;
            s.yVel = std::clamp(s.yVel, -8.f, 8.f);
            s.y += s.yVel;

            if (hold && s.canJump) {
                float b = s.mini ? 5.5f : Phys::UFO_BOOST;
                s.yVel = s.upsideDown ? -b : b;
                s.canJump = false;
            }
            if (!hold) s.canJump = true;
        } break;

        case BotGameMode::Wave: {
            float ws = s.xSpeed * (s.mini ? 0.7f : 1.0f);
            s.y += hold ? (s.upsideDown ? -ws : ws) : (s.upsideDown ? ws : -ws);
            s.onGround = false;
        } break;

        case BotGameMode::Swing: {
            // crude
            float sg = (hold ? -g : g) * 0.56f;
            s.yVel += sg;
            s.yVel = std::clamp(s.yVel, -8.f, 8.f);
            s.y += s.yVel;
        } break;
    }

    s.x += s.xSpeed;
    s.y = std::clamp(s.y, Phys::GROUND - 80.f, Phys::CEILING + 80.f);

    // 1) Hazards always kill
    if (willDieHazardsOnly(s.x, s.y, s.mini, s.mode)) {
        s.dead = true;
        return;
    }

    // 2) Solids:
    if (isPlatformMode(s.mode)) {
        // resolve landings; side/bottom hits kill
        bool ok = resolveSolidCollisionsPlatform(
            s.x, s.y, s.yVel, s.onGround, s.upsideDown, s.mini, s.mode,
            s.prevX, s.prevY
        );
        if (!ok) {
            s.dead = true;
            return;
        }
    } else {
        // non-platform modes die on any solid overlap
        float psz = getPlayerSizeFor(s.mini, s.mode);
        for (const LevelObj* blk : g_solids) {
            float dx = blk->x - s.x;
            if (dx < -60.f || dx > 120.f) continue;
            if (checkAABB(s.x, s.y, psz, *blk)) {
                s.dead = true;
                return;
            }
        }
    }
}

// ============================
// Decision (simple compare)
// ============================

static bool shouldBotClick() {
    SimState base;
    base.x = g_playerX;
    base.y = g_playerY;
    base.yVel = g_playerYVel;
    base.xSpeed = g_xSpeed;
    base.onGround = g_playerOnGround;
    base.canJump = true;
    base.upsideDown = g_isUpsideDown;
    base.mini = g_isMini;
    base.mode = g_gameMode;

    int frames = g_settings.simFrames;
    int holdF  = g_settings.holdDuration;

    int survNo = 0;
    {
        SimState s = base;
        for (int i = 0; i < frames; i++) {
            simFrame(s, false);
            if (s.dead) break;
            survNo++;
        }
    }

    int survYes = 0;
    {
        SimState s = base;
        for (int i = 0; i < frames; i++) {
            bool hold = (i < holdF);
            simFrame(s, hold);
            if (s.dead) break;
            survYes++;
        }
    }

    return survYes > survNo;
}

// ============================
// UI / debug drawing nodes (fixed layers)
// ============================

class BotHUD : public CCNode {
public:
    CCLabelBMFont* l1 = nullptr;
    CCLabelBMFont* l2 = nullptr;

    static BotHUD* create() {
        auto* r = new BotHUD();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        l1 = CCLabelBMFont::create("Bot: OFF", "bigFont.fnt");
        l1->setScale(0.4f);
        l1->setAnchorPoint(ccp(0, 1));
        l1->setPosition(ccp(10, CCDirector::sharedDirector()->getWinSize().height - 10));
        addChild(l1);

        l2 = CCLabelBMFont::create("", "chatFont.fnt");
        l2->setScale(0.5f);
        l2->setAnchorPoint(ccp(0, 1));
        l2->setPosition(ccp(10, CCDirector::sharedDirector()->getWinSize().height - 35));
        addChild(l2);

        scheduleUpdate();
        return true;
    }

    void update(float) override {
        g_settings.load();

        this->setVisible(g_settings.debugOverlay);

        l1->setString(g_botEnabled ? "Bot: ON" : "Bot: OFF");
        l1->setColor(g_botEnabled ? ccc3(0,255,0) : ccc3(255,100,100));

        const char* modes[] = {"Cube","Ship","Ball","UFO","Wave","Robot","Spider","Swing"};
        char buf[256];
        snprintf(buf, sizeof(buf), "X:%.0f Y:%.0f Vel:%.2f %s",
            g_playerX, g_playerY, g_playerYVel, modes[(int)g_gameMode]);
        l2->setString(buf);
    }
};

class BotWorldDebug : public CCNode {
public:
    CCDrawNode* dn = nullptr;

    static BotWorldDebug* create() {
        auto* r = new BotWorldDebug();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        dn = CCDrawNode::create();
        addChild(dn);
        scheduleUpdate();
        return true;
    }

    void update(float) override {
        g_settings.load();
        dn->clear();

        if (!g_settings.debugDraw || !g_levelAnalyzed) return;

        // draw player box (world space)
        float psz = getPlayerSizeFor(g_isMini, g_gameMode);
        dn->drawRect(
            ccp(g_playerX - psz/2, g_playerY - psz/2),
            ccp(g_playerX + psz/2, g_playerY + psz/2),
            ccc4f(1,1,1,0.25f), 1.f, ccc4f(1,1,1,0.8f)
        );

        // draw hazards/solids near player
        float minX = g_playerX - 200.f;
        float maxX = g_playerX + 600.f;

        for (auto* h : g_hazards) {
            if (h->x < minX || h->x > maxX) continue;
            float hw = (h->w * g_settings.hazardHitboxScale) / 2.f;
            float hh = (h->h * g_settings.hazardHitboxScale) / 2.f;
            dn->drawRect(ccp(h->x-hw, h->y-hh), ccp(h->x+hw, h->y+hh),
                ccc4f(1,0,0,0.15f), 1.f, ccc4f(1,0,0,0.6f));
        }
        for (auto* s : g_solids) {
            if (s->x < minX || s->x > maxX) continue;
            float hw = (s->w * g_settings.hazardHitboxScale) / 2.f;
            float hh = (s->h * g_settings.hazardHitboxScale) / 2.f;
            dn->drawRect(ccp(s->x-hw, s->y-hh), ccp(s->x+hw, s->y+hh),
                ccc4f(0.2f,0.6f,1.f,0.10f), 1.f, ccc4f(0.2f,0.6f,1.f,0.5f));
        }
    }
};

static BotHUD* g_hud = nullptr;
static BotWorldDebug* g_worldDebug = nullptr;

// ============================
// Hooks
// ============================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_settings.load();

        // HUD should be in UI layer (screen space)
        if (!g_hud) g_hud = BotHUD::create();
        if (g_hud->getParent()) g_hud->removeFromParentAndCleanup(false);

        if (this->m_uiLayer) {
            this->m_uiLayer->addChild(g_hud, 9999);
        } else {
            // fallback: scene (still screen-space)
            CCDirector::sharedDirector()->getRunningScene()->addChild(g_hud, 9999);
        }

        // Debug draw should be in world/object layer
        if (!g_worldDebug) g_worldDebug = BotWorldDebug::create();
        if (g_worldDebug->getParent()) g_worldDebug->removeFromParentAndCleanup(false);

        if (this->m_objectLayer) {
            this->m_objectLayer->addChild(g_worldDebug, 9998);
        } else {
            this->addChild(g_worldDebug, 9998);
        }

        g_levelAnalyzed = false;
        g_isHolding = false;
        g_frameCounter = 0;

        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        analyzeLevel(this);
    }

    void onQuit() {
        // detach nodes so they don't persist into other scenes
        if (g_hud && g_hud->getParent()) g_hud->removeFromParentAndCleanup(false);
        if (g_worldDebug && g_worldDebug->getParent()) g_worldDebug->removeFromParentAndCleanup(false);
        PlayLayer::onQuit();
    }
};

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);

        g_frameCounter++;

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) return;
        if (!g_levelAnalyzed) return;
        if (!g_botEnabled) return;
        if (pl->m_isPaused || pl->m_hasCompletedLevel || m_player1->m_isDead) return;

        // read player state
        g_playerX = m_player1->getPositionX();
        g_playerY = m_player1->getPositionY();
        g_playerYVel = m_player1->m_yVelocity;
        g_playerOnGround = m_player1->m_isOnGround;
        g_isUpsideDown = m_player1->m_isUpsideDown;
        g_isMini = (m_player1->m_vehicleSize != 1.0f);

        if (m_player1->m_isShip) g_gameMode = BotGameMode::Ship;
        else if (m_player1->m_isBall) g_gameMode = BotGameMode::Ball;
        else if (m_player1->m_isBird) g_gameMode = BotGameMode::UFO;
        else if (m_player1->m_isDart) g_gameMode = BotGameMode::Wave;
        else if (m_player1->m_isRobot) g_gameMode = BotGameMode::Robot;
        else if (m_player1->m_isSpider) g_gameMode = BotGameMode::Spider;
        else if (m_player1->m_isSwing) g_gameMode = BotGameMode::Swing;
        else g_gameMode = BotGameMode::Cube;

        // speed
        float spd = m_player1->m_playerSpeed;
        if (spd <= 0.8f) g_xSpeed = Phys::SPEED_SLOW / 240.f;
        else if (spd <= 0.95f) g_xSpeed = Phys::SPEED_NORMAL / 240.f;
        else if (spd <= 1.05f) g_xSpeed = Phys::SPEED_FAST / 240.f;
        else if (spd <= 1.15f) g_xSpeed = Phys::SPEED_FASTER / 240.f;
        else g_xSpeed = Phys::SPEED_FASTEST / 240.f;

        // settings occasionally
        if (g_frameCounter % 120 == 0) g_settings.load();

        bool click = shouldBotClick();
        if (click != g_isHolding) {
            this->handleButton(click, 1, true);
            g_isHolding = click;
        }
    }
};

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto ws = CCDirector::sharedDirector()->getWinSize();

        auto* menu = CCMenu::create();
        menu->setPosition(ccp(0, 0));
        addChild(menu, 100);

        // Toggle
        auto* onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* togg = CCMenuItemToggler::create(offSpr, onSpr, this, menu_selector(BotPauseLayer::onBotToggle));
        togg->setPosition(ccp(ws.width - 30.f, ws.height - 30.f));
        togg->toggle(g_botEnabled);
        menu->addChild(togg);

        // Settings button
        auto* gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        auto* btn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(BotPauseLayer::onOpenSettings));
        btn->setPosition(ccp(ws.width - 30.f, ws.height - 75.f));
        btn->setScale(0.7f);
        menu->addChild(btn);
    }

    void onBotToggle(CCObject*) {
        g_botEnabled = !g_botEnabled;

        if (!g_botEnabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) {
                gj->handleButton(false, 1, true);
            }
            g_isHolding = false;
        }
    }

    void onOpenSettings(CCObject*) {
        #if GDBOT_HAS_MOD_SETTINGS_POPUP
            ModSettingsPopup::create(Mod::get())->show();
        #else
            FLAlertLayer::create(
                "GD AutoBot",
                "ModSettingsPopup header not found in this Geode SDK build.\n"
                "Open the mod settings from the Geode Mods list instead.",
                "OK"
            )->show();
        #endif
    }
};

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && key == KEY_F8) {
            g_botEnabled = !g_botEnabled;
            if (!g_botEnabled && g_isHolding) {
                if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
                g_isHolding = false;
            }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

$on_mod(Loaded) {
    g_settings.load();
    log::info("GD AutoBot loaded (F8 toggle).");
}

