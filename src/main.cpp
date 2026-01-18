#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

// ============================================================================
// SIMPLE LOGGING
// ============================================================================

#define LOGD(...) geode::log::debug(__VA_ARGS__)
#define LOGI(...) geode::log::info(__VA_ARGS__)
#define LOGW(...) geode::log::warn(__VA_ARGS__)
#define LOGE(...) geode::log::error(__VA_ARGS__)

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

namespace Physics {
    constexpr float BLOCK = 30.0f;
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VEL = 11.180032f;
    constexpr float XVEL = 5.770002f;
}

// ============================================================================
// SIMPLE RECT
// ============================================================================

struct HitRect {
    float x, y, w, h;
    
    bool intersects(const HitRect& o) const {
        return !((x + w) <= o.x || (o.x + o.w) <= x ||
                 (y + h) <= o.y || (o.y + o.h) <= y);
    }
};

// ============================================================================
// LEVEL OBJECT
// ============================================================================

struct LevelObject {
    int id;
    float x, y, w, h;
    bool isHazard;
    bool isSolid;
};

// ============================================================================
// PLAYER SIM STATE
// ============================================================================

struct SimState {
    float x = 0, y = 105;
    float velY = 0;
    bool onGround = true;
    bool dead = false;
    bool won = false;
    int frame = 0;
};

// ============================================================================
// LEVEL ANALYZER
// ============================================================================

class LevelAnalyzer {
public:
    std::vector<LevelObject> objects;
    float levelLength = 0;
    bool loaded = false;
    
    static LevelAnalyzer& get() {
        static LevelAnalyzer instance;
        return instance;
    }
    
    void analyze(PlayLayer* pl) {
        objects.clear();
        levelLength = 0;
        loaded = false;
        
        if (!pl) {
            LOGE("PlayLayer is null");
            return;
        }
        
        LOGI("=== ANALYZING LEVEL ===");
        
        // Debug: print all member info
        LOGI("m_player1: {}", pl->m_player1 ? "exists" : "null");
        LOGI("m_levelLength: {}", pl->m_levelLength);
        
        // Use m_levelLength directly
        levelLength = pl->m_levelLength;
        if (levelLength < 100) levelLength = 5000; // Default
        
        // Try to get objects from m_objects
        if (pl->m_objects) {
            int count = pl->m_objects->count();
            LOGI("m_objects count: {}", count);
            
            for (int i = 0; i < count; i++) {
                auto obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                if (obj) {
                    processObject(obj);
                }
            }
        } else {
            LOGI("m_objects is null, trying alternative methods...");
        }
        
        // Try batchNodePlayer
        if (objects.empty() && pl->m_batchNodePlayer) {
            LOGI("Trying m_batchNodePlayer...");
            auto children = pl->m_batchNodePlayer->getChildren();
            if (children) {
                LOGI("batchNodePlayer children: {}", children->count());
            }
        }
        
        // Try object layer
        if (objects.empty() && pl->m_objectLayer) {
            LOGI("Trying m_objectLayer...");
            scanNode(pl->m_objectLayer);
        }
        
        // Last resort: scan everything
        if (objects.empty()) {
            LOGI("Scanning all children...");
            scanNode(pl);
        }
        
        loaded = objects.size() > 0;
        
        LOGI("=== ANALYSIS COMPLETE ===");
        LOGI("Total objects found: {}", objects.size());
        LOGI("Hazards: {}", std::count_if(objects.begin(), objects.end(), [](auto& o) { return o.isHazard; }));
        LOGI("Solids: {}", std::count_if(objects.begin(), objects.end(), [](auto& o) { return o.isSolid; }));
        LOGI("Level length: {}", levelLength);
    }
    
private:
    void scanNode(CCNode* node) {
        if (!node) return;
        
        auto children = node->getChildren();
        if (!children) return;
        
        for (int i = 0; i < children->count(); i++) {
            auto child = static_cast<CCNode*>(children->objectAtIndex(i));
            if (!child) continue;
            
            if (auto gameObj = typeinfo_cast<GameObject*>(child)) {
                processObject(gameObj);
            }
            
            // Recurse
            scanNode(child);
        }
    }
    
