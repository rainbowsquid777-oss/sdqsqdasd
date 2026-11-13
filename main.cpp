#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
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
    {"Gimmick START marker", -1, false}, {"Gimmick END marker", -2, false},   // ids come from the settings
};
static constexpr int COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);
static int g_sel = 0;

static void announce(char const* prefix) {
    Notification::create(fmt::format("{}: {}", prefix, ITEMS[g_sel].name), NotificationIcon::None, 0.8f)->show();
}

// Everything created while an editor playtest runs (by this mod or others such as Auto Layout).
static std::vector<Ref<GameObject>> g_created;
static std::vector<Ref<GameObject>> g_hidden;   // orbs pasted by the gimmick system: kept invisible during playtests (setting "hide-orbs")

// ======================= Gimmicks =======================
// 1. Playtest through the part of the level where you want the gimmick. The mod keeps the route you flew and the
//    click EFFECTS that happened (jump, gravity flip, wave hold/release, ship hold) - no extra proof run is needed.
// 2. Back in the editor, select the objects of the gimmick and press "Save". The objects are saved together with
//    your most recent route that passed through them (and the click effects along it).
// 3. In later playtests, when the same effects happen in the same order and your route so far matches the saved
//    route, the gimmick is pasted - never onto a route that does not match.
// Gimmicks are stored relative to YOUR gravity: a block under you when you saved it is pasted under you again, and
// mirrored upside down if your gravity is flipped now. Blocks you stand on / land on are kept.
struct Gimmick {
    bool verified = false;
    std::string mode = "cube";
    bool mini = false;
    bool up = false;                  // gravity was upside down when it was saved (everything below is stored gravity-relative)
    std::vector<std::string> types;   // effect sequence: kick, flip, hold, release (= number of effects = number of counted clicks)
    std::vector<float> evx;           // X distance of every effect from the first one (click timing); empty in gimmicks saved by older versions
    float width = 30.f, x0 = 0.f, x1 = 0.f, ay = 0.f;
    CCPoint off = ccp(0.f, 0.f);      // leftmost object relative to the first click effect (y is gravity-relative)
    std::vector<CCPoint> route;       // saved route (player centre) relative to the first effect (y is gravity-relative)
    std::string data;
    mutable int hasOrb = -1;          // cache: does the gimmick contain a click-activated object (orb)?
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
        std::string types, route, evx;
        for (size_t i = 0; i < g.types.size(); i++) types += (i ? "," : "") + g.types[i];
        for (size_t i = 0; i < g.evx.size(); i++) evx += fmt::format("{}{:.1f}", i ? "," : "", g.evx[i]);
        for (auto& p : g.route) route += fmt::format("{:.1f},{:.1f};", p.x, p.y);
        out += fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}\n", g.verified ? 1 : 0, g.mode, g.mini ? 1 : 0, g.up ? 1 : 0, types, evx, g.width, g.x0, g.x1, g.ay, g.off.x, g.off.y, route, g.data);
    }
    Mod::get()->setSavedValue<std::string>("gimmicks5", out);
    Mod::get()->setSavedValue<std::string>("gimmicks4", "");   // older formats are replaced by this one
    Mod::get()->setSavedValue<std::string>("gimmicks3", "");
}
static void parseGimmicks(std::string const& text, int ver) {   // ver 3: no gravity, 4: gravity, 5: gravity + click timing
    std::istringstream in(text);
    std::string line;
    int nf = ver >= 5 ? 13 : (ver == 4 ? 12 : 11);
    while (std::getline(in, line)) {
        std::vector<std::string> f; size_t st = 0;
        for (int i = 0; i < nf; i++) { auto e = line.find('|', st); if (e == std::string::npos) break; f.push_back(line.substr(st, e - st)); st = e + 1; }
        if (static_cast<int>(f.size()) != nf) continue;
        size_t k = 0;
        Gimmick g;
        g.verified = f[k++] == "1"; g.mode = f[k++]; g.mini = f[k++] == "1";
        if (ver >= 4) g.up = f[k++] == "1";
        for (auto& t : splitStr(f[k++], ',')) if (!t.empty()) g.types.push_back(t);
        if (ver >= 5) for (auto& t : splitStr(f[k++], ',')) if (!t.empty()) g.evx.push_back(static_cast<float>(std::atof(t.c_str())));
        g.width = static_cast<float>(std::atof(f[k++].c_str()));
        g.x0 = static_cast<float>(std::atof(f[k++].c_str()));
        g.x1 = static_cast<float>(std::atof(f[k++].c_str()));
        g.ay = static_cast<float>(std::atof(f[k++].c_str()));
        float ox = static_cast<float>(std::atof(f[k++].c_str()));
        float oy = static_cast<float>(std::atof(f[k++].c_str()));
        g.off = ccp(ox, oy);
        for (auto& pt : splitStr(f[k++], ';')) {
            auto c = pt.find(',');
            if (c == std::string::npos) continue;
            g.route.push_back(ccp(static_cast<float>(std::atof(pt.substr(0, c).c_str())), static_cast<float>(std::atof(pt.substr(c + 1).c_str()))));
        }
        g.data = line.substr(st);
        g_gimmicks.push_back(std::move(g));
    }
}
static void loadGimmicks() {
    g_gimmicks.clear();
    auto v5 = Mod::get()->getSavedValue<std::string>("gimmicks5", "");
    if (!v5.empty()) { parseGimmicks(v5, 5); return; }
    auto v4 = Mod::get()->getSavedValue<std::string>("gimmicks4", "");
    if (!v4.empty()) { parseGimmicks(v4, 4); return; }
    parseGimmicks(Mod::get()->getSavedValue<std::string>("gimmicks3", ""), 3);   // saved by an older version (gravity down)
}
$execute { loadGimmicks(); }

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
// Spikes / slopes that come close to the route are left out. A solid block is only left out when it is really in the
// way (the route runs into or through it). A block that the route merely touches from above or below (the one you
// land on / stand on / hang from, in YOUR gravity) stays.
static bool blocksPath(GameObject* o, float margin) {
    auto t = o->m_objectType;
    bool solid = t == GameObjectType::Solid;
    if (!solid && t != GameObjectType::Hazard && t != GameObjectType::Slope) return false;
    CCRect r = o->getObjectRect();
    if (!solid) {
        r.origin.x -= margin; r.origin.y -= margin; r.size.width += margin * 2.f; r.size.height += margin * 2.f;
        for (auto& p : g_path) if (p.intersectsRect(r)) return true;
        return false;
    }
    for (auto& p : g_path) {
        float ox = std::min(p.getMaxX(), r.getMaxX()) - std::max(p.getMinX(), r.getMinX());
        float oy = std::min(p.getMaxY(), r.getMaxY()) - std::max(p.getMinY(), r.getMinY());
        if (ox > -margin && oy > 4.f) return true;   // vertical contact (oy <= 4) = standing on it / touching it: fine
    }
    return false;
}

