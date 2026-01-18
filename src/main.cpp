#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <deque>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using namespace geode::prelude;

// ============================================================
// Globals
// ============================================================

static bool  g_enabled = false;
static bool  g_levelAnalyzed = false;
static bool  g_isHolding = false;
static bool  g_prevDead = false;
static int   g_frameCounter = 0;

static float g_playerX = 0.f;
static float g_playerY = 0.f;
static float g_playerYVel = 0.f;
static bool  g_onGround = false;
static bool  g_upsideDown = false;
static bool  g_mini = false;
static float g_xSpeed = 311.58f / 240.f;

enum class Mode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };
static Mode g_mode = Mode::Cube;

// ============================================================
// Settings cache
// ============================================================

struct Settings {
    bool enabled = false;

    int beamWidth = 64;
    int horizonFrames = 240;
    int stepFrames = 3;
    int replanEvery = 6;
    int budgetMs = 4;
    int tapFrames = 2;

    float playerHitboxScale = 0.9f;
    float objHitboxScale = 1.0f;
    float safeMargin = 2.0f;

    bool debugHud = true;
    bool debugDraw = false;

    bool learn = true;
    float learnBin = 60.f;
    float learnStrength = 1.0f;

    void load() {
        auto* m = Mod::get();
        enabled = m->getSettingValue<bool>("enabled");

        beamWidth = (int)m->getSettingValue<int64_t>("beam-width");
        horizonFrames = (int)m->getSettingValue<int64_t>("horizon-frames");
        stepFrames = (int)m->getSettingValue<int64_t>("step-frames");
        replanEvery = (int)m->getSettingValue<int64_t>("replan-every");
        budgetMs = (int)m->getSettingValue<int64_t>("time-budget-ms");
        tapFrames = (int)m->getSettingValue<int64_t>("tap-frames");

        playerHitboxScale = (float)m->getSettingValue<double>("player-hitbox-scale");
        objHitboxScale = (float)m->getSettingValue<double>("obj-hitbox-scale");
        safeMargin = (float)m->getSettingValue<double>("safe-margin");

        debugHud = m->getSettingValue<bool>("debug-hud");
        debugDraw = m->getSettingValue<bool>("debug-draw");

        learn = m->getSettingValue<bool>("learn");
        learnBin = (float)m->getSettingValue<double>("learn-bin");
        learnStrength = (float)m->getSettingValue<double>("learn-strength");

        beamWidth = std::clamp(beamWidth, 8, 256);
        horizonFrames = std::clamp(horizonFrames, 30, 720);
        stepFrames = std::clamp(stepFrames, 1, 10);
        replanEvery = std::clamp(replanEvery, 1, 30);
        budgetMs = std::clamp(budgetMs, 1, 12);
        tapFrames = std::clamp(tapFrames, 1, 10);

        playerHitboxScale = std::clamp(playerHitboxScale, 0.5f, 1.3f);
        objHitboxScale = std::clamp(objHitboxScale, 0.7f, 1.6f);
        safeMargin = std::clamp(safeMargin, 0.f, 12.f);

        learnBin = std::clamp(learnBin, 10.f, 300.f);
        learnStrength = std::clamp(learnStrength, 0.f, 5.f);
    }
};

static Settings g_set;

// ============================================================
// IDs (NOTE: solids list is incomplete; extend as needed)
// ============================================================

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

// Conservative “likely solid” IDs.
// If the bot still runs into blocks, you must extend this set.
static const std::set<int> SOLID_IDS = []{
    std::set<int> s;
    for (int id = 1; id <= 34; ++id) s.insert(id);
    for (int id = 40; id <= 48; ++id) s.insert(id);
    return s;
}();

static const std::map<int, int> ORB_IDS = {
    {36, 0}, {84, 1}, {141, 2}, {1022, 3}, {1330, 4}, {1333, 5}, {1704, 6}, {1751, 7}
};
static const std::map<int, int> PAD_IDS = {
    {35, 0}, {67, 1}, {140, 2}, {1332, 3}, {452, 4}
};

// ============================================================
// Physics constants (approx)
// ============================================================

namespace Phys {
    constexpr float GROUND = 105.f;
    constexpr float CEILING = 2085.f;

