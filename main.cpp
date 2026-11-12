#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <algorithm>
#include <random>
#include <sstream>
using namespace geode::prelude;

// ======================= Manual placement (editor playtest: Q / E / R) =======================
struct Item { const char* name; int id; bool pad; };
static const Item ITEMS[] = {
    {"Cube portal", 12, false}, {"Ship portal", 13, false}, {"Ball portal", 47, false},
    {"UFO portal", 111, false}, {"Wave portal", 660, false}, {"Robot portal", 745, false},
    {"Spider portal", 1331, false}, {"Swing portal", 1933, false},
    {"Gravity down portal", 10, false}, {"Gravity up portal", 11, false},
    {"Teleport portal (entry)", 747, false}, {"Teleport portal (exit)", 749, false},
    {"Yellow orb", 36, false}, {"Pink orb", 141, false}, {"Blue orb", 84, false},
    {"Green orb", 1022, false}, {"Black orb", 1330, false}, {"Red orb", 1333, false},
    {"Dash orb", 1704, false},
    {"Yellow pad", 35, true}, {"Pink pad", 140, true}, {"Blue pad", 67, true}, {"Red pad", 1332, true},
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

// Everything created while an editor playtest runs (by this mod or others such as Auto Layout).
static std::vector<Ref<GameObject>> g_created;

// ======================= Gimmicks =======================
// 1. Select objects in the editor and press "Save".
// 2. Playtest through the gimmick once. When you get past it alive, the mod records which click EFFECTS
//    happened (jump, gravity flip, wave hold/release, ship hold) and your exact route. The gimmick is then "verified".
//    Every new clean pass re-records the route (the latest tested route is the one that counts).
// 3. In later playtests, when the same effects happen in the same order and your route so far matches the tested
//    route, the gimmick is pasted. It is never pasted unverified, and never onto a route that does not match.
struct Gimmick {
    bool verified = false;
    std::string mode = "cube";
    bool mini = false;
    std::vector<std::string> types;   // effect sequence: kick, flip, hold, release
    float width = 30.f, x0 = 0.f, x1 = 0.f;
    std::vector<CCPoint> route;       // tested route (player centre) relative to the first effect
    std::string data;
};
static std::vector<Gimmick> g_gimmicks;

static std::vector<std::string> splitStr(std::string const& s, char d) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == d) { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    return out;
}

static void storeGimmicks() {
    std::string out;
    for (auto& g : g_gimmicks) {
        std::string types, route;
        for (size_t i = 0; i < g.types.size(); i++) types += (i ? "," : "") + g.types[i];
        for (auto& p : g.route) route += fmt::format("{:.1f},{:.1f};", p.x, p.y);
        out += fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}\n", g.verified ? 1 : 0, g.mode, g.mini ? 1 : 0, types, g.width, g.x0, g.x1, route, g.data);
    }
    Mod::get()->setSavedValue<std::string>("gimmicks2", out);
}
static void loadGimmicks() {
    g_gimmicks.clear();
    std::istringstream in(Mod::get()->getSavedValue<std::string>("gimmicks2", ""));
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f; size_t st = 0;
        for (int i = 0; i < 8; i++) { auto e = line.find('|', st); if (e == std::string::npos) break; f.push_back(line.substr(st, e - st)); st = e + 1; }
        if (f.size() != 8) continue;
        Gimmick g;
        g.verified = f[0] == "1"; g.mode = f[1]; g.mini = f[2] == "1";
        for (auto& t : splitStr(f[3], ',')) if (!t.empty()) g.types.push_back(t);
        g.width = static_cast<float>(std::atof(f[4].c_str()));
        g.x0 = static_cast<float>(std::atof(f[5].c_str()));
        g.x1 = static_cast<float>(std::atof(f[6].c_str()));
        for (auto& pt : splitStr(f[7], ';')) {
            auto c = pt.find(',');
            if (c == std::string::npos) continue;
            g.route.push_back(ccp(static_cast<float>(std::atof(pt.substr(0, c).c_str())), static_cast<float>(std::atof(pt.substr(c + 1).c_str()))));
        }
        g.data = line.substr(st);
        g_gimmicks.push_back(std::move(g));
    }
}
$execute { loadGimmicks(); }

