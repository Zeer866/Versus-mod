#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>
#include <cctype>
#include <unordered_map>

using namespace geode::prelude;

static constexpr char const* VERSUS_URL = "https://zeer2.alwaysdata.net/api";

struct VersusGhost {
    int id = 0;
    std::string name;
    float x = 0.f, y = 0.f, rot = 0.f;
    int pct = 0;
    bool dead = false, finished = false;
};

static std::string versusName() {
    std::string n = GJAccountManager::get()->m_username;
    return n.empty() ? std::string("Player") : n;
}

// ---------------- Red (HTTP polling) ----------------
class VersusNet {
public:
    static VersusNet& get() {
        static VersusNet inst;
        return inst;
    }

    bool connected = false;
    int myId = 0;
    int levelID = 0;
    int hostId = 0;
    int countdown = 0;
    std::string code;
    std::string state = "none";
    std::vector<std::pair<int, std::string>> players;
    std::vector<VersusGhost> ghosts;

    bool inRoom() const { return !code.empty(); }
    bool racing() const { return state == "racing"; }

    void createRoom(std::string name, int level) {
        m_closing = false;
        send(matjson::makeObject({{"t", "create"}, {"name", name}, {"level", level}}));
    }
    void joinRoom(std::string roomCode, std::string name) {
        m_closing = false;
        send(matjson::makeObject({{"t", "join"}, {"code", roomCode}, {"name", name}}));
    }
    void startRace() { send(matjson::makeObject({{"t", "start"}})); }
    void sendFinish() { send(matjson::makeObject({{"t", "finish"}})); }
    void sendPos(float x, float y, float rot, float pct) {
        send(matjson::makeObject({
            {"t", "pos"},
            {"x", (double)x},
            {"y", (double)y},
            {"rot", (double)rot},
            {"pct", (double)pct},
        }));
    }
    void leaveRoom() {
        send(matjson::makeObject({{"t", "leave"}}));
        m_closing = true;
        code.clear();
        state = "none";
        players.clear();
        ghosts.clear();
        countdown = 0;
    }

    void tick(float dt) {
        if (!connected) return;
        m_timer += dt;
        float interval = racing() ? 0.1f : 0.4f;
        if (m_busy || m_timer < interval) return;
        m_timer = 0.f;
        m_busy = true;

        matjson::Value body = matjson::makeObject({
            {"pid", m_pid},
            {"msgs", matjson::Value(m_outbox)},
        });
        m_outbox.clear();

        auto req = web::WebRequest();
        req.timeout(std::chrono::seconds(5));
        req.bodyJSON(body);
        m_holder.spawn(req.post(VERSUS_URL), [this](web::WebResponse res) {
            m_busy = false;
            this->onResponse(res);
        });
    }

private:
    std::string m_pid;
    std::vector<matjson::Value> m_outbox;
    async::TaskHolder<web::WebResponse> m_holder;
    float m_timer = 0.f;
    bool m_busy = false;
    bool m_closing = false;
    int m_fails = 0;

    void send(matjson::Value msg) {
        connected = true;
        m_outbox.push_back(std::move(msg));
    }

    void reset() {
        connected = false;
        m_pid.clear();
        m_outbox.clear();
        m_closing = false;
        m_fails = 0;
        code.clear();
        state = "none";
        players.clear();
        ghosts.clear();
        countdown = 0;
    }

    void onResponse(web::WebResponse& res) {
        if (!res.ok()) {
            m_fails++;
            if (m_fails == 1) {
                Notification::create("Versus: sin conexion", NotificationIcon::Error)->show();
            }
            if (m_fails >= 5) this->reset();
            return;
        }
        m_fails = 0;

        auto parsed = res.json();
        if (!parsed.isOk()) return;
        auto j = parsed.unwrap();

        m_pid = j["pid"].asString().unwrapOr(m_pid);
        myId = (int)j["id"].asInt().unwrapOr(myId);

        auto msgs = j["msgs"].asArray();
        if (msgs.isOk()) {
            for (auto m : msgs.unwrap()) this->handle(m);
        }

        if (m_closing && m_outbox.empty()) this->reset();
    }