    constexpr float GRAV = 0.958199f;
    constexpr float JUMP = 11.180032f;
    constexpr float JUMP_MINI = 9.4f;

    constexpr float SHIP_ACCEL = 0.8f;
    constexpr float SHIP_MAX = 8.f;

    constexpr float BALL_VEL = 6.f;

    constexpr float UFO_BOOST = 7.f;

    constexpr float MAX_VEL = 15.f;

    constexpr float PLAYER_SIZE = 30.f;
    constexpr float MINI_SCALE = 0.6f;
    constexpr float WAVE_SCALE = 0.5f;

    constexpr float SPEED_SLOW = 251.16f;
    constexpr float SPEED_NORMAL = 311.58f;
    constexpr float SPEED_FAST = 387.42f;
    constexpr float SPEED_FASTER = 468.0f;
    constexpr float SPEED_FASTEST = 576.0f;
}

// ============================================================
// Level objects
// ============================================================

struct Obj {
    int id = 0;
    float x = 0, y = 0;
    float w = 0, h = 0;

    bool hazard = false;
    bool solid = false;
    bool orb = false;
    bool pad = false;

    int sub = 0;
};

static std::vector<Obj> g_objs;
static std::vector<const Obj*> g_haz;
static std::vector<const Obj*> g_sol;
static std::vector<const Obj*> g_orb;
static std::vector<const Obj*> g_pad;

// ============================================================
// Learning (session)
// penalize bins for actions that lead to death
// ============================================================

static std::unordered_map<int, float> g_penaltyHold;
static std::unordered_map<int, float> g_penaltyRelease;

struct RecentDecision {
    int bin = 0;
    bool hold = false;
};
static std::deque<RecentDecision> g_recent;
static const size_t MAX_RECENT = 200;

static int binForX(float x) {
    return (int)std::floor(x / std::max(1.f, g_set.learnBin));
}

// ============================================================
// Helpers
// ============================================================

static inline bool platformMode(Mode m) {
    return m == Mode::Cube || m == Mode::Ball || m == Mode::Robot || m == Mode::Spider;
}

static float playerSize(bool mini, Mode mode) {
    float sz = Phys::PLAYER_SIZE;
    if (mini) sz *= Phys::MINI_SCALE;
    if (mode == Mode::Wave) sz *= Phys::WAVE_SCALE;
    return sz * g_set.playerHitboxScale;
}

static bool aabb(float px, float py, float psz, const Obj& o, float extra = 0.f) {
    float ph = psz / 2.f + g_set.safeMargin + extra;
    float ow = (o.w * g_set.objHitboxScale) / 2.f;
    float oh = (o.h * g_set.objHitboxScale) / 2.f;

    return !(px + ph < o.x - ow || px - ph > o.x + ow || py + ph < o.y - oh || py - ph > o.y + oh);
}

// ============================================================
// Analyze level (no pointers stored to temporary memory)
// ============================================================

static void analyzeLevel(PlayLayer* pl) {
    g_levelAnalyzed = false;
    g_objs.clear();
    g_haz.clear();
    g_sol.clear();
    g_orb.clear();
    g_pad.clear();

    if (!pl || !pl->m_objects) return;

    int hazards = 0, solids = 0, orbs = 0, pads = 0;

    g_objs.reserve((size_t)pl->m_objects->count() / 2);

    for (int i = 0; i < pl->m_objects->count(); i++) {
        auto* go = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!go) continue;

        int id = go->m_objectID;

        Obj o;
        o.id = id;
        o.x = go->getPositionX();
        o.y = go->getPositionY();

        auto sz = go->getContentSize();
        float sc = go->getScale();
        o.w = std::max(5.f, sz.width * sc * 0.9f);
        o.h = std::max(5.f, sz.height * sc * 0.9f);

        bool important = false;

        if (HAZARD_IDS.count(id)) {
            o.hazard = true;
            hazards++;
            important = true;
        } else if (SOLID_IDS.count(id)) {
            o.solid = true;
            solids++;
            important = true;
        } else if (auto it = ORB_IDS.find(id); it != ORB_IDS.end()) {
            o.orb = true;
            o.sub = it->second;
            orbs++;
            important = true;
        } else if (auto it = PAD_IDS.find(id); it != PAD_IDS.end()) {
            o.pad = true;
            o.sub = it->second;
            pads++;
            important = true;
        }

        if (important) g_objs.push_back(o);
    }

    std::sort(g_objs.begin(), g_objs.end(), [](const Obj& a, const Obj& b) { return a.x < b.x; });

    for (auto& o : g_objs) {
        if (o.hazard) g_haz.push_back(&o);
        if (o.solid)  g_sol.push_back(&o);
        if (o.orb)    g_orb.push_back(&o);
        if (o.pad)    g_pad.push_back(&o);
    }

    log::info("Bot: {} hazards, {} solids, {} orbs, {} pads", hazards, solids, orbs, pads);

    g_levelAnalyzed = true;
}