// ---- click effects ----
struct Ev { std::string type; CCPoint pos; float t; std::string mode; bool mini; bool up; };
struct Run { int presses = 0; std::vector<CCPoint> pts; std::vector<Ev> evs; };   // one playtest attempt: route + click effects
struct Span { float l, r; };
struct Pending { size_t idx; CCPoint at; float stretch; float releaseX; bool up; Span sp; std::vector<Ref<GameObject>> early; };

// Click-activated objects (orbs). These are pasted IMMEDIATELY at the click; the rest of the gimmick follows after the route check.
static bool isOrbId(int id) {
    switch (id) {
        case 36: case 84: case 141: case 1022: case 1330: case 1333: case 1704: case 1751: case 1594: case 3004: return true;
        default: return false;
    }
}
static bool isClickObject(GameObject* o) { return isOrbId(o->m_objectID); }
static bool gimmickHasOrb(Gimmick const& g) {
    if (g.hasOrb >= 0) return g.hasOrb == 1;
    g.hasOrb = 0;
    for (auto& obj : splitStr(g.data, ';')) {
        auto f = splitStr(obj, ',');
        for (size_t i = 0; i + 1 < f.size(); i += 2) if (f[i] == "1" && isOrbId(std::atoi(f[i + 1].c_str()))) { g.hasOrb = 1; return true; }
    }
    return false;
}
// An orb placed AHEAD of the player while he is falling along a route that looks like the start of a saved gimmick.
// If he clicks it, the gimmick goes on as usual; if he does not, he simply flies past and the orb is taken back.
struct Pred { size_t idx; CCPoint at; float stretch; bool up; Span sp; std::vector<Ref<GameObject>> early; };
static std::vector<Pred> g_pred;
static std::vector<Ev> g_events;

// ---- room check: the X range a gimmick will occupy is computed BEFORE it is pasted ----
static std::vector<Span> g_reserved;   // X ranges of gimmicks that are queued / pasted in this run
static void releaseReserved(Span s) {
    for (size_t i = 0; i < g_reserved.size(); i++) if (g_reserved[i].l == s.l && g_reserved[i].r == s.r) { g_reserved.erase(g_reserved.begin() + i); return; }
}
static void dbgNote(std::string const& s);
static Span gimmickSpan(Gimmick const& g, CCPoint e0, float stretch) {
    float l = e0.x + g.off.x - 15.f;                                       // leftmost object (a block is 30 wide)
    float r = e0.x + g.off.x + (g.x1 - g.x0) * stretch + 15.f;             // rightmost object
    return {l, r};
}
// X distance from the span to the nearest other object of the level (0 = overlapping in X).
static float nearestObjectGap(LevelEditorLayer* lel, Span s, bool withDeco, std::vector<Ref<GameObject>> const* skip = nullptr) {
    float best = 1e9f;
    auto objs = lel->m_objects;
    if (!objs) return best;
    for (unsigned i = 0; i < objs->count(); i++) {
        auto o = static_cast<GameObject*>(objs->objectAtIndex(i));
        if (!o) continue;
        if (skip && std::any_of(skip->begin(), skip->end(), [o](Ref<GameObject> const& r) { return r.data() == o; })) continue;
        if (!withDeco && o->m_objectType == GameObjectType::Decoration) continue;
        CCRect r = o->getObjectRect();
        float gap = std::max(r.getMinX() - s.r, s.l - r.getMaxX());
        best = std::min(best, gap < 0.f ? 0.f : gap);
    }
    return best;
}
static std::vector<Run> g_runs;   // finished attempts (newest last)
static Run g_cur;                 // the attempt in progress / the last one
static bool g_newRunPending = false, g_wasPlaying = false;
static std::vector<Pending> g_pending;
static float g_time = 0.f, g_lastPlacedX = -1e9f, g_lastX = 0.f;
static float g_lastPress = -10.f, g_lastRelease = -10.f;
static bool g_pendPress = false, g_pendRelease = false;
static CCPoint g_prevPos; static float g_prevVyN = 0.f; static bool g_prevUpside = false, g_havePrevState = false;

// A click only counts when it has an EFFECT shortly afterwards:
//   cube / ufo / robot : an upward kick (jump, orb)      ball / spider / swing : gravity flip
//   wave : goes up after pressing ("hold") or down after releasing ("release")      ship : starts rising ("hold")
static float g_dbgMaxKick = 0.f;
static bool g_falling = false;
static float g_prevR = 0.f, g_pressR = 0.f;   // vertical slope (dy per dx) at the last update / when the button was pressed

