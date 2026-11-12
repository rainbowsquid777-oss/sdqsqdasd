#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <random>
#include <sstream>
using namespace geode::prelude;

struct Item { const char* name; int id; bool pad; };

// Object IDs of GD 2.2. Add or remove lines freely.
static const Item ITEMS[] = {
    // game mode portals
    {"Cube portal", 12, false}, {"Ship portal", 13, false}, {"Ball portal", 47, false},
    {"UFO portal", 111, false}, {"Wave portal", 660, false}, {"Robot portal", 745, false},
    {"Spider portal", 1331, false}, {"Swing portal", 1933, false},
    // gravity / teleport
    {"Gravity down portal", 10, false}, {"Gravity up portal", 11, false},
    {"Teleport portal (entry)", 747, false}, {"Teleport portal (exit)", 749, false},
    // orbs
    {"Yellow orb", 36, false}, {"Pink orb", 141, false}, {"Blue orb", 84, false},
    {"Green orb", 1022, false}, {"Black orb", 1330, false}, {"Red orb", 1333, false},
    {"Dash orb", 1704, false},
    // pads
    {"Yellow pad", 35, true}, {"Pink pad", 140, true}, {"Blue pad", 67, true}, {"Red pad", 1332, true},
    // gimmicks
    {"Mini portal", 101, false}, {"Normal size portal", 99, false},
    {"Dual portal", 286, false}, {"Single portal", 287, false},
    {"Mirror on", 45, false}, {"Mirror off", 46, false},
    {"Speed 0.5x", 200, false}, {"Speed 1x", 201, false}, {"Speed 2x", 202, false},
    {"Speed 3x", 203, false}, {"Speed 4x", 1334, false},
};
static constexpr int COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);
static int g_sel = 0;

static void announce(char const* prefix) {
    Notification::create(fmt::format("{}: {}", prefix, ITEMS[g_sel].name), NotificationIcon::None, 0.8f)->show();
}


// Every object created while an editor playtest is running (by this mod or by other mods such as Auto Layout).
static std::vector<Ref<GameObject>> g_created;

// ---------------- Gimmicks ----------------
// Select objects in the editor and press G: they are saved as a gimmick for N clicks (settings).
// During a playtest, when your last N clicks (within the click window, in the right gamemode) match,
// the gimmick is pasted. The LEFTMOST object of the gimmick is put where the player was at the first click.
struct Gimmick { int clicks; std::string mode; float width; std::string data; };
static std::vector<Gimmick> g_gimmicks;

static void storeGimmicks() {
    std::string out;
    for (auto& g : g_gimmicks) out += fmt::format("{}|{}|{}|{}\n", g.clicks, g.mode, g.width, g.data);
    Mod::get()->setSavedValue<std::string>("gimmicks", out);
}
static void loadGimmicks() {
    g_gimmicks.clear();
    std::istringstream in(Mod::get()->getSavedValue<std::string>("gimmicks", ""));
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f; size_t st = 0;
        for (int i = 0; i < 3; i++) { auto e = line.find('|', st); if (e == std::string::npos) break; f.push_back(line.substr(st, e - st)); st = e + 1; }
        if (f.size() != 3) continue;
        g_gimmicks.push_back({std::atoi(f[0].c_str()), f[1], static_cast<float>(std::atof(f[2].c_str())), line.substr(st)});
    }
}
$execute { loadGimmicks(); }

static void saveSelectionAsGimmick(LevelEditorLayer* lel) {
    if (!lel->m_editorUI) return;
    auto sel = lel->m_editorUI->getSelectedObjects();
    if (!sel || sel->count() == 0) return;
    std::string data; float minX = 1e9f, maxX = -1e9f;
    for (unsigned i = 0; i < sel->count(); i++) {
        auto o = static_cast<GameObject*>(sel->objectAtIndex(i));
        std::string s = o->getSaveString(lel);
        if (s.empty()) continue;
        if (s.back() != ';') s += ';';
        data += s;
        minX = std::min(minX, o->getPositionX()); maxX = std::max(maxX, o->getPositionX());
    }
    if (data.empty()) return;
    int clicks = static_cast<int>(Mod::get()->getSettingValue<int64_t>("new-clicks"));
    g_gimmicks.push_back({clicks, Mod::get()->getSettingValue<std::string>("new-mode"), maxX - minX + 30.f, data});
    storeGimmicks();
    Notification::create(fmt::format("Gimmick saved ({} click(s)), total {}", clicks, g_gimmicks.size()), NotificationIcon::Success, 1.2f)->show();
}