// ============================================================
// Collision / death checks
// ============================================================

static bool dieHazards(float x, float y, bool mini, Mode mode) {
    float psz = playerSize(mini, mode);

    // only check near window
    float minX = x - 120.f;
    float maxX = x + 500.f;

    for (auto* h : g_haz) {
        if (h->x < minX) continue;
        if (h->x > maxX) break;
        if (aabb(x, y, psz, *h)) return true;
    }

    // bounds (loose)
    if (y < Phys::GROUND - 100.f || y > Phys::CEILING + 100.f) return true;
    return false;
}

// platform resolution: land on top, die on side/bottom
static bool resolveSolidsPlatform(float& x, float& y, float& yVel, bool& onGround,
                                 bool upsideDown, bool mini, Mode mode,
                                 float prevX, float prevY) {
    float psz = playerSize(mini, mode);
    float pHalf = psz / 2.f;

    float minX = x - 150.f;
    float maxX = x + 250.f;

    for (auto* s : g_sol) {
        if (s->x < minX) continue;
        if (s->x > maxX) break;
        if (!aabb(x, y, psz, *s)) continue;

        float ow = (s->w * g_set.objHitboxScale) / 2.f;
        float oh = (s->h * g_set.objHitboxScale) / 2.f;

        float solidTop = s->y + oh;
        float solidBottom = s->y - oh;

        float prevBottom = prevY - pHalf;
        float prevTop = prevY + pHalf;
        float curBottom = y - pHalf;
        float curTop = y + pHalf;

        const float eps = 2.0f;

        if (!upsideDown) {
            // land on top
            if (prevBottom >= solidTop - eps && curBottom < solidTop) {
                y = solidTop + pHalf;
                yVel = 0.f;
                onGround = true;
                continue;
            }
            // hit underside -> die
            if (prevTop <= solidBottom + eps && curTop > solidBottom) {
                return false;
            }
        } else {
            // upside-down land on underside
            if (prevTop <= solidBottom + eps && curTop > solidBottom) {
                y = solidBottom - pHalf;
                yVel = 0.f;
                onGround = true;
                continue;
            }
            // hit "top" in upside-down -> die
            if (prevBottom >= solidTop - eps && curBottom < solidTop) {
                return false;
            }
        }

        // side -> die
        return false;
    }

    return true;
}

static bool dieSolidsNonPlatform(float x, float y, bool mini, Mode mode) {
    float psz = playerSize(mini, mode);

    float minX = x - 150.f;
    float maxX = x + 250.f;

    for (auto* s : g_sol) {
        if (s->x < minX) continue;
        if (s->x > maxX) break;
        if (aabb(x, y, psz, *s)) return true;
    }
    return false;
}

// ============================================================
// Simulation (trajectory engine)
// ============================================================

struct Sim {
    float x = 0, y = 0;
    float yVel = 0;
    float xSpeed = 0;
    float prevX = 0, prevY = 0;

    bool onGround = false;
    bool canJump = true;
    bool upsideDown = false;
    bool mini = false;
    Mode mode = Mode::Cube;

    float orbCD = 0.f;
    int lastOrb = -1;

    bool dead = false;
};

static Obj const* findOrbAt(float x, float y, bool mini, Mode mode) {
    float psz = playerSize(mini, mode) * 1.5f;
    float minX = x - 100.f;
    float maxX = x + 100.f;
    for (auto* o : g_orb) {
        if (o->x < minX) continue;
        if (o->x > maxX) break;
        if (aabb(x, y, psz, *o)) return o;
    }
    return nullptr;
}

