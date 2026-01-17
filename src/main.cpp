#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <vector>
#include <fstream>
#include <cmath>

using namespace geode::prelude;

enum class GameMode {
    Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing, Unknown
};

enum class ActionType {
    Click = 0, Hold = 1, Release = 2
};

struct Action {
    float x;
    ActionType type;
    GameMode mode;
    float y;
};

struct PlayerState {
    float x;
    float y;
    float velY;
    float rotation;
    bool onGround;
    bool isDead;
    bool isUpsideDown;
    bool isMini;
    GameMode mode;
    float groundY;
};

class Bot {
private:
    Bot() = default;

public:
    static Bot* get() {
        static Bot instance;
        return &instance;
    }

    bool enabled = false;
    bool pathfinding = false;
    bool replaying = false;
    bool holding = false;
    bool pendingRelease = false;
    
    std::vector<Action> actions;
    std::vector<Action> bestActions;
    size_t nextIdx = 0;
    
    float bestX = 0.f;
    float lastDeathX = 0.f;
    int fails = 0;
    int attempts = 0;
    std::vector<float> deathSpots;
    
    PlayerState state;
    PlayerState lastState;
    
    GameMode getGameMode(PlayerObject* player) {
        if (!player) return GameMode::Unknown;
        if (player->m_isShip) return GameMode::Ship;
        if (player->m_isBall) return GameMode::Ball;
        if (player->m_isBird) return GameMode::Ufo;
        if (player->m_isDart) return GameMode::Wave;
        if (player->m_isRobot) return GameMode::Robot;
        if (player->m_isSpider) return GameMode::Spider;
        if (player->m_isSwing) return GameMode::Swing;
        return GameMode::Cube;
    }
    
    std::string gameModeStr(GameMode mode) {
        switch (mode) {
            case GameMode::Cube: return "Cube";
            case GameMode::Ship: return "Ship";
            case GameMode::Ball: return "Ball";
            case GameMode::Ufo: return "UFO";
            case GameMode::Wave: return "Wave";
            case GameMode::Robot: return "Robot";
            case GameMode::Spider: return "Spider";
            case GameMode::Swing: return "Swing";
            default: return "???";
        }
    }
    
    bool needsHoldInput(GameMode mode) {
        return mode == GameMode::Ship || 
               mode == GameMode::Ufo || 
               mode == GameMode::Wave ||
               mode == GameMode::Swing;
    }
    
    void updateState(PlayerObject* player) {
        if (!player) return;
        
        lastState = state;
        
        state.x = player->getPositionX();
        state.y = player->getPositionY();
        state.velY = player->m_yVelocity;
        state.rotation = player->getRotation();
        state.onGround = player->m_isOnGround;
        state.isDead = player->m_isDead;
        state.isUpsideDown = player->m_isUpsideDown;
        state.isMini = player->m_vehicleSize < 1.0f;
        state.mode = getGameMode(player);
        
        if (state.onGround) {
            state.groundY = state.y;
        }
    }
    
    float calculateClickPosition(float deathX, int failCount) {
        GameMode mode = state.mode;
        float baseOffset = 25.f;
        
        switch (mode) {
            case GameMode::Cube:
            case GameMode::Robot:
                baseOffset = 30.f;
                break;
            case GameMode::Ship:
            case GameMode::Ufo:
                baseOffset = 50.f;
                break;
            case GameMode::Wave:
                baseOffset = 20.f;
                break;
            case GameMode::Ball:
            case GameMode::Spider:
                baseOffset = 35.f;
                break;
            default:
                baseOffset = 25.f;
        }
        
        float offset;
        switch (failCount) {
            case 1: offset = baseOffset; break;
            case 2: offset = baseOffset * 2; break;
            case 3: offset = baseOffset * 0.5f; break;
            case 4: offset = baseOffset * 3; break;
            case 5: offset = baseOffset * 0.25f; break;
            case 6: offset = -baseOffset * 0.5f; break;
            case 7: offset = baseOffset * 1.5f; break;
            case 8: offset = baseOffset * 2.5f; break;
            default: offset = baseOffset * ((failCount % 5) + 1) * 0.5f;
        }
        
        return deathX - offset;
    }
    
    ActionType determineActionType(float x) {
        GameMode mode = state.mode;
        
        if (needsHoldInput(mode)) {
            bool hasRecentHold = false;
            for (auto& a : actions) {
                if (a.x > x - 100 && a.x < x && a.type == ActionType::Hold) {
                    hasRecentHold = true;
                }
                if (a.x > x - 50 && a.x < x && a.type == ActionType::Release) {
                    hasRecentHold = false;
                }
            }
            return hasRecentHold ? ActionType::Release : ActionType::Hold;
        }
        
        return ActionType::Click;
    }
    