static std::mt19937 g_rng{std::random_device{}()};
static float rnd() { return std::uniform_real_distribution<float>(0.f, 1.f)(g_rng); }

struct Click { float x, y, t; };
struct Pending { size_t idx; CCPoint at; float stretch; float releaseX; };
static std::vector<Click> g_clicks;
static std::vector<Pending> g_pending;
static float g_time = 0.f, g_lastPlacedX = -1e9f, g_lastX = 0.f;

static char const* modeOf(PlayerObject* p) {
    if (p->m_isShip) return "ship";   if (p->m_isBall) return "ball";   if (p->m_isBird) return "ufo";
    if (p->m_isDart) return "wave";   if (p->m_isRobot) return "robot"; if (p->m_isSpider) return "spider";
    if (p->m_isSwing) return "swing"; return "cube";
}

static void pasteGimmick(LevelEditorLayer* lel, Gimmick const& g, CCPoint at, float stretch) {
    if (!lel->m_editorUI) return;
    auto arr = lel->createObjectsFromString(g.data, true, false);
    if (!arr || arr->count() == 0) return;
    GameObject* anchor = nullptr;
    for (unsigned i = 0; i < arr->count(); i++) {
        auto o = static_cast<GameObject*>(arr->objectAtIndex(i));
        if (!anchor || o->getPositionX() < anchor->getPositionX() ||
            (o->getPositionX() == anchor->getPositionX() && o->getPositionY() < anchor->getPositionY())) anchor = o;
    }
    CCPoint a = anchor->getPosition();
    for (unsigned i = 0; i < arr->count(); i++) {
        auto o = static_cast<GameObject*>(arr->objectAtIndex(i));
        CCPoint p = o->getPosition();
        CCPoint target = ccp(at.x + (p.x - a.x) * stretch, at.y + (p.y - a.y));
        lel->m_editorUI->moveObject(o, target - p);
        g_created.push_back(o);
    }
}

static void onPlayerClick(PlayerObject* pl) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    if (!Mod::get()->getSettingValue<bool>("auto-paste") || g_gimmicks.empty()) return;
    g_clicks.push_back({pl->getPositionX(), pl->getPositionY(), g_time});
    while (g_clicks.size() > 12) g_clicks.erase(g_clicks.begin());
    float window = static_cast<float>(Mod::get()->getSettingValue<double>("window"));
    float gap = static_cast<float>(Mod::get()->getSettingValue<double>("min-gap"));
    float chance = static_cast<float>(Mod::get()->getSettingValue<double>("chance"));
    std::string cur = modeOf(pl);
    int best = 0; std::vector<size_t> cands;
    for (size_t i = 0; i < g_gimmicks.size(); i++) {
        auto& g = g_gimmicks[i];
        if (g.clicks > static_cast<int>(g_clicks.size())) continue;
        if (g.mode != "any" && g.mode != cur) continue;
        if (g_time - g_clicks[g_clicks.size() - g.clicks].t > window) continue;
        if (g_clicks[g_clicks.size() - g.clicks].x - g_lastPlacedX < gap) continue;
        if (g.clicks > best) { best = g.clicks; cands.clear(); }
        if (g.clicks == best) cands.push_back(i);
    }
    if (cands.empty() || rnd() > chance) return;
    size_t idx = cands[static_cast<size_t>(rnd() * cands.size()) % cands.size()];
    auto& g = g_gimmicks[idx];
    auto first = g_clicks[g_clicks.size() - g.clicks];
    float stretch = Mod::get()->getSettingValue<bool>("stretch") ? 0.8f + 0.4f * rnd() : 1.f;
    // Pasted only after the player has passed it, so it can never hit the player on the spot.
    g_pending.push_back({idx, ccp(first.x, first.y), stretch, first.x + g.width * stretch + 60.f});
    g_lastPlacedX = first.x;
    g_clicks.clear();
}

