#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <vector>
#include <fstream>

using namespace geode::prelude;

struct Action {
    float x;
    int type;
};

class Bot {
public:
    static Bot* get() {
        static Bot instance;
        return &instance;
    }

    bool enabled = false;
    bool pathfinding = false;
    bool replaying = false;
    bool holding = false;

    std::vector<Action> actions;
    size_t nextIdx = 0;
    float bestX = 0.f;
    int fails = 0;
    int attempts = 0;

    void reset() {
        nextIdx = 0;
        holding = false;
    }

    void startPathfind() {
        enabled = true;
        pathfinding = true;
        replaying = false;
        actions.clear();
        bestX = 0.f;
        fails = 0;
        attempts = 0;
        reset();
        log::info("Bot: Pathfinding STARTED");
    }

    void startReplay() {
        if (actions.empty()) {
            log::info("Bot: No actions to replay!");
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

    bool addAction(float x, int type) {
        for (auto& a : actions) {
            if (std::abs(a.x - x) < 5.f) return false;
        }
        actions.push_back({x, type});
        std::sort(actions.begin(), actions.end(),
            [](auto& a, auto& b) { return a.x < b.x; });
        log::info("Bot: Added click at X={}", x);
        return true;
    }

    void removeNear(float x) {
        for (int i = actions.size() - 1; i >= 0; i--) {
            if (actions[i].x > x - 80.f && actions[i].x < x) {
                log::info("Bot: Removed action at X={}", actions[i].x);
                actions.erase(actions.begin() + i);
                return;
            }
        }
    }

    void onDeath(float x) {
        if (!pathfinding) return;
        
        attempts++;
        log::info("Bot: Death at X={} (attempt {})", x, attempts);

        if (x > bestX + 2.f) {
            bestX = x;
            fails = 0;
            log::info("Bot: NEW BEST! X={}", x);
        } else {
            fails++;

            float newX = 0.f;
            if (fails == 1) newX = x - 25.f;
            else if (fails == 2) newX = x - 50.f;
            else if (fails == 3) newX = x - 15.f;
            else if (fails == 4) newX = x - 70.f;
            else if (fails == 5) newX = x - 8.f;
            else if (fails == 6) { removeNear(x); fails = 1; return; }
            else if (fails == 7) newX = x + 5.f;
            else if (fails > 10) {
                actions.erase(
                    std::remove_if(actions.begin(), actions.end(),
                        [x](auto& a) { return a.x > x - 200.f; }),
                    actions.end());
                fails = 0;
                log::info("Bot: Reset section");
                return;
            }

            if (newX > 0.f) addAction(newX, 0);
        }
    }

    void process(PlayerObject* p, float x) {
        if (!enabled || !p) return;

        while (nextIdx < actions.size() && x >= actions[nextIdx].x) {
            auto& a = actions[nextIdx];
            if (a.type == 0) {
                p->pushButton(PlayerButton::Jump);
                p->releaseButton(PlayerButton::Jump);
                log::info("Bot: Click at X={}", a.x);
            } else if (a.type == 1) {
                p->pushButton(PlayerButton::Jump);
                holding = true;
            } else {
                p->releaseButton(PlayerButton::Jump);
                holding = false;
            }
            nextIdx++;
        }
    }

    void release(PlayerObject* p) {
        if (p && holding) {
            p->releaseButton(PlayerButton::Jump);
            holding = false;
        }
    }

    void save() {
        auto path = Mod::get()->getSaveDir() / "bot.json";
        std::ofstream f(path);
        f << "{\"best\":" << bestX << ",\"actions\":[";
        for (size_t i = 0; i < actions.size(); i++) {
            f << "{\"x\":" << actions[i].x << ",\"t\":" << actions[i].type << "}";
            if (i < actions.size() - 1) f << ",";
        }
        f << "]}";
        log::info("Bot: Saved {} actions", actions.size());
    }

    void load() {
        auto path = Mod::get()->getSaveDir() / "bot.json";
        std::ifstream f(path);
        if (!f) return;

        std::string s((std::istreambuf_iterator<char>(f)), {});
        actions.clear();

        size_t pos = 0;
        while ((pos = s.find("\"x\":", pos)) != std::string::npos) {
            pos += 4;
            float x = std::stof(s.substr(pos));
            pos = s.find("\"t\":", pos) + 4;
            int t = std::stoi(s.substr(pos));
            actions.push_back({x, t});
        }

        pos = s.find("\"best\":");
        if (pos != std::string::npos) bestX = std::stof(s.substr(pos + 7));
        
        log::info("Bot: Loaded {} actions, best={}", actions.size(), bestX);
    }
    
    std::string getStatus() {
        if (!enabled) return "OFF";
        if (pathfinding) return "PATHFIND";
        if (replaying) return "REPLAY";
        return "ON";
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
        m_fields->label->setScale(0.6f);
        m_fields->label->setZOrder(999);
        addChild(m_fields->label);

        return true;
    }

    void update(float dt) {
        PlayLayer::update(dt);
        auto bot = Bot::get();

        if (m_player1 && !m_player1->m_isDead) {
            bot->process(m_player1, m_player1->getPositionX());
        }

        if (m_fields->label) {
            if (bot->enabled) {
                auto txt = fmt::format("[{}] X:{:.0f} | Best:{:.0f} | Clicks:{} | Try:{}",
                    bot->getStatus(),
                    m_player1 ? m_player1->getPositionX() : 0.f,
                    bot->bestX, 
                    bot->actions.size(),
                    bot->attempts);
                m_fields->label->setString(txt.c_str());
            } else {
                m_fields->label->setString("");
            }
        }
    }

    void resetLevel() {
        auto bot = Bot::get();
        if (m_player1) bot->release(m_player1);
        bot->reset();
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (p == m_player1) {
            Bot::get()->onDeath(p->getPositionX());
            Bot::get()->release(p);
        }
        PlayLayer::destroyPlayer(p, o);
    }
};

class BotPopup : public geode::Popup<> {
protected:
    CCLabelBMFont* statusLabel = nullptr;

    bool setup() override {
        setTitle("GD Bot");
        auto menu = CCMenu::create();
        menu->setPosition(getContentSize() / 2);

        // Pathfind button
        auto btn1 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Pathfind", "bigFont.fnt", "GJ_button_02.png", 0.8f), 
            this, menu_selector(BotPopup::onPath));
        btn1->setPosition({0, 50});
        menu->addChild(btn1);

        // Replay button
        auto btn2 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Replay", "bigFont.fnt", "GJ_button_01.png", 0.8f), 
            this, menu_selector(BotPopup::onReplay));
        btn2->setPosition({0, 15});
        menu->addChild(btn2);

        // Stop button
        auto btn3 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Stop", "bigFont.fnt", "GJ_button_06.png", 0.8f), 
            this, menu_selector(BotPopup::onStop));
        btn3->setPosition({0, -20});
        menu->addChild(btn3);