static void saveSelectionAsGimmick(EditorUI* ui) {
    auto lel = LevelEditorLayer::get();
    if (!lel || !ui) return;
    std::vector<GameObject*> objs;
    if (auto sel = ui->getSelectedObjects()) {
        for (unsigned i = 0; i < sel->count(); i++) objs.push_back(static_cast<GameObject*>(sel->objectAtIndex(i)));
    }
    if (objs.empty() && ui->m_selectedObject) objs.push_back(ui->m_selectedObject);
    if (objs.empty()) {
        FLAlertLayer::create("Layout Placer", "Nothing selected. Select the objects of your gimmick first, then press Save.", "OK")->show();
        return;
    }
    std::string data; float minX = 1e9f, maxX = -1e9f; int count = 0;
    for (auto o : objs) {
        std::string s = o->getSaveString(lel);
        if (s.empty()) continue;
        if (s.back() != ';') s += ';';
        data += s; count++;
        minX = std::min(minX, o->getPositionX()); maxX = std::max(maxX, o->getPositionX());
    }
    if (data.empty()) {
        FLAlertLayer::create("Layout Placer", "Could not read the selected objects.", "OK")->show();
        return;
    }
    Gimmick g;
    g.width = maxX - minX + 30.f; g.x0 = minX; g.x1 = maxX; g.data = data;
    g_gimmicks.push_back(std::move(g));
    storeGimmicks();
    FLAlertLayer::create("Gimmick saved", fmt::format("Saved {} object(s) as gimmick #{}.\n\nNow playtest through it once. When you get past it alive, it is verified and can be pasted automatically.", count, g_gimmicks.size()), "OK")->show();
}

static std::mt19937 g_rng{std::random_device{}()};
static float rnd() { return std::uniform_real_distribution<float>(0.f, 1.f)(g_rng); }

// ---- player state / route ----
static std::vector<CCRect> g_path;      // hitbox swept per frame (to keep pasted blocks off the route)
static std::vector<CCPoint> g_pts;      // player centre per frame (to compare with tested routes)
static CCRect g_prevRect;
static bool g_hasPrev = false;

static char const* modeOf(PlayerObject* p) {
    if (p->m_isShip) return "ship";   if (p->m_isBall) return "ball";   if (p->m_isBird) return "ufo";
    if (p->m_isDart) return "wave";   if (p->m_isRobot) return "robot"; if (p->m_isSpider) return "spider";
    if (p->m_isSwing) return "swing"; return "cube";
}
static bool isMini(PlayerObject* p) { return p->m_vehicleSize < 0.8f; }

// Hitbox: normal 30, mini 0.6x, wave is a much smaller square (10, mini wave 6).
static CCRect playerRect(PlayerObject* p) {
    float size = (p->m_isDart ? 10.f : 30.f) * p->m_vehicleSize;
    auto pos = p->getPosition();
    return CCRect(pos.x - size / 2.f, pos.y - size / 2.f, size, size);
}
static CCRect rectUnion(CCRect const& a, CCRect const& b) {
    float x0 = std::min(a.getMinX(), b.getMinX()), y0 = std::min(a.getMinY(), b.getMinY());
    float x1 = std::max(a.getMaxX(), b.getMaxX()), y1 = std::max(a.getMaxY(), b.getMaxY());
    return CCRect(x0, y0, x1 - x0, y1 - y0);
}
static bool blocksPath(GameObject* o, float margin) {
    auto t = o->m_objectType;
    if (t != GameObjectType::Solid && t != GameObjectType::Hazard && t != GameObjectType::Slope) return false;
    CCRect r = o->getObjectRect();
    r.origin.x -= margin; r.origin.y -= margin; r.size.width += margin * 2.f; r.size.height += margin * 2.f;
    for (auto& p : g_path) if (p.intersectsRect(r)) return true;
    return false;
}

// ---- click effects ----
struct Ev { std::string type; CCPoint pos; float t; std::string mode; bool mini; };
struct Rec { bool active = false; std::vector<CCPoint> pts; std::vector<Ev> evs; };
struct Pending { size_t idx; CCPoint at; float stretch; float releaseX; };
static std::vector<Ev> g_events;
static std::vector<Rec> g_recs;
static std::vector<Pending> g_pending;
static float g_time = 0.f, g_lastPlacedX = -1e9f, g_lastX = 0.f;
static float g_lastPress = -10.f, g_lastRelease = -10.f;
static bool g_pendPress = false, g_pendRelease = false;
static CCPoint g_prevPos; static float g_prevVyN = 0.f; static bool g_prevUpside = false, g_havePrevState = false;