// Wall-clock seconds. (The dt passed to PlayerObject::update is not guaranteed to be seconds, so it is not used for timing.)
static float nowSeconds() {
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}
static bool debugOn() { return Mod::get()->getSettingValue<bool>("debug"); }
static void dbgNote(std::string const& s) { if (debugOn()) Notification::create(s, NotificationIcon::Warning, 1.2f)->show(); }

// Vertical motion is measured as a SLOPE (dy per dx) between two consecutive updates. That does not depend on the
// time step, the frame rate or the game speed: a jump / orb kicks the slope up by a lot in one step, gravity only
// changes it a little per step.
static std::string detectEffect(PlayerObject* pl) {
    std::string type;
    auto pos = pl->getPosition();
    bool up = pl->m_isUpsideDown;
    std::string mode = modeOf(pl);
    if (!g_havePrevState) { g_prevPos = pos; g_prevUpside = up; g_prevR = 0.f; g_falling = false; g_havePrevState = true; return type; }
    if (g_pendPress && g_time - g_lastPress > 0.3f) g_pendPress = false;
    if (g_pendRelease && g_time - g_lastRelease > 0.3f) g_pendRelease = false;

    if (up != g_prevUpside) {   // gravity flipped
        if (g_pendPress && (mode == "ball" || mode == "spider" || mode == "swing")) { type = "flip"; g_pendPress = false; }
        g_prevUpside = up; g_prevPos = pos; g_prevR = 0.f; g_falling = false;
        return type;
    }
    float dx = pos.x - g_prevPos.x;
    if (dx < 0.02f) return type;   // no horizontal movement since the last sample: wait for the next one
    float sign = up ? -1.f : 1.f;
    float r = sign * (pos.y - g_prevPos.y) / dx;
    float dr = r - g_prevR;
    if (g_pendPress) g_dbgMaxKick = std::max(g_dbgMaxKick, dr);

    if (mode == "wave") {
        if (g_pendPress && r > 0.5f) { type = "hold"; g_pendPress = false; }
        else if (g_pendRelease && r < -0.5f) { type = "release"; g_pendRelease = false; }
    } else if (mode == "ship") {
        if (g_pendPress && r - g_pressR > 0.25f) { type = "hold"; g_pendPress = false; }
    } else if (mode != "ball" && mode != "spider" && mode != "swing") {
        float kick = mode == "robot" ? 0.12f : 0.3f;
        if (g_pendPress && dr > kick && r > 0.05f) { type = "kick"; g_pendPress = false; }
    }
    // "fall": a cube / ufo / robot starts falling (off an edge or after the top of a jump).
    if (mode == "cube" || mode == "ufo" || mode == "robot") {
        if (g_falling) { if (r > -0.1f) g_falling = false; }
        else if (r < -0.3f) { g_falling = true; if (type.empty()) type = "fall"; }
    } else g_falling = false;
    g_prevPos = pos; g_prevR = r; g_prevUpside = up;
    return type;
}

static bool routeMatches(Gimmick const& g, CCPoint e0, float tol, bool curUp, float* outAvg = nullptr, float* outMax = nullptr, int* outN = nullptr) {
    if (g.route.size() < 3) return false;
    float s = curUp ? -1.f : 1.f;   // compare in gravity-relative terms
    int n = 0; float sum = 0.f, mx = 0.f;
    for (auto& p : g_pts) {
        float rx = p.x - e0.x, ry = s * (p.y - e0.y);
        if (rx < g.route.front().x || rx > g.route.back().x) continue;
        auto it = std::lower_bound(g.route.begin(), g.route.end(), rx, [](CCPoint const& a, float v) { return a.x < v; });
        if (it == g.route.end()) continue;
        float d = std::fabs(it->y - ry);
        sum += d; mx = std::max(mx, d); n++;
    }
    if (outN) *outN = n;
    if (n < 5) return false;
    if (outAvg) *outAvg = sum / n;
    if (outMax) *outMax = mx;
    return sum / n <= tol && mx <= tol * 3.f;
}