static void onPlayerUpdate(PlayerObject* pl, float dt) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    g_time += dt;
    float x = pl->getPositionX();
    if (x < g_lastX - 100.f) { g_clicks.clear(); g_pending.clear(); g_lastPlacedX = -1e9f; } // restart / checkpoint
    g_lastX = x;
    for (size_t i = 0; i < g_pending.size();) {
        if (x > g_pending[i].releaseX) {
            if (g_pending[i].idx < g_gimmicks.size()) pasteGimmick(lel, g_gimmicks[g_pending[i].idx], g_pending[i].at, g_pending[i].stretch);
            g_pending.erase(g_pending.begin() + i);
        } else i++;
    }
}

class $modify(GimmickPlayer, PlayerObject) {
    bool pushButton(PlayerButton b) {
        bool r = PlayerObject::pushButton(b);
        if (b == PlayerButton::Jump) onPlayerClick(this);
        return r;
    }
    void update(float dt) {
        PlayerObject::update(dt);
        onPlayerUpdate(this, dt);
    }
};

// Keys during an editor playtest:  Q = previous item,  E = next item,  R = place at the player.
class $modify(PlacerKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat) {
            auto lel = LevelEditorLayer::get();
            if (lel && lel->m_playbackMode != PlaybackMode::Playing) {
                g_created.clear();
                if (key == KEY_G) saveSelectionAsGimmick(lel);
                else if (key == KEY_H) { g_gimmicks.clear(); storeGimmicks(); Notification::create("All gimmicks cleared", NotificationIcon::None, 0.8f)->show(); }
            }
            if (lel && lel->m_playbackMode == PlaybackMode::Playing && lel->m_player1) {
                if (key == KEY_Q) { g_sel = (g_sel + COUNT - 1) % COUNT; announce("Selected"); }
                else if (key == KEY_E) { g_sel = (g_sel + 1) % COUNT; announce("Selected"); }
                else if (key == KEY_R) {
                    auto p = lel->m_player1->getPosition();
                    p.x += static_cast<float>(Mod::get()->getSettingValue<double>("offset-x"));
                    if (ITEMS[g_sel].pad) p.y -= 9.f;
                    if (auto o = lel->createObject(ITEMS[g_sel].id, p, false)) g_created.push_back(o);
                    announce("Placed");
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// Track everything created during a playtest and keep it visible.
class $modify(TrackLevelEditor, LevelEditorLayer) {
    GameObject* createObject(int id, CCPoint pos, bool undo) {
        auto o = LevelEditorLayer::createObject(id, pos, undo);
        if (o && m_playbackMode == PlaybackMode::Playing && g_created.size() < 20000) g_created.push_back(o);
        return o;
    }
};

// Button in the editor that opens this mod's settings.
class $modify(PlacerEditorUI, EditorUI) {
    void keepGeneratedVisible(float) {
        auto lel = LevelEditorLayer::get();
        if (!lel || lel->m_playbackMode != PlaybackMode::Playing) return;
        if (!Mod::get()->getSettingValue<bool>("show-generated")) return;
        for (auto& o : g_created) if (o) o->setVisible(true);
    }
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;
        this->schedule(schedule_selector(PlacerEditorUI::keepGeneratedVisible));
        auto size = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("Placer");
        spr->setScale(0.6f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCObject*) { openSettingsPopup(Mod::get()); });
        auto menu = CCMenu::create();
        menu->setPosition(ccp(0.f, 0.f));
        btn->setPosition(ccp(40.f, size.height - 110.f));
        menu->addChild(btn);
        this->addChild(menu, 100);
        return true;
    }
};