// A click only counts when it has an EFFECT shortly afterwards:
//   cube / ufo / robot : an upward kick (jump, orb)      ball / spider / swing : gravity flip
//   wave : goes up after pressing ("hold") or down after releasing ("release")      ship : starts rising ("hold")
static std::string detectEffect(PlayerObject* pl, float dt) {
    std::string type;
    auto pos = pl->getPosition();
    bool up = pl->m_isUpsideDown;
    float sign = up ? -1.f : 1.f;
    float vyN = (g_havePrevState && dt > 1e-5f) ? sign * (pos.y - g_prevPos.y) / dt : 0.f;
    if (g_pendPress && g_time - g_lastPress > 0.25f) g_pendPress = false;
    if (g_pendRelease && g_time - g_lastRelease > 0.25f) g_pendRelease = false;
    if (g_havePrevState) {
        std::string mode = modeOf(pl);
        if (mode == "ball" || mode == "spider" || mode == "swing") {
            if (g_pendPress && up != g_prevUpside) { type = "flip"; g_pendPress = false; }
        } else if (mode == "wave") {
            if (g_pendPress && vyN > 60.f) { type = "hold"; g_pendPress = false; }
            else if (g_pendRelease && vyN < -60.f) { type = "release"; g_pendRelease = false; }
        } else if (mode == "ship") {
            if (g_pendPress && ((vyN > 0.f && g_prevVyN <= 0.f) || (vyN - g_prevVyN) > 3.f)) { type = "hold"; g_pendPress = false; }
        } else {
            float kick = mode == "robot" ? 60.f : 200.f;
            if (g_pendPress && (vyN - g_prevVyN) > kick) { type = "kick"; g_pendPress = false; }
        }
    }
    g_prevPos = pos; g_prevVyN = vyN; g_prevUpside = up; g_havePrevState = true;
    return type;
}

static bool routeMatches(Gimmick const& g, CCPoint e0, float tol) {
    if (g.route.size() < 3) return false;
    int n = 0; float sum = 0.f, mx = 0.f;
    for (auto& p : g_pts) {
        float rx = p.x - e0.x, ry = p.y - e0.y;
        if (rx < g.route.front().x || rx > g.route.back().x) continue;
        auto it = std::lower_bound(g.route.begin(), g.route.end(), rx, [](CCPoint const& a, float v) { return a.x < v; });
        if (it == g.route.end()) continue;
        float d = std::fabs(it->y - ry);
        sum += d; mx = std::max(mx, d); n++;
    }
    if (n < 5) return false;
    return sum / n <= tol && mx <= tol * 3.f;
}

static void finalizeRecording(size_t i) {
    auto& g = g_gimmicks[i]; auto& r = g_recs[i];
    if (r.evs.empty()) {
        Notification::create(fmt::format("Gimmick #{}: no click effect found while passing it, not verified", i + 1), NotificationIcon::Warning, 1.5f)->show();
        return;
    }
    auto e0 = r.evs[0];
    g.types.clear();
    for (auto& e : r.evs) g.types.push_back(e.type);
    g.mode = e0.mode; g.mini = e0.mini;
    g.route.clear(); float lastX = -1e9f;
    for (auto& p : r.pts) {
        CCPoint rel = p - e0.pos;
        if (rel.x < -30.f || rel.x - lastX < 2.f) continue;
        g.route.push_back(rel); lastX = rel.x;
    }
    g.verified = true;
    storeGimmicks();
    Notification::create(fmt::format("Gimmick #{} verified ({} effect(s), route recorded)", i + 1, g.types.size()), NotificationIcon::Success, 1.5f)->show();
}