    void processObject(GameObject* obj) {
        if (!obj) return;
        
        int id = obj->m_objectID;
        if (id <= 0) return;
        
        auto pos = obj->getPosition();
        auto size = obj->getContentSize();
        float scaleX = std::abs(obj->getScaleX());
        float scaleY = std::abs(obj->getScaleY());
        
        float w = size.width * scaleX;
        float h = size.height * scaleY;
        if (w < 1) w = Physics::BLOCK;
        if (h < 1) h = Physics::BLOCK;
        
        LevelObject lo;
        lo.id = id;
        lo.x = pos.x;
        lo.y = pos.y;
        lo.w = w;
        lo.h = h;
        
        // Check if hazard (spikes, saws)
        lo.isHazard = (id == 8 || id == 39 || id == 103 || id == 135 || 
                       id == 140 || id == 1332 || id == 1333 || id == 1334 ||
                       id == 88 || id == 89 || id == 98 || id == 397);
        
        // Check if solid block
        lo.isSolid = (id >= 1 && id <= 7) || (id >= 40 && id <= 50);
        
        if (lo.isHazard || lo.isSolid) {
            objects.push_back(lo);
        }
        
        if (pos.x > levelLength) {
            levelLength = pos.x;
        }
    }
};

// ============================================================================
// SIMPLE PATHFINDER
// ============================================================================

class SimplePathfinder {
public:
    std::atomic<bool> running{false};
    std::atomic<bool> found{false};
    std::atomic<float> progress{0};
    std::vector<bool> solution;
    std::mutex mtx;
    std::thread worker;
    
    static SimplePathfinder& get() {
        static SimplePathfinder instance;
        return instance;
    }
    
    ~SimplePathfinder() {
        stop();
    }
    
    void start() {
        if (running) return;
        
        auto& analyzer = LevelAnalyzer::get();
        if (!analyzer.loaded) {
            LOGE("Level not analyzed!");
            return;
        }
        
        running = true;
        found = false;
        progress = 0;
        solution.clear();
        
        worker = std::thread([this]() {
            findPath();
        });
    }
    
    void stop() {
        running = false;
        if (worker.joinable()) {
            worker.join();
        }
    }
    
private:
    void findPath() {
        LOGI("Pathfinder thread started");
        
        auto& analyzer = LevelAnalyzer::get();
        float levelLen = analyzer.levelLength + 100;
        
        // Simple simulation
        std::vector<std::pair<SimState, std::vector<bool>>> beam;
        
        SimState initial;
        beam.push_back({initial, {}});
        
        int maxFrames = 50000;
        float bestX = 0;
        
        for (int frame = 0; frame < maxFrames && running; frame++) {
            std::vector<std::pair<SimState, std::vector<bool>>> nextBeam;
            
            for (auto& [state, inputs] : beam) {
                if (state.dead) continue;
                
                if (state.x >= levelLen - 50) {
                    std::lock_guard<std::mutex> lock(mtx);
                    solution = inputs;
                    found = true;
                    running = false;
                    LOGI("Path found! {} inputs", solution.size());
                    return;
                }
                
                // Try both inputs
                for (int inp = 0; inp < 2; inp++) {
                    bool click = (inp == 1);
                    
                    SimState ns = state;
                    std::vector<bool> ni = inputs;
                    
                    // Simulate one frame
                    simulateFrame(ns, click, analyzer);
                    ni.push_back(click);
                    
                    if (!ns.dead) {
                        nextBeam.push_back({ns, ni});
                    }
                }
            }
            
            if (nextBeam.empty()) {
                LOGE("All states dead at frame {}", frame);
                break;
            }
            
            // Sort by x position
            std::sort(nextBeam.begin(), nextBeam.end(),
                [](auto& a, auto& b) { return a.first.x > b.first.x; });
            
            // Keep best states
            if (nextBeam.size() > 1000) {
                nextBeam.resize(1000);
            }
            
            beam = std::move(nextBeam);
            
            if (!beam.empty() && beam[0].first.x > bestX) {
                bestX = beam[0].first.x;
                progress = bestX / levelLen;
            }
            
            if (frame % 100 == 0) {
                LOGI("Frame {}, beam size {}, best x {:.0f}", frame, beam.size(), bestX);
            }
        }
        
        if (!beam.empty()) {
            std::lock_guard<std::mutex> lock(mtx);
            solution = beam[0].second;
        }
        
        running = false;
        LOGI("Pathfinder finished, best progress: {:.1f}%", progress * 100);
    }
    