// phase 1 = only the click-activated objects (orbs), phase 2 = everything except them, 0 = everything.
static int pasteGimmick(LevelEditorLayer* lel, Gimmick const& g, CCPoint e0, float stretch, bool curUp, int phase = 0, std::vector<Ref<GameObject>>* keptOut = nullptr, bool allOrNothing = false) {
    if (!lel->m_editorUI) return 0;
    auto arr = lel->createObjectsFromString(g.data, true, false);
    if (!arr || arr->count() == 0) return 0;
    std::vector<GameObject*> objs; std::vector<CCPoint> saved;
    for (unsigned i = 0; i < arr->count(); i++) {
        auto o = static_cast<GameObject*>(arr->objectAtIndex(i));
        objs.push_back(o); saved.push_back(o->getPosition());
    }
    size_t ai = 0;
    for (size_t i = 1; i < objs.size(); i++)
        if (saved[i].x < saved[ai].x || (saved[i].x == saved[ai].x && saved[i].y < saved[ai].y)) ai = i;
    CCPoint a = saved[ai];
    float sSaved = g.up ? -1.f : 1.f, sCur = curUp ? -1.f : 1.f;
    bool flip = g.up != curUp;   // saved with the other gravity: mirror the gimmick so it is "below" you again
    for (size_t i = 0; i < objs.size(); i++) {
        auto o = objs[i];
        float relY = g.off.y + sSaved * (saved[i].y - a.y);   // gravity-relative height above the first click effect
        CCPoint target = ccp(e0.x + g.off.x + (saved[i].x - a.x) * stretch, e0.y + sCur * relY);
        if (flip) lel->m_editorUI->transformObject(o, EditCommand::FlipY, false);
        lel->m_editorUI->moveObject(o, target - o->getPosition());
    }
    if (phase != 0) {
        std::vector<GameObject*> keep;
        for (auto o : objs) {
            bool click = isClickObject(o);
            if ((phase == 1) == click) keep.push_back(o);
            else {
                g_created.erase(std::remove_if(g_created.begin(), g_created.end(), [o](Ref<GameObject> const& r) { return r.data() == o; }), g_created.end());
                lel->removeObject(o, true);
            }
        }
        objs = keep;
    }
    float margin = static_cast<float>(Mod::get()->getSettingValue<double>("safety-margin"));
    std::vector<GameObject*> bad;
    if (phase != 1) for (auto o : objs) if (blocksPath(o, margin)) bad.push_back(o);
    if (allOrNothing && !bad.empty()) {   // the gimmick is never changed: either it goes in exactly as saved, or not at all
        for (auto o : objs) {
            g_created.erase(std::remove_if(g_created.begin(), g_created.end(), [o](Ref<GameObject> const& r) { return r.data() == o; }), g_created.end());
            lel->removeObject(o, true);
        }
        return static_cast<int>(bad.size());
    }
    for (auto o : bad) {
        g_created.erase(std::remove_if(g_created.begin(), g_created.end(), [o](Ref<GameObject> const& r) { return r.data() == o; }), g_created.end());
        lel->removeObject(o, true);
    }
    for (auto o : objs) if (std::find(bad.begin(), bad.end(), o) == bad.end()) {
        g_created.push_back(o);
        if (keptOut) keptOut->push_back(o);
        if (phase != 2 && isClickObject(o) && g_hidden.size() < 20000 && Mod::get()->getSettingValue<bool>("hide-orbs")) g_hidden.push_back(o);
    }
    return static_cast<int>(bad.size());
}

static void removeList(LevelEditorLayer* lel, std::vector<Ref<GameObject>>& list) {
    for (auto& r : list) {
        GameObject* o = r.data();
        if (!o) continue;
        g_created.erase(std::remove_if(g_created.begin(), g_created.end(), [o](Ref<GameObject> const& q) { return q.data() == o; }), g_created.end());
        g_hidden.erase(std::remove_if(g_hidden.begin(), g_hidden.end(), [o](Ref<GameObject> const& q) { return q.data() == o; }), g_hidden.end());
        if (lel) lel->removeObject(o, true);
    }
    list.clear();
}
static void removeEarly(LevelEditorLayer* lel, Pending& p) { removeList(lel, p.early); }

static void onEffect(PlayerObject* pl, std::string const& type) {
    Ev ev{type, pl->getPosition(), g_time, modeOf(pl), isMini(pl), pl->m_isUpsideDown};
    g_events.push_back(ev);
    while (g_events.size() > 16) g_events.erase(g_events.begin());
    if (g_cur.evs.size() < 3000) g_cur.evs.push_back(ev);
    if (debugOn()) Notification::create(fmt::format("effect: {} ({}{})", type, ev.mode, ev.mini ? ", mini" : ""), NotificationIcon::None, 0.5f)->show();

    if (!Mod::get()->getSettingValue<bool>("auto-paste")) return;
    // The effect window is set in FRAMES; the frame rate setting converts it to seconds.
    float fps = static_cast<float>(std::max<int64_t>(1, Mod::get()->getSettingValue<int64_t>("fps")));
    float windowFrames = static_cast<float>(Mod::get()->getSettingValue<int64_t>("window-frames"));
    float window = windowFrames / fps;
    float gap = static_cast<float>(Mod::get()->getSettingValue<double>("min-gap"));
    float chance = static_cast<float>(Mod::get()->getSettingValue<double>("chance"));
    bool matchTiming = Mod::get()->getSettingValue<bool>("match-timing");
    float timingTol = static_cast<float>(Mod::get()->getSettingValue<double>("timing-tolerance"));
    size_t best = 0; std::vector<size_t> cands;
    for (size_t i = 0; i < g_gimmicks.size(); i++) {
        auto& g = g_gimmicks[i];
        size_t n = g.types.size();
        if (!g.verified || n == 0 || n > g_events.size()) continue;
        size_t off = g_events.size() - n; bool ok = true;
        for (size_t k = 0; k < n; k++) if (g_events[off + k].type != g.types[k]) { ok = false; break; }
        if (!ok) continue;
        auto& first = g_events[off];
        if (first.mode != g.mode || first.mini != g.mini) { dbgNote(fmt::format("Gimmick #{} not used: mode differs (saved {}{}, now {}{})", i + 1, g.mode, g.mini ? " mini" : "", first.mode, first.mini ? " mini" : "")); continue; }
        if (matchTiming && g.evx.size() == n) {   // click timing: every effect must come at about the same distance after the first one as when it was saved
            bool tOk = true;
            for (size_t k = 1; k < n; k++) if (std::fabs((g_events[off + k].pos.x - first.pos.x) - g.evx[k]) > timingTol) { tOk = false; break; }
            if (!tOk) { dbgNote(fmt::format("Gimmick #{} not used: click timing differs from the saved gimmick", i + 1)); continue; }
        }
        if (g_events.back().t - first.t > window) { dbgNote(fmt::format("Gimmick #{} not used: effects took {:.0f} frames, the Effect window is {:.0f}", i + 1, (g_events.back().t - first.t) * fps, windowFrames)); continue; }
        if (first.pos.x - g_lastPlacedX < gap) { dbgNote(fmt::format("Gimmick #{} not used: too close to the previous gimmick (Minimum gap)", i + 1)); continue; }
        if (first.pos.x >= g.x0 - g.off.x - 20.f && first.pos.x <= g.x1 + 60.f) continue;   // that is the original gimmick itself
        if (n > best) { best = n; cands.clear(); }
        if (n == best) cands.push_back(i);
    }
    if (cands.empty()) return;
    // An orb was already placed ahead of you for this gimmick and you clicked it: keep it and carry on with that gimmick.
    for (size_t pi = 0; pi < g_pred.size(); pi++) {
        auto& pr = g_pred[pi];
        if (std::find(cands.begin(), cands.end(), pr.idx) == cands.end()) continue;
        auto& pg = g_gimmicks[pr.idx];
        auto pf = g_events[g_events.size() - pg.types.size()];
        if (std::fabs(pf.pos.x - (pr.at.x + pg.off.x)) > 70.f) continue;
        Pending np{pr.idx, pr.at, pr.stretch, pr.at.x + pg.off.x + pg.width * pr.stretch + 60.f, pr.up, pr.sp, std::move(pr.early)};
        g_pending.push_back(std::move(np));
        g_pred.erase(g_pred.begin() + pi);
        g_lastPlacedX = pf.pos.x;
        g_events.clear();
        return;
    }
    if (rnd() > chance) { dbgNote("Gimmick not used: Paste chance said no"); return; }
    size_t idx = cands[static_cast<size_t>(rnd() * cands.size()) % cands.size()];
    auto& g = g_gimmicks[idx];
    auto first = g_events[g_events.size() - g.types.size()];
    float stretch = 1.f;   // saved gimmicks are never stretched or changed
    float spacing = static_cast<float>(Mod::get()->getSettingValue<double>("spacing"));
    Span sp = gimmickSpan(g, first.pos, stretch);
    for (auto& r : g_reserved) if (sp.l - spacing < r.r && sp.r + spacing > r.l) { dbgNote(fmt::format("Gimmick #{} not used: would overlap another gimmick of this run", idx + 1)); return; }
    g_reserved.push_back(sp);
    Pending np{idx, first.pos, stretch, first.pos.x + g.off.x + g.width * stretch + 60.f, first.up, sp, {}};
    // Orbs go in right now, at the click, so they work immediately. The rest is pasted after you pass the gimmick (route check).
    if (auto lel = LevelEditorLayer::get()) {
        if (nearestObjectGap(lel, sp, Mod::get()->getSettingValue<bool>("spacing-deco")) >= spacing)
            pasteGimmick(lel, g, first.pos, stretch, first.up, 1, &np.early);
    }
    g_pending.push_back(std::move(np));
    g_lastPlacedX = first.pos.x;
    g_events.clear();
}