    void reset() {
        nextIdx = 0;
        holding = false;
        pendingRelease = false;
    }
    
    void startPathfind() {
        enabled = true;
        pathfinding = true;
        replaying = false;
        actions.clear();
        bestActions.clear();
        deathSpots.clear();
        bestX = 0.f;
        lastDeathX = 0.f;
        fails = 0;
        attempts = 0;
        reset();
        log::info("Bot: Pathfinding started");
    }
    
    void startReplay() {
        if (actions.empty() && !bestActions.empty()) {
            actions = bestActions;
        }
        if (actions.empty()) {
            log::warn("Bot: No actions to replay");
            return;
        }
        enabled = true;
        pathfinding = false;
        replaying = true;
        reset();
        log::info("Bot: Replaying {} actions", actions.size());
    }
    
    void stop() {
        enabled = false;
        pathfinding = false;
        replaying = false;
        holding = false;
        log::info("Bot: Stopped");
    }
    
    bool addAction(float x, ActionType type, GameMode mode = GameMode::Cube) {
        for (auto& a : actions) {
            if (std::abs(a.x - x) < 5.f) return false;
        }
        
        Action action;
        action.x = x;
        action.type = type;
        action.mode = mode;
        action.y = state.y;
        
        actions.push_back(action);
        std::sort(actions.begin(), actions.end(),
            [](const Action& a, const Action& b) { return a.x < b.x; });
        
        log::info("Bot: Added {} at X={:.0f} [{}]", 
            type == ActionType::Click ? "Click" : (type == ActionType::Hold ? "Hold" : "Release"),
            x, gameModeStr(mode));
        return true;
    }
    
    bool removeNear(float x, float range = 80.f) {
        for (int i = actions.size() - 1; i >= 0; i--) {
            if (actions[i].x > x - range && actions[i].x < x) {
                log::info("Bot: Removed action at X={:.0f}", actions[i].x);
                actions.erase(actions.begin() + i);
                return true;
            }
        }
        return false;
    }
    
    void clearAfter(float x) {
        size_t before = actions.size();
        actions.erase(
            std::remove_if(actions.begin(), actions.end(),
                [x](const Action& a) { return a.x >= x; }),
            actions.end());
        log::info("Bot: Cleared {} actions after X={:.0f}", before - actions.size(), x);
    }
    
    void onDeath(float x, float y, PlayerObject* player) {
        if (!pathfinding) return;
        
        attempts++;
        deathSpots.push_back(x);
        
        int deathsHere = 0;
        for (float spot : deathSpots) {
            if (std::abs(spot - x) < 30.f) deathsHere++;
        }
        
        log::info("Bot: Death #{} at X={:.0f} Y={:.0f} [{}] (deaths here: {})", 
            attempts, x, y, gameModeStr(state.mode), deathsHere);
        
        if (x > bestX + 2.f) {
            bestX = x;
            bestActions = actions;
            fails = 0;
            log::info("Bot: ★ NEW BEST: {:.0f}", x);
        } else {
            fails++;
            learnFromDeath(x, y, deathsHere, player);
        }
        
        lastDeathX = x;
    }
    
    void learnFromDeath(float deathX, float deathY, int deathsHere, PlayerObject* player) {
        GameMode mode = state.mode;
        
        if (fails > 15 || deathsHere > 10) {
            clearAfter(deathX - 200.f);
            deathSpots.clear();
            fails = 0;
            log::info("Bot: Reset section - too many fails");
            return;
        }
        
        if (fails == 7 || fails == 12) {
            if (removeNear(deathX)) {
                fails = std::max(1, fails - 3);
                return;
            }
        }
        
        float newX = calculateClickPosition(deathX, fails);
        
        if (newX > 0.f) {
            ActionType type = determineActionType(newX);
            addAction(newX, type, mode);
            
            if (type == ActionType::Hold) {
                float releaseX = deathX + 30.f + (fails * 10.f);
                addAction(releaseX, ActionType::Release, mode);
            }
        }
    }
    
    void processFrame(GJBaseGameLayer* layer, PlayerObject* player) {
        if (!enabled || !layer || !player || player->m_isDead) return;
        
        updateState(player);
        float x = state.x;
        
        // Handle pending release for clicks
        if (pendingRelease) {
            layer->handleButton(false, 1, true);
            pendingRelease = false;
        }
        
        while (nextIdx < actions.size() && x >= actions[nextIdx].x) {
            Action& a = actions[nextIdx];
            
            switch (a.type) {
                case ActionType::Click:
                    layer->handleButton(true, 1, true);
                    pendingRelease = true;
                    log::info("Bot: Click at X={:.0f}", a.x);
                    break;
                case ActionType::Hold:
                    layer->handleButton(true, 1, true);
                    holding = true;
                    log::info("Bot: Hold at X={:.0f}", a.x);
                    break;
                case ActionType::Release:
                    layer->handleButton(false, 1, true);
                    holding = false;
                    log::info("Bot: Release at X={:.0f}", a.x);
                    break;
            }
            
            nextIdx++;
        }
    }
    