    void simulateFrame(SimState& s, bool click, LevelAnalyzer& analyzer) {
        float dt = 1.0f / 240.0f;
        
        // Apply gravity
        s.velY -= Physics::GRAVITY * dt * 60;
        
        // Jump
        if (s.onGround && click) {
            s.velY = Physics::JUMP_VEL;
            s.onGround = false;
        }
        
        // Move
        s.y += s.velY * dt * 60;
        s.x += Physics::XVEL * dt;
        
        // Clamp velocity
        if (s.velY < -20) s.velY = -20;
        if (s.velY > 20) s.velY = 20;
        
        // Ground check
        if (s.y <= 105) {
            s.y = 105;
            s.velY = 0;
            s.onGround = true;
        }
        
        // Collision check
        HitRect player = {s.x - 12, s.y - 12, 24, 24};
        
        for (auto& obj : analyzer.objects) {
            if (obj.x < s.x - 100) continue;
            if (obj.x > s.x + 100) break;
            
            HitRect objRect = {obj.x - obj.w/2, obj.y - obj.h/2, obj.w, obj.h};
            
            if (player.intersects(objRect)) {
                if (obj.isHazard) {
                    s.dead = true;
                    return;
                }
                
                if (obj.isSolid) {
                    // Simple collision resolution
                    float overlapY = (player.y + player.h) - objRect.y;
                    if (overlapY > 0 && overlapY < 20 && s.velY <= 0) {
                        s.y = objRect.y + objRect.h + 12;
                        s.velY = 0;
                        s.onGround = true;
                    }
                }
            }
        }
        
        s.frame++;
    }
};

// ============================================================================
// REPLAY
// ============================================================================

class SimpleReplay {
public:
    std::vector<bool> inputs;
    size_t frame = 0;
    bool playing = false;
    
    static SimpleReplay& get() {
        static SimpleReplay instance;
        return instance;
    }
    
    void load(const std::vector<bool>& inp) {
        inputs = inp;
        frame = 0;
        LOGI("Loaded {} inputs", inputs.size());
    }
    
    void start() {
        playing = true;
        frame = 0;
    }
    
    void stop() {
        playing = false;
    }
    
    bool getInput() {
        if (!playing || frame >= inputs.size()) return false;
        return inputs[frame];
    }
    
    void advance() {
        if (playing) {
            frame++;
            if (frame >= inputs.size()) {
                playing = false;
            }
        }
    }
};

// ============================================================================
// POPUP
// ============================================================================

class PFPopup : public geode::Popup<> {
protected:
    CCLabelBMFont* m_label = nullptr;
    
    bool setup() override {
        setTitle("Pathfinder");
        
        m_label = CCLabelBMFont::create("Ready", "bigFont.fnt");
        m_label->setScale(0.4f);
        m_label->setPosition({0, 20});
        m_mainLayer->addChild(m_label);
        
        auto analyzeBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Analyze", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(PFPopup::onAnalyze)
        );
        analyzeBtn->setPosition({-70, -20});
        