    void handle(matjson::Value m) {
        if (m_closing) return;
        auto t = m["t"].asString().unwrapOr("");

        if (t == "room") {
            code = m["code"].asString().unwrapOr("");
            levelID = (int)m["level"].asInt().unwrapOr(0);
            hostId = (int)m["host"].asInt().unwrapOr(0);
            state = m["state"].asString().unwrapOr("lobby");
            players.clear();
            auto arr = m["players"].asArray();
            if (arr.isOk()) {
                for (auto p : arr.unwrap()) {
                    players.push_back({
                        (int)p["id"].asInt().unwrapOr(0),
                        p["name"].asString().unwrapOr("?"),
                    });
                }
            }
        } else if (t == "countdown") {
            countdown = (int)m["seconds"].asInt().unwrapOr(0);
            state = "countdown";
        } else if (t == "go") {
            state = "racing";
            Notification::create("Versus: YA!")->show();
        } else if (t == "snap") {
            ghosts.clear();
            auto arr = m["players"].asArray();
            if (arr.isOk()) {
                for (auto p : arr.unwrap()) {
                    VersusGhost g;
                    g.id = (int)p["id"].asInt().unwrapOr(0);
                    g.name = p["name"].asString().unwrapOr("?");
                    g.x = (float)p["x"].asDouble().unwrapOr(0.0);
                    g.y = (float)p["y"].asDouble().unwrapOr(0.0);
                    g.rot = (float)p["rot"].asDouble().unwrapOr(0.0);
                    g.pct = (int)p["pct"].asDouble().unwrapOr(0.0);
                    g.dead = p["dead"].asBool().unwrapOr(false);
                    g.finished = p["finished"].asBool().unwrapOr(false);
                    ghosts.push_back(g);
                }
            }
        } else if (t == "results") {
            std::string txt;
            auto arr = m["ranking"].asArray();
            if (arr.isOk()) {
                for (auto r : arr.unwrap()) {
                    int pos = (int)r["pos"].asInt().unwrapOr(0);
                    std::string name = r["name"].asString().unwrapOr("?");
                    double time = r["time"].asDouble().unwrapOr(-1.0);
                    int pct = (int)r["pct"].asDouble().unwrapOr(0.0);
                    if (time >= 0.0) {
                        txt += fmt::format("{}. {} - {:.2f}s\n", pos, name, time / 1000.0);
                    } else {
                        txt += fmt::format("{}. {} - {}%\n", pos, name, pct);
                    }
                }
            }
            state = "lobby";
            FLAlertLayer::create("Versus - Resultados", txt, "OK")->show();
        } else if (t == "error") {
            auto msg = m["msg"].asString().unwrapOr("Error");
            Notification::create(msg, NotificationIcon::Error)->show();
        }
    }
};

// Tick global: mantiene la conexion viva en cualquier pantalla
class $modify(VersusScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);
        VersusNet::get().tick(dt);
    }
};

// ---------------- Popup ----------------
class VersusPopup : public Popup {
protected:
    TextInput* m_codeInput = nullptr;
    TextInput* m_levelInput = nullptr;
    CCLabelBMFont* m_info = nullptr;

    bool init() {
        if (!Popup::init(340.f, 250.f)) return false;
        this->setTitle("Versus");

        m_codeInput = TextInput::create(110.f, "Codigo");
        m_codeInput->setCommonFilter(CommonFilter::Alphanumeric);
        m_codeInput->setMaxCharCount(5);
        m_mainLayer->addChildAtPosition(m_codeInput, Anchor::Center, ccp(-80, 60));

        m_levelInput = TextInput::create(110.f, "ID del nivel");
        m_levelInput->setCommonFilter(CommonFilter::Int);
        m_levelInput->setNumericMode(true);
        m_levelInput->setMaxCharCount(10);
        m_mainLayer->addChildAtPosition(m_levelInput, Anchor::Center, ccp(80, 60));

        auto addBtn = [this](char const* text, SEL_MenuHandler cb, CCPoint pos) {
            auto spr = ButtonSprite::create(text);
            spr->setScale(0.75f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, pos);
        };
        addBtn("Crear", menu_selector(VersusPopup::onCreate), ccp(-100, 20));
        addBtn("Unirse", menu_selector(VersusPopup::onJoin), ccp(0, 20));
        addBtn("Salir", menu_selector(VersusPopup::onLeave), ccp(100, 20));
        addBtn("Iniciar", menu_selector(VersusPopup::onStart), ccp(-60, -15));
        addBtn("Abrir nivel", menu_selector(VersusPopup::onOpenLevel), ccp(60, -15));

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.7f);
        m_mainLayer->addChildAtPosition(m_info, Anchor::Center, ccp(0, -75));