static int pasteGimmick(LevelEditorLayer* lel, Gimmick const& g, CCPoint at, float stretch) {
    if (!lel->m_editorUI) return 0;
    auto arr = lel->createObjectsFromString(g.data, true, false);
    if (!arr || arr->count() == 0) return 0;
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
    }
    float margin = static_cast<float>(Mod::get()->getSettingValue<double>("safety-margin"));
    std::vector<GameObject*> bad;
    for (unsigned i = 0; i < arr->count(); i++) {
        auto o = static_cast<GameObject*>(arr->objectAtIndex(i));
        if (blocksPath(o, margin)) bad.push_back(o);
    }
    for (auto o : bad) {
        g_created.erase(std::remove_if(g_created.begin(), g_created.end(), [o](Ref<GameObject> const& r) { return r.data() == o; }), g_created.end());
        lel->removeObject(o, true);
    }
    for (unsigned i = 0; i < arr->count(); i++) {
        auto o = static_cast<GameObject*>(arr->objectAtIndex(i));
        if (std::find(bad.begin(), bad.end(), o) == bad.end()) g_created.push_back(o);
    }
    return static_cast<int>(bad.size());
}

static void onEffect(PlayerObject* pl, std::string const& type) {
    Ev ev{type, pl->getPosition(), g_time, modeOf(pl), isMini(pl)};
    g_events.push_back(ev);
    while (g_events.size() > 16) g_events.erase(g_events.begin());
    for (size_t i = 0; i < g_recs.size() && i < g_gimmicks.size(); i++)
        if (g_recs[i].active && ev.pos.x >= g_gimmicks[i].x0 - 60.f) g_recs[i].evs.push_back(ev);

    if (!Mod::get()->getSettingValue<bool>("auto-paste")) return;
    float window = static_cast<float>(Mod::get()->getSettingValue<double>("window"));
    float gap = static_cast<float>(Mod::get()->getSettingValue<double>("min-gap"));
    float chance = static_cast<float>(Mod::get()->getSettingValue<double>("chance"));
    size_t best = 0; std::vector<size_t> cands;
    for (size_t i = 0; i < g_gimmicks.size(); i++) {
        auto& g = g_gimmicks[i];
        size_t n = g.types.size();
        if (!g.verified || n == 0 || n > g_events.size()) continue;
        size_t off = g_events.size() - n; bool ok = true;
        for (size_t k = 0; k < n; k++) if (g_events[off + k].type != g.types[k]) { ok = false; break; }
        if (!ok) continue;
        auto& first = g_events[off];
        if (first.mode != g.mode || first.mini != g.mini) continue;
        if (g_events.back().t - first.t > window) continue;
        if (first.pos.x - g_lastPlacedX < gap) continue;
        if (first.pos.x >= g.x0 - 60.f && first.pos.x <= g.x1 + 60.f) continue;   // that is the original gimmick itself
        if (n > best) { best = n; cands.clear(); }
        if (n == best) cands.push_back(i);
    }
    if (cands.empty() || rnd() > chance) return;
    size_t idx = cands[static_cast<size_t>(rnd() * cands.size()) % cands.size()];
    auto& g = g_gimmicks[idx];
    auto first = g_events[g_events.size() - g.types.size()];
    float stretch = Mod::get()->getSettingValue<bool>("stretch") ? 0.9f + 0.2f * rnd() : 1.f;
    g_pending.push_back({idx, first.pos, stretch, first.pos.x + g.width * stretch + 60.f});
    g_lastPlacedX = first.pos.x;
    g_events.clear();
}

static void onPlayerPress(PlayerObject* pl) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    g_pendPress = true; g_lastPress = g_time;
}
static void onPlayerRelease(PlayerObject* pl) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    g_pendRelease = true; g_lastRelease = g_time;
}