        auto findBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Find", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(PFPopup::onFind)
        );
        findBtn->setPosition({0, -20});
        
        auto playBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Play", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(PFPopup::onPlay)
        );
        playBtn->setPosition({70, -20});
        
        auto menu = CCMenu::create();
        menu->addChild(analyzeBtn);
        menu->addChild(findBtn);
        menu->addChild(playBtn);
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);
        
        schedule(schedule_selector(PFPopup::onUpdate), 0.1f);
        
        return true;
    }
    
    void onUpdate(float dt) {
        auto& pf = SimplePathfinder::get();
        auto& analyzer = LevelAnalyzer::get();
        
        std::string text;
        if (pf.running) {
            text = fmt::format("Finding: {:.1f}%", pf.progress * 100);
        } else if (pf.found) {
            text = fmt::format("Found! {} inputs", pf.solution.size());
        } else if (analyzer.loaded) {
            text = fmt::format("Analyzed: {} objects", analyzer.objects.size());
        } else {
            text = "Click Analyze first";
        }
        
        m_label->setString(text.c_str());
    }
    
    void onAnalyze(CCObject*) {
        auto pl = PlayLayer::get();
        if (pl) {
            LevelAnalyzer::get().analyze(pl);
        } else {
            FLAlertLayer::create("Error", "Not in a level!", "OK")->show();
        }
    }
    
    void onFind(CCObject*) {
        if (!LevelAnalyzer::get().loaded) {
            FLAlertLayer::create("Error", "Analyze first!", "OK")->show();
            return;
        }
        SimplePathfinder::get().start();
    }
    
    void onPlay(CCObject*) {
        auto& pf = SimplePathfinder::get();
        if (pf.found && !pf.solution.empty()) {
            SimpleReplay::get().load(pf.solution);
            SimpleReplay::get().start();
            onClose(nullptr);
        } else {
            FLAlertLayer::create("Error", "No path found!", "OK")->show();
        }
    }
    
public:
    static PFPopup* create() {
        auto ret = new PFPopup();
        if (ret->initAnchored(280, 140)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ============================================================================
// HOOKS
// ============================================================================

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        bool inputHeld = false;
    };
    
    void update(float dt) {
        auto& replay = SimpleReplay::get();
        
        if (replay.playing && m_player1) {
            bool inp = replay.getInput();
            
            if (inp && !m_fields->inputHeld) {
                m_fields->inputHeld = true;
                // Try direct jump call
                if (m_player1->m_isOnGround) {
                    m_player1->m_yVelocity = Physics::JUMP_VEL;
                    m_player1->m_isOnGround = false;
                }
            } else if (!inp) {
                m_fields->inputHeld = false;
            }
            
            replay.advance();
        }
        
        PlayLayer::update(dt);
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->inputHeld = false;
        
        auto& replay = SimpleReplay::get();
        if (replay.playing) {
            replay.frame = 0;
        }
    }
};

// Add button to pause menu instead of level info
class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        auto menu = CCMenu::create();
        
        auto spr = ButtonSprite::create("PF", "goldFont.fnt", "GJ_button_04.png", 0.8f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(MyPauseLayer::onPF)
        );
        
        menu->addChild(btn);
        menu->setPosition({50, 50});
        this->addChild(menu, 100);
        
        LOGI("PF button added to pause menu");
    }
    
    void onPF(CCObject*) {
        PFPopup::create()->show();
    }
};

// Also add to main menu for testing
class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        
        auto menu = CCMenu::create();
        
        auto spr = ButtonSprite::create("PF", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(MyMenuLayer::onPF)
        );
        
        menu->addChild(btn);
        menu->setPosition({60, 60});
        this->addChild(menu, 100);
        
        return true;
    }
    
    void onPF(CCObject*) {
        FLAlertLayer::create("Info", "Enter a level first, then pause to access Pathfinder", "OK")->show();
    }
};

$on_mod(Loaded) {
    LOGI("=== GD Pathfinder Loaded ===");
}