static void tryPredict(LevelEditorLayer* lel, PlayerObject* pl) {
    if (!Mod::get()->getSettingValue<bool>("auto-paste") || !Mod::get()->getSettingValue<bool>("predict-orbs")) return;
    if (!g_falling || !g_pred.empty() || g_pts.size() < 8) return;
    std::string mode = modeOf(pl); bool mini = isMini(pl); bool up = pl->m_isUpsideDown;
    CCPoint cur = pl->getPosition();
    std::vector<CCPoint> win;   // the last ~90 units of your fall
    for (size_t i = g_pts.size(); i-- > 0;) {
        if (cur.x - g_pts[i].x > 90.f || win.size() >= 60) break;
        win.push_back(g_pts[i]);
    }
    if (win.size() < 6 || cur.x - win.back().x < 30.f) return;
    float tol = static_cast<float>(Mod::get()->getSettingValue<double>("route-tolerance"));
    float gap = static_cast<float>(Mod::get()->getSettingValue<double>("min-gap"));
    float spacing = static_cast<float>(Mod::get()->getSettingValue<double>("spacing"));
    float chance = static_cast<float>(Mod::get()->getSettingValue<double>("chance"));
    float sg = up ? -1.f : 1.f;
    size_t bestIdx = static_cast<size_t>(-1); float bestAvg = 1e9f; CCPoint bestE0;
    for (size_t i = 0; i < g_gimmicks.size(); i++) {
        auto& g = g_gimmicks[i];
        if (!g.verified || g.types.empty() || g.types[0] != "kick" || g.mode != mode || g.mini != mini || g.route.size() < 3) continue;
        if (!gimmickHasOrb(g)) continue;
        for (size_t k = 0; k < g.route.size(); k++) {
            float rxk = g.route[k].x;
            if (rxk > -60.f) break;   // the orb must still be at least 60 units ahead of you
            CCPoint e0 = ccp(cur.x - rxk, cur.y - sg * g.route[k].y);
            int n = 0; float sum = 0.f, mx = 0.f;
            for (auto& w : win) {
                float rx = w.x - e0.x, ry = sg * (w.y - e0.y);
                if (rx < g.route.front().x || rx > g.route.back().x) continue;
                auto it = std::lower_bound(g.route.begin(), g.route.end(), rx, [](CCPoint const& a, float v) { return a.x < v; });
                if (it == g.route.end()) continue;
                float d = std::fabs(it->y - ry);
                sum += d; mx = std::max(mx, d); n++;
            }
            if (n < 6) continue;
            float avg = sum / n;
            if (avg <= tol && mx <= tol * 3.f && avg < bestAvg) { bestAvg = avg; bestIdx = i; bestE0 = e0; }
        }
    }
    if (bestIdx == static_cast<size_t>(-1)) return;
    auto& g = g_gimmicks[bestIdx];
    if (bestE0.x >= g.x0 - g.off.x - 20.f && bestE0.x <= g.x1 + 60.f) return;   // that is the original gimmick itself
    if (bestE0.x - g_lastPlacedX < gap) return;
    if (rnd() > chance) { g_pred.push_back({bestIdx, bestE0, 1.f, up, {0.f, 0.f}, {}}); g_pred.back().idx = static_cast<size_t>(-1); return; }   // remember the "no" for this fall
    float stretch = 1.f;
    Span sp = gimmickSpan(g, bestE0, stretch);
    for (auto& r : g_reserved) if (sp.l - spacing < r.r && sp.r + spacing > r.l) return;
    if (nearestObjectGap(lel, sp, Mod::get()->getSettingValue<bool>("spacing-deco")) < spacing) return;
    Pred pr{bestIdx, bestE0, stretch, up, sp, {}};
    pasteGimmick(lel, g, bestE0, stretch, up, 1, &pr.early);
    if (pr.early.empty()) return;
    g_reserved.push_back(sp);
    g_pred.push_back(std::move(pr));
    if (debugOn()) Notification::create("orb placed ahead - click it or fly past", NotificationIcon::None, 0.8f)->show();
}