static void onPlayerUpdate(PlayerObject* pl, float dt) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    g_time += dt;
    float x = pl->getPositionX();
    if (g_recs.size() != g_gimmicks.size()) g_recs.assign(g_gimmicks.size(), Rec{});
    if (x < g_lastX - 100.f) {   // restart / death / checkpoint
        g_events.clear(); g_pending.clear(); g_lastPlacedX = -1e9f; g_path.clear(); g_pts.clear(); g_hasPrev = false;
        g_havePrevState = false; g_pendPress = g_pendRelease = false;
        for (auto& r : g_recs) { r.active = false; r.pts.clear(); r.evs.clear(); }
    }
    g_lastX = x;
    {
        CCRect cur = playerRect(pl);
        g_path.push_back(g_hasPrev ? rectUnion(g_prevRect, cur) : cur);
        g_pts.push_back(pl->getPosition());
        g_prevRect = cur; g_hasPrev = true;
        if (g_path.size() > 60000) { g_path.erase(g_path.begin(), g_path.begin() + 30000); g_pts.erase(g_pts.begin(), g_pts.begin() + 30000); }
    }
    // record / verify the original gimmicks while the player passes them
    for (size_t i = 0; i < g_gimmicks.size(); i++) {
        auto& g = g_gimmicks[i]; auto& r = g_recs[i];
        if (!r.active && x >= g.x0 - 60.f && x <= g.x1) { r.active = true; r.pts.clear(); r.evs.clear(); }
        if (r.active) {
            r.pts.push_back(pl->getPosition());
            if (x > g.x1 + 60.f) { finalizeRecording(i); r.active = false; r.pts.clear(); r.evs.clear(); }
        }
    }
    auto type = detectEffect(pl, dt);
    if (!type.empty()) onEffect(pl, type);

    float tol = static_cast<float>(Mod::get()->getSettingValue<double>("route-tolerance"));
    for (size_t i = 0; i < g_pending.size();) {
        if (x > g_pending[i].releaseX) {
            auto p = g_pending[i];
            g_pending.erase(g_pending.begin() + i);
            if (p.idx >= g_gimmicks.size()) continue;
            if (!routeMatches(g_gimmicks[p.idx], p.at, tol)) {
                Notification::create("Gimmick skipped: your route differs from the tested route", NotificationIcon::Warning, 1.f)->show();
                continue;
            }
            int removed = pasteGimmick(lel, g_gimmicks[p.idx], p.at, p.stretch);
            Notification::create(removed ? fmt::format("Gimmick pasted ({} object(s) left out: they touched your route)", removed) : std::string("Gimmick pasted"), NotificationIcon::Success, 1.f)->show();
        } else i++;
    }
}

class $modify(GimmickPlayer, PlayerObject) {
    bool pushButton(PlayerButton b) {
        bool r = PlayerObject::pushButton(b);
        if (b == PlayerButton::Jump) onPlayerPress(this);
        return r;
    }
    bool releaseButton(PlayerButton b) {
        bool r = PlayerObject::releaseButton(b);
        if (b == PlayerButton::Jump) onPlayerRelease(this);
        return r;
    }
    void update(float dt) {
        PlayerObject::update(dt);
        onPlayerUpdate(this, dt);
    }
};

// ======================= Keys (playtest) =======================
// Q = previous item,  E = next item,  R = place at the player.
class $modify(PlacerKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat) {
            auto lel = LevelEditorLayer::get();
            if (lel && lel->m_playbackMode != PlaybackMode::Playing) g_created.clear();
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

// Keep everything created during a playtest visible.
class $modify(TrackLevelEditor, LevelEditorLayer) {
    GameObject* createObject(int id, CCPoint pos, bool undo) {
        auto o = LevelEditorLayer::createObject(id, pos, undo);
        if (o && m_playbackMode == PlaybackMode::Playing && g_created.size() < 20000) g_created.push_back(o);
        return o;
    }
};

// Editor buttons: Placer (settings), Save (gimmick), Clear (delete all gimmicks).
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
        auto menu = CCMenu::create();
        menu->setPosition(ccp(0.f, 0.f));
        auto addBtn = [&](char const* label, float y, std::function<void()> fn) {
            auto spr = ButtonSprite::create(label);
            spr->setScale(0.6f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [fn](CCObject*) { fn(); });
            btn->setPosition(ccp(45.f, y));
            menu->addChild(btn);
        };
        addBtn("Placer", size.height - 110.f, []() { openSettingsPopup(Mod::get()); });
        addBtn("Save", size.height - 140.f, [this]() { saveSelectionAsGimmick(this); });
        addBtn("Clear", size.height - 170.f, []() {
            g_gimmicks.clear(); storeGimmicks();
            FLAlertLayer::create("Layout Placer", "All saved gimmicks were deleted.", "OK")->show();
        });
        this->addChild(menu, 100);
        return true;
    }
};