        // Save button
        auto btn4 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save", "bigFont.fnt", "GJ_button_03.png", 0.6f), 
            this, menu_selector(BotPopup::onSave));
        btn4->setPosition({-50, -55});
        menu->addChild(btn4);

        // Load button
        auto btn5 = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_03.png", 0.6f), 
            this, menu_selector(BotPopup::onLoad));
        btn5->setPosition({50, -55});
        menu->addChild(btn5);

        // Status label
        statusLabel = CCLabelBMFont::create("Status: OFF", "chatFont.fnt");
        statusLabel->setPosition({0, -85});
        statusLabel->setScale(0.7f);
        menu->addChild(statusLabel);
        
        updateStatus();

        m_mainLayer->addChild(menu);
        return true;
    }
    
    void updateStatus() {
        auto bot = Bot::get();
        std::string status = fmt::format("Status: {} | Best: {:.0f} | Clicks: {}", 
            bot->getStatus(), bot->bestX, bot->actions.size());
        if (statusLabel) statusLabel->setString(status.c_str());
    }

    void onPath(CCObject*) { 
        Bot::get()->startPathfind(); 
        FLAlertLayer::create("Bot", "Pathfinding started!\n\nResume and play the level.\nIt will learn from your deaths.", "OK")->show();
        updateStatus();
    }
    
    void onReplay(CCObject*) { 
        auto bot = Bot::get();
        if (bot->actions.empty()) {
            FLAlertLayer::create("Bot", "No actions recorded!\n\nUse Pathfind first.", "OK")->show();
            return;
        }
        bot->startReplay(); 
        FLAlertLayer::create("Bot", fmt::format("Replaying {} clicks!\n\nResume the level.", bot->actions.size()), "OK")->show();
        updateStatus();
    }
    
    void onStop(CCObject*) { 
        Bot::get()->stop(); 
        FLAlertLayer::create("Bot", "Bot stopped.", "OK")->show();
        updateStatus();
    }
    
    void onSave(CCObject*) { 
        Bot::get()->save(); 
        FLAlertLayer::create("Bot", "Saved!", "OK")->show(); 
    }
    
    void onLoad(CCObject*) { 
        Bot::get()->load(); 
        updateStatus();
        FLAlertLayer::create("Bot", fmt::format("Loaded {} actions!", Bot::get()->actions.size()), "OK")->show(); 
    }

public:
    static BotPopup* create() {
        auto ret = new BotPopup();
        if (ret && ret->initAnchored(220, 200)) { ret->autorelease(); return ret; }
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
        
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_01.png", 0.7f),
            this, menu_selector(BotPauseLayer::onBot));
        btn->setPosition({winSize.width - 60, winSize.height - 30});
        menu->addChild(btn);
        
        addChild(menu, 100);
    }

    void onBot(CCObject*) { 
        BotPopup::create()->show(); 
    }
};

$on_mod(Loaded) { 
    log::info("GD Bot loaded successfully!"); 
}