static void saveSelectionAsGimmick(EditorUI* ui) {
    auto lel = LevelEditorLayer::get();
    if (!lel || !ui) return;
    std::vector<GameObject*> objs;
    if (auto sel = ui->getSelectedObjects()) {
        for (unsigned i = 0; i < sel->count(); i++) objs.push_back(static_cast<GameObject*>(sel->objectAtIndex(i)));
    }
    if (objs.empty() && ui->m_selectedObject) objs.push_back(ui->m_selectedObject);
    int idS = static_cast<int>(Mod::get()->getSettingValue<int64_t>("marker-start-id"));
    int idE = static_cast<int>(Mod::get()->getSettingValue<int64_t>("marker-end-id"));
    std::vector<float> selM;   // marker blocks that are part of the selection
    {   // the marker blocks themselves are never part of a gimmick
        std::vector<GameObject*> real;
        for (auto o : objs) {
            if (!o) continue;
            if (o->m_objectID == idS || o->m_objectID == idE) selM.push_back(o->getPositionX());
            else real.push_back(o);
        }
        objs = real;
    }
    if (objs.empty()) {
        FLAlertLayer::create("Layout Placer", "Nothing selected. Select the objects of your gimmick first, then press Save.", "OK")->show();
        return;
    }
    std::string data; float minX = 1e9f, maxX = -1e9f, anchorY = 0.f; int count = 0;
    for (auto o : objs) {
        std::string s = o->getSaveString(lel);
        if (s.empty()) continue;
        if (s.back() != ';') s += ';';
        data += s; count++;
        if (o->getPositionX() < minX || (o->getPositionX() == minX && o->getPositionY() < anchorY)) anchorY = o->getPositionY();
        minX = std::min(minX, o->getPositionX()); maxX = std::max(maxX, o->getPositionX());
    }
    if (data.empty()) {
        FLAlertLayer::create("Layout Placer", "Could not read the selected objects.", "OK")->show();
        return;
    }

    // Marker blocks: the START marker at or before the first object and the END marker at or after the last one.
    // When both exist, ONLY the route (and the clicks) from the start marker's X to the end marker's X are saved.
    float mS = 0.f, mE = 0.f; bool hasS = false, hasE = false;
    // Both marker IDs count as "a marker"; the LEFT one of a pair is the start and the RIGHT one is the end, so it does
    // not matter which of the two marker objects you placed where (two identical markers work as well).
    std::vector<float> xsM;
    if (lel->m_objects) {
        for (unsigned i = 0; i < lel->m_objects->count(); i++) {
            auto o = static_cast<GameObject*>(lel->m_objects->objectAtIndex(i));
            if (o && (o->m_objectID == idS || o->m_objectID == idE)) xsM.push_back(o->getPositionX());
        }
        std::sort(xsM.begin(), xsM.end());
    }
    if (selM.size() >= 2) {
        // Markers that you SELECTED together with the gimmick are used as they are: leftmost = start, rightmost = end.
        mS = *std::min_element(selM.begin(), selM.end()); mE = *std::max_element(selM.begin(), selM.end());
        hasS = hasE = mE > mS + 0.5f;
    } else {
        // START: the nearest marker at or before the first object; if there is none, the first one that lies inside the gimmick.
        for (float x : xsM) if (x <= minX + 0.5f && (!hasS || x > mS)) { mS = x; hasS = true; }
        if (!hasS) for (float x : xsM) if (x <= maxX + 0.5f) { mS = x; hasS = true; break; }
        // END: the next marker to the right of the start marker (it may lie anywhere after it, even before the last object).
        if (hasS) for (float x : xsM) if (x > mS + 0.5f) { mE = x; hasE = true; break; }
    }
    if (!xsM.empty() && !(hasS && hasE)) {
        std::string where;
        for (size_t i = 0; i < xsM.size() && i < 10; i++) where += fmt::format("{}{:.0f}", i ? ", " : "", xsM[i]);
        FLAlertLayer::create("Layout Placer", fmt::format("Marker problem: found {} marker block(s) (object IDs {} / {}), but a gimmick needs a START marker at or before its first object and a second marker to the right of it as the END.\n\nPlace them with Q/E + R in a playtest (\"Gimmick START marker\" / \"Gimmick END marker\"), or change the marker object IDs in the mod settings to the objects you placed.\n\nMarkers found at X: {}\nTip: select the two markers together with the gimmick and press Save; then exactly those two markers are used.", xsM.size(), idS, idE, where).c_str(), "OK")->show();
        return;
    }
    bool useMarkers = hasS && hasE;
    float rangeL = useMarkers ? mS : minX, rangeR = useMarkers ? mE : maxX + 30.f;

    // The most recent playtest route that flew through the whole range.
    auto passes = [&](Run const& r) { return r.pts.size() >= 5 && r.pts.front().x <= rangeL && r.pts.back().x >= rangeR; };
    Run const* run = nullptr;
    if (passes(g_cur)) run = &g_cur;
    else for (auto it = g_runs.rbegin(); it != g_runs.rend(); ++it) if (passes(*it)) { run = &*it; break; }
    if (!run) {
        FLAlertLayer::create("Layout Placer", "No recent playtest route passes through the selected objects.\n\nPlaytest this part of the level first (start before the objects and get past them), then select the objects and press Save.", "OK")->show();
        return;
    }

    // Click effects that belong to this gimmick: from a little before the first object up to the last object.
    float lead = static_cast<float>(Mod::get()->getSettingValue<double>("lead-in"));
    std::vector<Ev> evs;
    if (useMarkers) { for (auto& e : run->evs) if (e.pos.x >= mS && e.pos.x <= mE) evs.push_back(e); }   // every click effect between the markers
    else for (auto& e : run->evs) if (e.pos.x >= minX - lead && e.pos.x <= maxX) evs.push_back(e);
    // Nothing inside the lead-in range: the effect that STARTED this trajectory (the fall that ends on the block, the
    // jump that began further back, ...) is the latest effect before the objects, so use that one.
    float usedBack = 0.f;
    if (evs.empty() && !useMarkers) {
        float maxBack = std::max(600.f, lead);
        Ev const* best = nullptr;
        for (auto& e : run->evs) if (e.pos.x < minX - lead && e.pos.x >= minX - maxBack && (!best || e.pos.x > best->pos.x)) best = &e;
        if (best) { evs.push_back(*best); usedBack = minX - best->pos.x; }
    }
    if (evs.empty() && useMarkers) {
        FLAlertLayer::create("Layout Placer", fmt::format("Your latest route has no click effect (jump, gravity flip, wave hold/release, ship hold, fall) between the START and END marker.\n\nIn that run: {} click(s), {} effect(s) in total.", run->presses, run->evs.size()).c_str(), "OK")->show();
        return;
    }
    if (evs.empty()) {
        FLAlertLayer::create("Layout Placer", fmt::format("Your latest route through these objects has no click effect (jump, gravity flip, wave hold/release, ship hold, fall) within {:.0f} units before the first object, or inside the objects.\n\nIn that run: {} click(s), {} effect(s) in total.", std::max(600.f, lead), run->presses, run->evs.size()).c_str(), "OK")->show();
        return;
    }

    Gimmick g;
    auto e0 = evs[0];
    g.width = maxX - minX + 30.f; g.x0 = minX; g.x1 = maxX; g.ay = anchorY; g.data = data;
    g.mode = e0.mode; g.mini = e0.mini; g.up = e0.up;
    float sg = g.up ? -1.f : 1.f;   // store everything gravity-relative
    for (auto& e : evs) { g.types.push_back(e.type); g.evx.push_back(e.pos.x - e0.pos.x); }   // number of clicks + their timing
    g.off = ccp(g.x0 - e0.pos.x, sg * (g.ay - e0.pos.y));
    float lastX = -1e9f;
    for (auto& p : run->pts) {
        CCPoint rel = p - e0.pos; rel.y *= sg;
        if (useMarkers ? (p.x < mS || p.x > mE || rel.x - lastX < 2.f) : (rel.x < -30.f || p.x > maxX + 60.f || rel.x - lastX < 2.f)) continue;
        g.route.push_back(rel); lastX = rel.x;
    }
    if (g.route.size() < 3) {
        FLAlertLayer::create("Layout Placer", "The route near the first click effect is too short to save. Playtest a bit further before the objects.", "OK")->show();
        return;
    }
    g.verified = true;   // it has a recorded route; that is all that is needed
    size_t nEff = g.types.size();
    g_gimmicks.push_back(std::move(g));
    storeGimmicks();
    FLAlertLayer::create("Gimmick saved", fmt::format("Saved {} object(s) as gimmick #{} with {} click effect(s) and their timing{}.\n\nIt is pasted when the same effects happen (in the same timing) and your route is similar - or, if it is not, when the gimmick can still be passed. It is always pasted exactly as saved.", count, g_gimmicks.size(), nEff, useMarkers ? fmt::format(", route saved only between the markers ({:.0f} units)", mE - mS) : (usedBack > 0.f ? fmt::format(", started {:.0f} units before the objects", usedBack) : std::string())).c_str(), "OK")->show();
}