    void forceRelease(GJBaseGameLayer* layer) {
        if (layer && holding) {
            layer->handleButton(false, 1, true);
            holding = false;
        }
        pendingRelease = false;
    }
    
    void save() {
        auto path = Mod::get()->getSaveDir() / "bot.json";
        std::ofstream f(path);
        
        f << "{\n";
        f << "  \"best\": " << bestX << ",\n";
        f << "  \"attempts\": " << attempts << ",\n";
        f << "  \"actions\": [\n";
        
        for (size_t i = 0; i < actions.size(); i++) {
            f << "    {\"x\": " << actions[i].x 
              << ", \"t\": " << static_cast<int>(actions[i].type)
              << ", \"m\": " << static_cast<int>(actions[i].mode)
              << ", \"y\": " << actions[i].y << "}";
            if (i < actions.size() - 1) f << ",";
            f << "\n";
        }
        
        f << "  ]\n}\n";
        
        log::info("Bot: Saved {} actions, best={:.0f}", actions.size(), bestX);
    }
    
    void load() {
        auto path = Mod::get()->getSaveDir() / "bot.json";
        std::ifstream f(path);
        if (!f) return;
        
        std::string s((std::istreambuf_iterator<char>(f)), {});
        actions.clear();
        
        size_t pos = 0;
        while ((pos = s.find("\"x\":", pos)) != std::string::npos) {
            Action a;
            pos += 4;
            a.x = std::stof(s.substr(pos));
            
            pos = s.find("\"t\":", pos) + 4;
            a.type = static_cast<ActionType>(std::stoi(s.substr(pos)));
            
            size_t mpos = s.find("\"m\":", pos);
            if (mpos != std::string::npos && mpos < s.find("}", pos)) {
                pos = mpos + 4;
                a.mode = static_cast<GameMode>(std::stoi(s.substr(pos)));
            } else {
                a.mode = GameMode::Cube;
            }
            
            size_t ypos = s.find("\"y\":", pos);
            if (ypos != std::string::npos && ypos < s.find("}", pos)) {
                pos = ypos + 4;
                a.y = std::stof(s.substr(pos));
            } else {
                a.y = 0.f;
            }
            
            actions.push_back(a);
        }
        
        pos = s.find("\"best\":");
        if (pos != std::string::npos) bestX = std::stof(s.substr(pos + 7));
        
        pos = s.find("\"attempts\":");
        if (pos != std::string::npos) attempts = std::stoi(s.substr(pos + 11));
        
        bestActions = actions;
        
        log::info("Bot: Loaded {} actions, best={:.0f}", actions.size(), bestX);
    }
    
    std::string getStatus() {
        if (!enabled) return "OFF";
        if (pathfinding) return "PATH";
        if (replaying) return "PLAY";
        return "ON";
    }
    
    std::string getDetailedStatus() {
        return fmt::format(
            "[{}] X:{:.0f} | Best:{:.0f} | Acts:{} | Try:{} | {}{}",
            getStatus(),
            state.x,
            bestX,
            actions.size(),
            attempts,
            gameModeStr(state.mode),
            holding ? " [HOLD]" : ""
        );
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool a, bool b) {
        if (!PlayLayer::init(level, a, b)) return false;
        
        Bot::get()->reset();

        m_fields->label = CCLabelBMFont::create("", "chatFont.fnt");
        m_fields->label->setAnchorPoint({0, 1});
        m_fields->label->setPosition({5, CCDirector::get()->getWinSize().height - 5});
        m_fields->label->setScale(0.5f);
        m_fields->label->setZOrder(9999);
        m_fields->label->setOpacity(200);
        addChild(m_fields->label);

        return true;
    }

    void update(float dt) {
        PlayLayer::update(dt);
        
        auto bot = Bot::get();

        if (m_player1 && !m_player1->m_isDead) {
            bot->processFrame(this, m_player1);
        }

        if (m_fields->label) {
            if (bot->enabled) {
                m_fields->label->setString(bot->getDetailedStatus().c_str());
                m_fields->label->setColor(bot->pathfinding ? ccc3(100, 255, 100) : ccc3(100, 200, 255));
            } else {
                m_fields->label->setString("");
            }
        }
    }

    void resetLevel() {
        auto bot = Bot::get();
        bot->forceRelease(this);
        bot->reset();
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (p == m_player1) {
            auto bot = Bot::get();
            bot->onDeath(p->getPositionX(), p->getPositionY(), p);
            bot->forceRelease(this);
        }
        PlayLayer::destroyPlayer(p, o);
    }
    