static Obj const* findPadAt(float x, float y, bool mini, Mode mode) {
    float psz = playerSize(mini, mode);
    float minX = x - 100.f;
    float maxX = x + 100.f;
    for (auto* p : g_pad) {
        if (p->x < minX) continue;
        if (p->x > maxX) break;
        if (aabb(x, y, psz, *p)) return p;
    }
    return nullptr;
}

static void simInteract(Sim& s, bool hold) {
    // pads (auto)
    if (auto* p = findPadAt(s.x, s.y, s.mini, s.mode)) {
        float b = 0.f;
        switch (p->sub) {
            case 0: b = 12.f; break;
            case 1: b = 16.f; break;
            case 2: b = 20.f; break;
            case 3: b = -12.f; break;
            case 4: s.upsideDown = !s.upsideDown; b = 0.f; break;
        }
        if (b != 0.f) {
            s.yVel = s.upsideDown ? -b : b;
            s.onGround = false;
        }
    }

    // orbs (only when holding)
    if (hold && s.orbCD <= 0.f) {
        if (auto* o = findOrbAt(s.x, s.y, s.mini, s.mode)) {
            if (o->id != s.lastOrb) {
                float b = 0.f;
                bool flip = false;
                switch (o->sub) {
                    case 0: b = 11.2f; break;
                    case 1: b = 14.f; break;
                    case 2: b = 18.f; break;
                    case 3: flip = true; b = 8.f; break;
                    case 4: flip = true; b = 11.2f; break;
                    case 6:
                    case 7: b = 15.f; break;
                    default: break;
                }
                if (flip) s.upsideDown = !s.upsideDown;
                if (b != 0.f) {
                    s.yVel = s.upsideDown ? -b : b;
                    s.onGround = false;
                }
                s.orbCD = 0.1f;
                s.lastOrb = o->id;
            }
        }
    }
}