        this->schedule(schedule_selector(VersusPopup::refresh), 0.25f);
        this->refresh(0.f);
        return true;
    }

    void refresh(float) {
        auto& net = VersusNet::get();
        std::string s;
        if (!net.inRoom()) {
            s = "Crea una sala (pon el ID del nivel)\no unete con un codigo.";
        } else {
            s = fmt::format("Sala: {}   Nivel: {}\n", net.code, net.levelID);
            if (net.state == "countdown") {
                s += fmt::format("Abre el nivel! Empieza en {}s\n", net.countdown);
            } else if (net.state == "racing") {
                s += "CARRERA EN CURSO\n";
            } else {
                s += "En espera\n";
            }
            s += fmt::format("Jugadores ({}):\n", net.players.size());
            for (auto& [id, name] : net.players) {
                s += fmt::format(" - {}{}\n", name, id == net.hostId ? " (host)" : "");
            }
        }
        m_info->setString(s.c_str());
    }

    void onCreate(CCObject*) {
        auto lvl = utils::numFromString<int>(m_levelInput->getString()).unwrapOr(0);
        if (lvl <= 0) {
            Notification::create("Pon el ID del nivel", NotificationIcon::Error)->show();
            return;
        }
        VersusNet::get().createRoom(versusName(), lvl);
    }

    void onJoin(CCObject*) {
        std::string code(m_codeInput->getString());
        for (auto& c : code) c = (char)std::toupper((unsigned char)c);
        if (code.size() < 5) {
            Notification::create("Pon el codigo de 5 letras", NotificationIcon::Error)->show();
            return;
        }
        VersusNet::get().joinRoom(code, versusName());
    }

    void onStart(CCObject*) { VersusNet::get().startRace(); }
    void onLeave(CCObject*) { VersusNet::get().leaveRoom(); }

    void onOpenLevel(CCObject*) {
        int id = VersusNet::get().levelID;
        if (id <= 0) return;
        auto search = GJSearchObject::create(SearchType::Search, std::to_string(id));
        auto scene = LevelBrowserLayer::scene(search);
        CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, scene));
    }

public:
    static VersusPopup* create() {
        auto ret = new VersusPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------- Boton en el menu principal ----------------
class $modify(VersusMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto spr = CircleButtonSprite::createWithSprite(
            "versus-icon.png"_spr, 1.f, CircleBaseColor::Green, CircleBaseSize::Medium
        );
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(VersusMenu::onVersus));
        btn->setID("versus-button");

        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto m = CCMenu::create();
            m->addChild(btn);
            m->setPosition(ccp(40.f, 40.f));
            this->addChild(m);
        }
        return true;
    }

    void onVersus(CCObject*) {
        if (auto p = VersusPopup::create()) p->show();
    }
};

// ---------------- Carrera dentro del nivel ----------------
class $modify(VersusPlayLayer, PlayLayer) {
    struct Fields {
        std::unordered_map<int, SimplePlayer*> ghosts;
        float posTimer = 0.f;
    };

    void clearGhosts() {
        for (auto& [id, g] : m_fields->ghosts) {
            if (g) g->removeFromParent();
        }
        m_fields->ghosts.clear();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto& net = VersusNet::get();
        if (!net.racing() || net.levelID != m_level->m_levelID.value()) {
            if (!m_fields->ghosts.empty()) this->clearGhosts();
            return;
        }

        m_fields->posTimer += dt;
        if (m_fields->posTimer >= 0.1f) {
            m_fields->posTimer = 0.f;
            net.sendPos(
                m_player1->getPositionX(),
                m_player1->getPositionY(),
                m_player1->getRotation(),
                this->getCurrentPercent()
            );
        }

        for (auto& g : net.ghosts) {
            if (g.id == net.myId) continue;

            SimplePlayer* node = nullptr;
            auto it = m_fields->ghosts.find(g.id);
            if (it == m_fields->ghosts.end()) {
                node = SimplePlayer::create(1);
                node->setZOrder(1000);
                auto label = CCLabelBMFont::create(g.name.c_str(), "chatFont.fnt");
                label->setScale(0.6f);
                label->setPosition(ccp(0.f, 28.f));
                node->addChild(label);
                m_objectLayer->addChild(node);
                m_fields->ghosts[g.id] = node;
            } else {
                node = it->second;
            }
            node->setPosition(ccp(g.x, g.y));
            node->setRotation(g.rot);
            node->setVisible(!g.dead && !g.finished);
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        auto& net = VersusNet::get();
        if (net.racing() && net.levelID == m_level->m_levelID.value()) {
            net.sendFinish();
        }
    }
};