    void levelComplete() {
        auto bot = Bot::get();
        if (bot->enabled) {
            log::info("Bot: ★★★ LEVEL COMPLETE! ★★★");
            bot->bestX = 999999.f;
            bot->bestActions = bot->actions;
            bot->save();
        }
        PlayLayer::levelComplete();
    }
};

class BotPopup : public geode::Popup<> {
protected:
    CCLabelBMFont* statusLabel = nullptr;

    bool setup() override {
        setTitle("Smart GD Bot");
        
        auto menu = CCMenu::create();
        menu->setPosition(getContentSize() / 2);

        auto btn1 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Pathfind", "bigFont.fnt", "GJ_button_02.png", 0.75f),
            this, menu_selector(BotPopup::onPath));
        btn1->setPosition({0, 55});
        menu->addChild(btn1);

        auto btn2 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Replay", "bigFont.fnt", "GJ_button_01.png", 0.75f),
            this, menu_selector(BotPopup::onReplay));
        btn2->setPosition({0, 20});
        menu->addChild(btn2);

        auto btn3 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Stop", "bigFont.fnt", "GJ_button_06.png", 0.75f),
            this, menu_selector(BotPopup::onStop));
        btn3->setPosition({0, -15});
        menu->addChild(btn3);

        auto btn4 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save", "bigFont.fnt", "GJ_button_03.png", 0.6f),
            this, menu_selector(BotPopup::onSave));
        btn4->setPosition({-55, -50});
        menu->addChild(btn4);

        auto btn5 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_03.png", 0.6f),
            this, menu_selector(BotPopup::onLoad));
        btn5->setPosition({55, -50});
        menu->addChild(btn5);
        
        auto btn6 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Clear", "bigFont.fnt", "GJ_button_06.png", 0.5f),
            this, menu_selector(BotPopup::onClear));
        btn6->setPosition({0, -50});
        menu->addChild(btn6);

        statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        statusLabel->setPosition({0, -80});
        statusLabel->setScale(0.6f);
        menu->addChild(statusLabel);
        
        updateStatus();

        m_mainLayer->addChild(menu);
        
        schedule(schedule_selector(BotPopup::updateStatus), 0.5f);
        
        return true;
    }
    
    void updateStatus(float dt = 0) {
        auto bot = Bot::get();
        std::string s = fmt::format(
            "{} | Best: {:.0f}\nActions: {} | Attempts: {}",
            bot->getStatus(), bot->bestX, bot->actions.size(), bot->attempts
        );
        if (statusLabel) statusLabel->setString(s.c_str());
    }

    void onPath(CCObject*) {
        Bot::get()->startPathfind();
        FLAlertLayer::create(
            "Pathfind Started",
            "Resume and play!\nBot will learn from deaths.",
            "OK"
        )->show();
        updateStatus();
    }
    
    void onReplay(CCObject*) {
        auto bot = Bot::get();
        if (bot->actions.empty() && bot->bestActions.empty()) {
            FLAlertLayer::create("Error", "No actions!\nPathfind first.", "OK")->show();
            return;
        }
        bot->startReplay();
        FLAlertLayer::create("Replay Started", "Resume the level!", "OK")->show();
        updateStatus();
    }
    
    void onStop(CCObject*) {
        Bot::get()->stop();
        updateStatus();
    }
    
    void onSave(CCObject*) {
        Bot::get()->save();
        FLAlertLayer::create("Saved", "Bot data saved!", "OK")->show();
    }
    
    void onLoad(CCObject*) {
        Bot::get()->load();
        updateStatus();
        FLAlertLayer::create("Loaded", fmt::format("Loaded {} actions!", Bot::get()->actions.size()), "OK")->show();
    }
    
    void onClear(CCObject*) {
        auto bot = Bot::get();
        bot->actions.clear();
        bot->bestActions.clear();
        bot->deathSpots.clear();
        bot->bestX = 0.f;
        bot->attempts = 0;
        bot->fails = 0;
        bot->reset();
        updateStatus();
        FLAlertLayer::create("Cleared", "All data cleared!", "OK")->show();
    }

public:
    static BotPopup* create() {
        auto ret = new BotPopup();
        if (ret && ret->initAnchored(240, 210)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        
        auto spr = ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_01.png", 0.65f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPauseLayer::onBot));
        btn->setPosition({winSize.width - 60, winSize.height - 25});
        menu->addChild(btn);
        
        addChild(menu, 100);
    }

    void onBot(CCObject*) {
        BotPopup::create()->show();
    }
};

$on_mod(Loaded) {
    log::info("Smart GD Bot loaded!");
}