static void simFrame(Sim& s, bool hold) {
    if (s.dead) return;

    s.prevX = s.x;
    s.prevY = s.y;

    if (s.orbCD > 0.f) s.orbCD -= 1.f / 240.f;

    float g = Phys::GRAV * (s.mini ? 0.8f : 1.0f);
    if (s.upsideDown) g = -g;

    float groundY = s.upsideDown ? Phys::CEILING : Phys::GROUND;

    switch (s.mode) {
        case Mode::Cube:
        case Mode::Robot:
        case Mode::Spider: {
            s.yVel -= g;
            s.yVel = std::clamp(s.yVel, -Phys::MAX_VEL, Phys::MAX_VEL);
            s.y += s.yVel;

            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0.f;
                s.onGround = true;
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

        case Mode::Ship: {
            float acc = s.mini ? 0.6f : Phys::SHIP_ACCEL;
            float maxV = s.mini ? 6.f : Phys::SHIP_MAX;
            s.yVel += hold ? (s.upsideDown ? -acc : acc) : (s.upsideDown ? acc : -acc);
            s.yVel = std::clamp(s.yVel, -maxV, maxV);
            s.y += s.yVel;
            s.onGround = false;
        } break;

        case Mode::Ball: {
            s.yVel -= g * 0.6f;
            s.yVel = std::clamp(s.yVel, -12.f, 12.f);
            s.y += s.yVel;

            bool hit = s.upsideDown ? (s.y >= groundY) : (s.y <= groundY);
            if (hit) {
                s.y = groundY;
                s.yVel = 0.f;
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

        case Mode::UFO: {
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

        case Mode::Wave: {
            float ws = s.xSpeed * (s.mini ? 0.7f : 1.0f);
            s.y += hold ? (s.upsideDown ? -ws : ws) : (s.upsideDown ? ws : -ws);
            s.onGround = false;
        } break;

        case Mode::Swing: {
            float sg = (hold ? -g : g) * 0.56f;
            s.yVel += sg;
            s.yVel = std::clamp(s.yVel, -8.f, 8.f);
            s.y += s.yVel;
            s.onGround = false;
        } break;
    }

    s.x += s.xSpeed;

    // interactions
    simInteract(s, hold);

    // hazards kill
    if (dieHazards(s.x, s.y, s.mini, s.mode)) {
        s.dead = true;
        return;
    }

    // solids
    if (platformMode(s.mode)) {
        if (!resolveSolidsPlatform(s.x, s.y, s.yVel, s.onGround, s.upsideDown, s.mini, s.mode, s.prevX, s.prevY)) {
            s.dead = true;
            return;
        }
    } else {
        if (dieSolidsNonPlatform(s.x, s.y, s.mini, s.mode)) {
            s.dead = true;
            return;
        }
    }
}

// ============================================================
// Trajectory-style debug draw (two paths: hold vs release)
// ============================================================

class WorldDebug : public CCNode {
public:
    CCDrawNode* dn = nullptr;

    static WorldDebug* create() {
        auto* r = new WorldDebug();
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
        g_set.load();
        dn->clear();
        if (!g_set.debugDraw || !g_levelAnalyzed) return;

        // draw hazards/solids around player
        float psz = playerSize(g_mini, g_mode);
        dn->drawRect(ccp(g_playerX - psz/2, g_playerY - psz/2), ccp(g_playerX + psz/2, g_playerY + psz/2),
            ccc4f(1,1,1,0.2f), 1.f, ccc4f(1,1,1,0.7f));

        float minX = g_playerX - 200.f;
        float maxX = g_playerX + 800.f;

        auto drawBox = [&](const Obj& o, ccColor4F fill, ccColor4F stroke) {
            float ow = (o.w * g_set.objHitboxScale) / 2.f;
            float oh = (o.h * g_set.objHitboxScale) / 2.f;
            dn->drawRect(ccp(o.x-ow, o.y-oh), ccp(o.x+ow, o.y+oh), fill, 1.f, stroke);
        };

        for (auto* h : g_haz) { if (h->x < minX) continue; if (h->x > maxX) break; drawBox(*h, ccc4f(1,0,0,0.12f), ccc4f(1,0,0,0.5f)); }
        for (auto* s : g_sol) { if (s->x < minX) continue; if (s->x > maxX) break; drawBox(*s, ccc4f(0.2f,0.6f,1,0.10f), ccc4f(0.2f,0.6f,1,0.35f)); }

        // trajectory: NO HOLD (red)
        Sim a;
        a.x = g_playerX; a.y = g_playerY; a.yVel = g_playerYVel; a.xSpeed = g_xSpeed;
        a.onGround = g_onGround; a.canJump = !g_isHolding;
        a.upsideDown = g_upsideDown; a.mini = g_mini; a.mode = g_mode;

        CCPoint prev = ccp(a.x, a.y);
        for (int i = 0; i < 120; i++) {
            simFrame(a, false);
            CCPoint cur = ccp(a.x, a.y);
            float alpha = 0.8f * (1.f - (float)i / 120.f);
            dn->drawSegment(prev, cur, 1.5f, ccc4f(1,0.3f,0.3f,alpha));
            prev = cur;
            if (a.dead) break;
        }

        // trajectory: HOLD (green)
        Sim b = a;
        b.dead = false;
        prev = ccp(b.x, b.y);
        for (int i = 0; i < 120; i++) {
            simFrame(b, true);
            CCPoint cur = ccp(b.x, b.y);
            float alpha = 0.8f * (1.f - (float)i / 120.f);
            dn->drawSegment(prev, cur, 1.5f, ccc4f(0.3f,1,0.3f,alpha));
            prev = cur;
            if (b.dead) break;
        }
    }
};

class HUD : public CCNode {
public:
    CCLabelBMFont* l1 = nullptr;
    CCLabelBMFont* l2 = nullptr;

    static HUD* create() {
        auto* r = new HUD();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        auto ws = CCDirector::sharedDirector()->getWinSize();

        l1 = CCLabelBMFont::create("Bot: OFF", "bigFont.fnt");
        l1->setScale(0.4f);
        l1->setAnchorPoint(ccp(0,1));
        l1->setPosition(ccp(10, ws.height - 10));
        addChild(l1);

        l2 = CCLabelBMFont::create("", "chatFont.fnt");
        l2->setScale(0.55f);
        l2->setAnchorPoint(ccp(0,1));
        l2->setPosition(ccp(10, ws.height - 35));
        addChild(l2);

        scheduleUpdate();
        return true;
    }

    void update(float) override {
        g_set.load();
        setVisible(g_set.debugHud);

        l1->setString(g_enabled ? "Bot: ON" : "Bot: OFF");
        l1->setColor(g_enabled ? ccc3(0,255,0) : ccc3(255,100,100));

        const char* modes[] = {"Cube","Ship","Ball","UFO","Wave","Robot","Spider","Swing"};
        char buf[256];
        snprintf(buf, sizeof(buf), "X:%.0f Y:%.0f vY:%.2f %s", g_playerX, g_playerY, g_playerYVel, modes[(int)g_mode]);
        l2->setString(buf);
    }
};

// per-level pointers stored in PlayLayer Fields (prevents crashes)
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        HUD* hud = nullptr;
        WorldDebug* dbg = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_set.load();
        g_enabled = g_set.enabled;
        g_prevDead = false;
        g_isHolding = false;
        g_frameCounter = 0;

        // reset learning per level (session learning)
        g_penaltyHold.clear();
        g_penaltyRelease.clear();
        g_recent.clear();

        // HUD in screen space: attach to scene (no moving)
        m_fields->hud = HUD::create();
        CCDirector::sharedDirector()->getRunningScene()->addChild(m_fields->hud, 999999);

        // Debug draw in world space: attach to PlayLayer (moves with level)
        m_fields->dbg = WorldDebug::create();
        this->addChild(m_fields->dbg, 999998);

        g_levelAnalyzed = false;
        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        analyzeLevel(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_prevDead = false;
        g_isHolding = false;
        g_recent.clear();

        if (!g_levelAnalyzed) analyzeLevel(this);
    }

    void onQuit() {
        // prevent crashes: remove HUD manually (it lives on scene)
        if (m_fields->hud && m_fields->hud->getParent()) {
            m_fields->hud->removeFromParentAndCleanup(true);
        }
        m_fields->hud = nullptr;

        // dbg is child of playlayer and will be destroyed automatically; still null it
        m_fields->dbg = nullptr;

        PlayLayer::onQuit();
    }
};

// ============================================================
// Planner (beam search) with learning penalties
// ============================================================

enum class Action : uint8_t { Hold, Release, Tap };

static bool holdFor(Action a, int withinStep) {
    if (a == Action::Hold) return true;
    if (a == Action::Release) return false;
    // Tap: hold only for first tapFrames frames of the step
    return withinStep < g_set.tapFrames;
}

struct Node {
    Sim s;
    float score = -1e30f;
    int parent = -1;
    Action act = Action::Release;
};

class Planner {
public:
    void reset() {
        plan.clear();
        framesSincePlan = 0;
        stepFrame = 0;
    }

    bool getNextHold(const Sim& start, bool currentHold, bool& outHold) {
        if (plan.empty() || (++framesSincePlan >= g_set.replanEvery)) {
            framesSincePlan = 0;
            buildPlan(start);
        }

        if (plan.empty()) {
            outHold = false;
            return false;
        }

        Action a = plan.front();
        outHold = holdFor(a, stepFrame);

        stepFrame++;
        if (stepFrame >= g_set.stepFrames) {
            stepFrame = 0;
            plan.pop_front();
        }

        return true;
    }

private:
    std::deque<Action> plan;
    int framesSincePlan = 0;
    int stepFrame = 0;

    float actionPenalty(float x, Action a) {
        if (!g_set.learn) return 0.f;
        int bin = binForX(x);
        if (a == Action::Hold || a == Action::Tap) {
            auto it = g_penaltyHold.find(bin);
            return it == g_penaltyHold.end() ? 0.f : it->second;
        } else {
            auto it = g_penaltyRelease.find(bin);
            return it == g_penaltyRelease.end() ? 0.f : it->second;
        }
    }

    float scoreState(const Sim& s, Action lastAct) {
        // Progress dominates
        float progress = s.x * 10.f;

        // Safety: distance to nearest danger (rough)
        float psz = playerSize(s.mini, s.mode);
        float bestD = 9999.f;

        float minX = s.x - 120.f;
        float maxX = s.x + 600.f;

        auto checkVec = [&](const std::vector<const Obj*>& v) {
            for (auto* o : v) {
                if (o->x < minX) continue;
                if (o->x > maxX) break;

                float ow = (o->w * g_set.objHitboxScale) / 2.f;
                float oh = (o->h * g_set.objHitboxScale) / 2.f;
                float ph = psz / 2.f;

                float dx = std::max(0.f, std::abs(s.x - o->x) - (ph + ow));
                float dy = std::max(0.f, std::abs(s.y - o->y) - (ph + oh));
                float d = std::sqrt(dx*dx + dy*dy);
                bestD = std::min(bestD, d);
            }
        };

        checkVec(g_haz);
        checkVec(g_sol);

        float safe01 = std::clamp(bestD / 60.f, 0.f, 1.f);
        float safety = safe01 * 800.f;

        // y regularization
        float yPenalty = std::abs(s.y - 300.f) * 0.2f;

        // learning penalty
        float lp = actionPenalty(s.x, lastAct) * 500.f;

        return progress + safety - yPenalty - lp;
    }

    void buildPlan(const Sim& start) {
        plan.clear();
        stepFrame = 0;

        auto t0 = std::chrono::high_resolution_clock::now();

        int steps = std::max(1, g_set.horizonFrames / std::max(1, g_set.stepFrames));
        int B = g_set.beamWidth;

        std::vector<std::vector<Node>> layers;
        layers.reserve((size_t)steps + 1);
        layers.push_back({});
        layers.back().reserve((size_t)B);
        layers.back().push_back(Node{ start, scoreState(start, Action::Release), -1, Action::Release });

        const std::vector<Action> actions = { Action::Release, Action::Hold, Action::Tap };

        for (int step = 0; step < steps; ++step) {
            auto& prev = layers.back();
            std::vector<Node> next;
            next.reserve((size_t)prev.size() * actions.size());

            for (int i = 0; i < (int)prev.size(); ++i) {
                for (auto a : actions) {
                    Node n;
                    n.s = prev[i].s;
                    n.parent = i;
                    n.act = a;

                    for (int k = 0; k < g_set.stepFrames; ++k) {
                        bool hold = holdFor(a, k);
                        simFrame(n.s, hold);
                        if (n.s.dead) break;
                    }
                    if (n.s.dead) continue;

                    n.score = scoreState(n.s, a);
                    next.push_back(std::move(n));
                }
            }

            if (next.empty()) break;

            // keep top B
            if ((int)next.size() > B) {
                std::nth_element(next.begin(), next.begin() + (B - 1), next.end(),
                    [](const Node& a, const Node& b) { return a.score > b.score; });
                next.resize((size_t)B);
            }
            std::sort(next.begin(), next.end(), [](const Node& a, const Node& b) { return a.score > b.score; });

            layers.push_back(std::move(next));

            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            if (ms >= g_set.budgetMs) break;
        }

        // find best final node
        int bestLayer = (int)layers.size() - 1;
        while (bestLayer > 0 && layers[bestLayer].empty()) bestLayer--;

        if (bestLayer <= 0 || layers[bestLayer].empty()) return;

        int bestIdx = 0; // layers are sorted
        // backtrack
        std::vector<Action> reversed;
        int layer = bestLayer;
        int idx = bestIdx;

        while (layer > 0) {
            const Node& n = layers[layer][idx];
            reversed.push_back(n.act);
            idx = n.parent;
            layer--;
            if (idx < 0) break;
        }

        std::reverse(reversed.begin(), reversed.end());
        for (auto a : reversed) plan.push_back(a);
    }
};

static Planner g_planner;

// ============================================================
// GJBaseGameLayer update: read state, detect death, learn, apply planner
// ============================================================

class $modify(BotGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);

        g_frameCounter++;
        g_set.load();
        g_enabled = g_set.enabled;

        auto* pl = PlayLayer::get();
        if (!pl || !m_player1) return;
        if (!g_levelAnalyzed) return;

        bool deadNow = m_player1->m_isDead;
        if (!g_prevDead && deadNow) {
            // learning: penalize last few decisions
            if (g_set.learn) {
                int count = 0;
                while (!g_recent.empty() && count < 60) {
                    auto d = g_recent.back();
                    g_recent.pop_back();
                    float add = 0.25f * g_set.learnStrength; // small incremental
                    if (d.hold) g_penaltyHold[d.bin] += add;
                    else g_penaltyRelease[d.bin] += add;
                    count++;
                }
            }
            g_planner.reset();
        }
        g_prevDead = deadNow;

        if (!g_enabled) return;
        if (pl->m_isPaused || pl->m_hasCompletedLevel || deadNow) return;

        // read state
        g_playerX = m_player1->getPositionX();
        g_playerY = m_player1->getPositionY();
        g_playerYVel = m_player1->m_yVelocity;
        g_onGround = m_player1->m_isOnGround;
        g_upsideDown = m_player1->m_isUpsideDown;
        g_mini = (m_player1->m_vehicleSize != 1.0f);

        if (m_player1->m_isShip) g_mode = Mode::Ship;
        else if (m_player1->m_isBall) g_mode = Mode::Ball;
        else if (m_player1->m_isBird) g_mode = Mode::UFO;
        else if (m_player1->m_isDart) g_mode = Mode::Wave;
        else if (m_player1->m_isRobot) g_mode = Mode::Robot;
        else if (m_player1->m_isSpider) g_mode = Mode::Spider;
        else if (m_player1->m_isSwing) g_mode = Mode::Swing;
        else g_mode = Mode::Cube;

        float spd = m_player1->m_playerSpeed;
        if (spd <= 0.8f) g_xSpeed = Phys::SPEED_SLOW / 240.f;
        else if (spd <= 0.95f) g_xSpeed = Phys::SPEED_NORMAL / 240.f;
        else if (spd <= 1.05f) g_xSpeed = Phys::SPEED_FAST / 240.f;
        else if (spd <= 1.15f) g_xSpeed = Phys::SPEED_FASTER / 240.f;
        else g_xSpeed = Phys::SPEED_FASTEST / 240.f;

        // build sim start
        Sim cur;
        cur.x = g_playerX;
        cur.y = g_playerY;
        cur.yVel = g_playerYVel;
        cur.xSpeed = g_xSpeed;
        cur.onGround = g_onGround;
        cur.canJump = !g_isHolding;
        cur.upsideDown = g_upsideDown;
        cur.mini = g_mini;
        cur.mode = g_mode;

        bool plannedHold = false;
        g_planner.getNextHold(cur, g_isHolding, plannedHold);

        // store decision for learning
        if (g_set.learn) {
            int bin = binForX(g_playerX);
            g_recent.push_back({ bin, plannedHold });
            if (g_recent.size() > MAX_RECENT) g_recent.pop_front();
        }

        // apply input
        if (plannedHold != g_isHolding) {
            this->handleButton(plannedHold, 1, true);
            g_isHolding = plannedHold;
        }
    }
};

// ============================================================
// Pause settings button + toggle
// ============================================================

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto ws = CCDirector::sharedDirector()->getWinSize();
        auto* menu = CCMenu::create();
        menu->setPosition(ccp(0,0));
        addChild(menu, 200);

        // Enable toggle (binds to setting)
        auto* onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto* offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* t = CCMenuItemToggler::create(offSpr, onSpr, this, menu_selector(BotPauseLayer::onToggle));
        t->setPosition(ccp(ws.width - 30.f, ws.height - 30.f));
        t->toggle(Mod::get()->getSettingValue<bool>("enabled"));
        menu->addChild(t);

        // Gear opens settings
        auto* gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        auto* gearBtn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(BotPauseLayer::onSettings));
        gearBtn->setPosition(ccp(ws.width - 30.f, ws.height - 75.f));
        gearBtn->setScale(0.7f);
        menu->addChild(gearBtn);
    }

    void onToggle(CCObject*) {
        bool v = !Mod::get()->getSettingValue<bool>("enabled");
        Mod::get()->setSettingValue("enabled", v);
        g_enabled = v;
        g_planner.reset();

        if (!g_enabled && g_isHolding) {
            if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
            g_isHolding = false;
        }
    }

    void onSettings(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};

// ============================================================
// Keyboard toggle (F8)
// ============================================================

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && key == KEY_F8) {
            bool v = !Mod::get()->getSettingValue<bool>("enabled");
            Mod::get()->setSettingValue("enabled", v);
            g_enabled = v;
            g_planner.reset();

            if (!g_enabled && g_isHolding) {
                if (auto* gj = GJBaseGameLayer::get()) gj->handleButton(false, 1, true);
                g_isHolding = false;
            }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// ============================================================
// Mod init
// ============================================================

$on_mod(Loaded) {
    g_set.load();
    log::info("GD AutoBot loaded. F8 toggles enabled. Settings in Geode / pause gear.");
}