static void onPlayerPress(PlayerObject* pl) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    g_pendPress = true; g_lastPress = nowSeconds(); g_pressR = g_prevR;
    g_cur.presses++;
    if (debugOn()) Notification::create("click", NotificationIcon::None, 0.3f)->show();
}
static void onPlayerRelease(PlayerObject* pl) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    g_pendRelease = true; g_lastRelease = nowSeconds();
}

static void onPlayerUpdate(PlayerObject* pl, float dt) {
    auto lel = LevelEditorLayer::get();
    if (!lel || lel->m_playbackMode != PlaybackMode::Playing || pl != lel->m_player1) return;
    (void)dt;
    g_time = nowSeconds();
    float x = pl->getPositionX();
    if (g_newRunPending || x < g_lastX - 100.f) {   // new playtest / restart / death / checkpoint
        g_newRunPending = false;
        if (g_cur.pts.size() >= 5) { g_runs.push_back(std::move(g_cur)); if (g_runs.size() > 6) g_runs.erase(g_runs.begin()); }
        g_cur = Run{};
        g_events.clear(); g_pending.clear(); for (auto& pr : g_pred) removeList(lel, pr.early); g_pred.clear(); g_reserved.clear(); g_lastPlacedX = -1e9f; g_path.clear(); g_pts.clear(); g_hasPrev = false;
        g_havePrevState = false; g_pendPress = g_pendRelease = false;
    }
    g_lastX = x;
    {
        CCRect cur = playerRect(pl);
        g_path.push_back(g_hasPrev ? rectUnion(g_prevRect, cur) : cur);
        g_pts.push_back(pl->getPosition());
        if (g_cur.pts.empty() || x - g_cur.pts.back().x >= 2.f) { if (g_cur.pts.size() < 100000) g_cur.pts.push_back(pl->getPosition()); }
        g_prevRect = cur; g_hasPrev = true;
        if (g_path.size() > 60000) { g_path.erase(g_path.begin(), g_path.begin() + 30000); g_pts.erase(g_pts.begin(), g_pts.begin() + 30000); }
    }
    auto type = detectEffect(pl);
    if (!type.empty()) onEffect(pl, type);
    // Orbs placed ahead that you flew past without clicking: take them back.
    for (size_t i = 0; i < g_pred.size();) {
        auto& pr = g_pred[i];
        bool dead = pr.idx == static_cast<size_t>(-1) ? !g_falling : (pr.idx >= g_gimmicks.size() || x > pr.at.x + g_gimmicks[pr.idx].off.x + 60.f);
        if (dead) {
            if (pr.idx != static_cast<size_t>(-1)) { releaseReserved(pr.sp); removeList(lel, pr.early); }
            g_pred.erase(g_pred.begin() + i);
        } else i++;
    }
    tryPredict(lel, pl);

    float tol = static_cast<float>(Mod::get()->getSettingValue<double>("route-tolerance"));
    for (size_t i = 0; i < g_pending.size();) {
        if (x > g_pending[i].releaseX) {
            auto p = std::move(g_pending[i]);
            g_pending.erase(g_pending.begin() + i);
            if (p.idx >= g_gimmicks.size()) { removeEarly(lel, p); continue; }
            auto& pg = g_gimmicks[p.idx];
            float dAvg = 0.f, dMax = 0.f; int dN = 0;
            bool matched = routeMatches(pg, p.at, tol, p.up, &dAvg, &dMax, &dN);
            if (!matched && !Mod::get()->getSettingValue<bool>("allow-passable")) {
                releaseReserved(p.sp);   // the orb of a successful click stays
                if (dN < 5) Notification::create(fmt::format("Gimmick #{} skipped: too little of your route overlaps the saved route ({} points)", p.idx + 1, dN), NotificationIcon::Warning, 1.5f)->show();
                else Notification::create(fmt::format("Gimmick #{} skipped: route differs (avg {:.0f} / max {:.0f}, allowed {:.0f} / {:.0f})", p.idx + 1, dAvg, dMax, tol, tol * 3.f), NotificationIcon::Warning, 1.5f)->show();
                continue;
            }
            {
                float spacing = static_cast<float>(Mod::get()->getSettingValue<double>("spacing"));
                Span sp = gimmickSpan(pg, p.at, p.stretch);
                float gap = nearestObjectGap(lel, sp, Mod::get()->getSettingValue<bool>("spacing-deco"), &p.early);
                if (gap < spacing) {
                    releaseReserved(p.sp);
                    Notification::create(fmt::format("Gimmick #{} skipped: not enough room (nearest object {:.0f} units away, need {:.0f})", p.idx + 1, gap, spacing), NotificationIcon::Warning, 1.5f)->show();
                    continue;
                }
            }
            // The route is similar, OR (when it is not) the gimmick is still fine as long as none of its objects is in the way of
            // the route you flew from its start until now (with your current hitbox). It goes in exactly as saved or not at all.
            int blocked = pasteGimmick(lel, pg, p.at, 1.f, p.up, p.early.empty() ? 0 : 2, nullptr, true);
            if (blocked) {
                releaseReserved(p.sp);
                Notification::create(fmt::format("Gimmick #{} skipped: {} object(s) would be in your way, so it cannot be passed", p.idx + 1, blocked), NotificationIcon::Warning, 1.5f)->show();
                continue;
            }
            Notification::create(matched ? std::string("Gimmick pasted") : std::string("Gimmick pasted (route differs, but you can pass it)"), NotificationIcon::Success, 1.f)->show();
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
                    if (ITEMS[g_sel].id >= 0) p.x += static_cast<float>(Mod::get()->getSettingValue<double>("offset-x"));   // markers sit exactly at the player
                    int id = ITEMS[g_sel].id;
                    if (id < 0) id = static_cast<int>(Mod::get()->getSettingValue<int64_t>(id == -1 ? "marker-start-id" : "marker-end-id"));
                    if (ITEMS[g_sel].pad) p.y -= 9.f;
                    if (auto o = lel->createObject(id, p, false)) g_created.push_back(o);
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
        bool playing = lel && lel->m_playbackMode == PlaybackMode::Playing;
        if (!playing && g_wasPlaying) {   // playtest ended: the next one starts a new attempt
            g_newRunPending = true;
            // orbs of clicks that worked stay in the level; only orbs nobody clicked (g_pred) are taken back
            g_pending.clear(); for (auto& pr : g_pred) removeList(lel, pr.early); g_pred.clear(); g_reserved.clear();
        }
        g_wasPlaying = playing;
        if (!playing) return;
        if (Mod::get()->getSettingValue<bool>("show-generated"))
            for (auto& o : g_created) if (o) o->setVisible(true);
        if (Mod::get()->getSettingValue<bool>("hide-orbs"))
            for (auto& o : g_hidden) if (o) o->setVisible(false);
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
