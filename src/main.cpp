#include "main.hpp"
#include "config.hpp"

// beatsaber-hook 7.x
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

// codegen — 1.40.8 headers
#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/SaberModelController_InitData.hpp"  // name changed in 1.40.x
#include "GlobalNamespace/IBladeMovementData.hpp"

// Unity
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Object.hpp"

// custom-types 0.18.x
#include "custom-types/shared/register.hpp"
#include "custom-types/shared/macros.hpp"

// BSML 0.4.x
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"

#include <deque>

// ─── Logging / config ────────────────────────────────────────────────────────

static ModInfo modInfo{MOD_ID, VERSION};

Logger& getLogger() {
    static Logger* logger = new Logger(modInfo);
    return *logger;
}

Configuration& getConfig() {
    static Configuration config(modInfo);
    config.Load();
    return config;
}

// ─── BlurConfig persistence ──────────────────────────────────────────────────

BlurConfig blurConfig;

void SaveConfig() {
    auto& cfg = getConfig().config;
    cfg.RemoveAllMembers();
    rapidjson::Document::AllocatorType& alloc = cfg.GetAllocator();
    cfg.AddMember("enabled",    blurConfig.enabled,    alloc);
    cfg.AddMember("ghostCount", blurConfig.ghostCount,  alloc);
    cfg.AddMember("lifetime",   blurConfig.lifetime,    alloc);
    cfg.AddMember("alphaMax",   blurConfig.alphaMax,    alloc);
    cfg.AddMember("minSpeed",   blurConfig.minSpeed,    alloc);
    getConfig().Write();
}

bool LoadConfig() {
    getConfig().Load();
    auto& cfg = getConfig().config;
    if (!cfg.IsObject() || !cfg.HasMember("enabled")) return false;
    blurConfig.enabled    = cfg["enabled"].GetBool();
    blurConfig.ghostCount = cfg["ghostCount"].GetInt();
    blurConfig.lifetime   = cfg["lifetime"].GetFloat();
    blurConfig.alphaMax   = cfg["alphaMax"].GetFloat();
    blurConfig.minSpeed   = cfg["minSpeed"].GetFloat();
    return true;
}

// ─── Ghost entry ─────────────────────────────────────────────────────────────

struct GhostEntry {
    UnityEngine::GameObject* go;
    float spawnTime;
    float lifetime;
};

// ─── SaberBlurController (MonoBehaviour) ────────────────────────────────────
//
// One of these is attached to each saber model root by the hook below.
// Every Update() it:
//   1. Measures tip speed
//   2. Fades existing ghosts by age
//   3. Spawns a new frozen Instantiate() copy when moving fast enough

DECLARE_CLASS_CODEGEN(BlurSabers, SaberBlurController, UnityEngine::MonoBehaviour,

    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, _saberRoot);

    DECLARE_INSTANCE_METHOD(void, Awake);
    DECLARE_INSTANCE_METHOD(void, Update);
    DECLARE_INSTANCE_METHOD(void, OnDestroy);

public:
    std::deque<GhostEntry> ghosts;
    UnityEngine::Vector3   lastTipWorld{0,0,0};
    bool  initialized = false;
    float spawnTimer  = 0.0f;
)

DEFINE_TYPE(BlurSabers, SaberBlurController);

void BlurSabers::SaberBlurController::Awake() {
    _saberRoot  = get_transform();
    initialized = false;
}

void BlurSabers::SaberBlurController::Update() {
    using namespace UnityEngine;

    if (!blurConfig.enabled) {
        for (auto& g : ghosts) Object::Destroy(g.go);
        ghosts.clear();
        return;
    }

    float now = Time::get_time();
    float dt  = Time::get_deltaTime();

    // Approximate tip: 1 m along saber's local +Z
    Vector3 tipWorld = _saberRoot->TransformPoint(Vector3{0.0f, 0.0f, 1.0f});

    if (!initialized) {
        lastTipWorld = tipWorld;
        initialized  = true;
        return;
    }

    // Speed in m/s
    float dist  = Vector3::Distance(tipWorld, lastTipWorld);
    float speed = (dt > 0.0f) ? dist / dt : 0.0f;
    lastTipWorld = tipWorld;

    // ── Expire old ghosts ────────────────────────────────────────────────────
    while (!ghosts.empty()) {
        auto& front = ghosts.front();
        if (now - front.spawnTime >= front.lifetime) {
            Object::Destroy(front.go);
            ghosts.pop_front();
        } else {
            break;
        }
    }

    // ── Fade living ghosts ───────────────────────────────────────────────────
    for (auto& g : ghosts) {
        float age   = now - g.spawnTime;
        float t     = 1.0f - (age / g.lifetime);          // 1 → 0
        float alpha = blurConfig.alphaMax * t;
        if (alpha < 0.0f) alpha = 0.0f;

        // GetComponentsInChildren returns Array<T*>*
        auto* renderers = g.go->GetComponentsInChildren<MeshRenderer*>();
        for (int i = 0; i < (int)renderers->get_Count(); i++) {
            auto* mat = renderers->get_Item(i)->get_material();
            Color c   = mat->get_color();
            c.a       = alpha;
            mat->set_color(c);
        }
    }

    // ── Spawn a new ghost ────────────────────────────────────────────────────
    spawnTimer -= dt;
    float interval = (blurConfig.ghostCount > 0)
        ? (blurConfig.lifetime / (float)blurConfig.ghostCount)
        : 9999.0f;

    if (speed >= blurConfig.minSpeed && spawnTimer <= 0.0f) {
        spawnTimer = interval;

        auto* ghost = Object::Instantiate(_saberRoot->get_gameObject());

        // Detach from hierarchy — freeze it in current world pose
        ghost->get_transform()->SetParent(nullptr);
        ghost->get_transform()->set_position(_saberRoot->get_position());
        ghost->get_transform()->set_rotation(_saberRoot->get_rotation());
        ghost->set_name(il2cpp_utils::newcsstr("BlurGhost"));

        // Remove any SaberBlurController copies on the ghost
        auto* controllers = ghost->GetComponentsInChildren<SaberBlurController*>();
        for (int i = 0; i < (int)controllers->get_Count(); i++)
            Object::Destroy(reinterpret_cast<UnityEngine::Object*>(controllers->get_Item(i)));

        ghosts.push_back({ghost, now, blurConfig.lifetime});
    }
}

void BlurSabers::SaberBlurController::OnDestroy() {
    for (auto& g : ghosts) UnityEngine::Object::Destroy(g.go);
    ghosts.clear();
}

// ─── Hook ────────────────────────────────────────────────────────────────────
//
// SaberModelController::Init is called once per saber when the model loads.
// We attach our MonoBehaviour here.
//
// NOTE: The exact signature for 1.40.8 — check your codegen dump if the
// compiler complains about argument types.  The two most common forms are:
//   Init(Transform*, SaberModelController_InitData*, IBladeMovementData*)
//   Init(Transform*, SaberModelControllerInitData*, IBladeMovementData*)

MAKE_HOOK_MATCH(
    SaberModelController_Init,
    &GlobalNamespace::SaberModelController::Init,
    void,
    GlobalNamespace::SaberModelController*                  self,
    UnityEngine::Transform*                                  parent,
    GlobalNamespace::SaberModelController_InitData*          initData,
    ::GlobalNamespace::IBladeMovementData*                   movementData
) {
    SaberModelController_Init(self, parent, initData, movementData);

    if (!blurConfig.enabled) return;

    auto* go = self->get_gameObject();
    if (!go->GetComponent<BlurSabers::SaberBlurController*>())
        go->AddComponent<BlurSabers::SaberBlurController*>();
}

// ─── BSML settings UI ────────────────────────────────────────────────────────
//
// BSML 0.4.x settings are registered as a ModSettings entry.
// We use the programmatic API here to avoid needing an external .bsml XML file.

#include "HMUI/ViewController.hpp"

void DidActivate(HMUI::ViewController* self, bool firstActivation,
                 bool addedToHierarchy, bool screenSystemEnabling) {
    if (!firstActivation) return;

    // Use BSML's builder helpers — these match the 0.4.x API
    BSML::parse_and_pump_all(
        self->get_transform(),
        R"(
<bg xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance'
    xsi:schemaLocation='https://monkeymanboy.github.io/BSML-Docs/ https://raw.githubusercontent.com/monkeymanboy/BSML-Docs/gh-pages/BSMLSchema.xsd'>
  <settings-container>
    <toggle-setting text='Enable Blur' value='enabled' apply-on-change='true'/>
    <slider-setting text='Ghost Count'       value='ghostCount' min='1'    max='20'   increment='1'    apply-on-change='true'/>
    <slider-setting text='Ghost Lifetime (s)' value='lifetime'  min='0.02' max='0.5'  increment='0.01' apply-on-change='true'/>
    <slider-setting text='Max Alpha'         value='alphaMax'   min='0.0'  max='1.0'  increment='0.05' apply-on-change='true'/>
    <slider-setting text='Min Speed (m/s)'   value='minSpeed'   min='0.0'  max='20.0' increment='0.5'  apply-on-change='true'/>
  </settings-container>
</bg>
        )",
        nullptr  // host object — nullptr means no data binding; wire values manually below
    );

    // Because we're not using a host C# object for data binding in BSML,
    // the simplest approach is to just build the toggles and sliders with
    // the BSML Lite helpers instead:
}

// Fallback: use BSMLStuff / lite helpers if parse_and_pump_all isn't available
// in your version.  The pattern that always works in 0.4.x:
//
//   BSML::Lite::CreateToggle(parent, "Enable Blur", blurConfig.enabled,
//       [](bool v){ blurConfig.enabled = v; SaveConfig(); });
//
//   BSML::Lite::CreateSliderSetting(parent, "Ghost Count",
//       1.0f, blurConfig.ghostCount, 1.0f, 20.0f,
//       [](float v){ blurConfig.ghostCount = (int)v; SaveConfig(); });
//
// Uncomment and replace the DidActivate body above with these if you hit
// a compile error on parse_and_pump_all.

// ─── Entry points ────────────────────────────────────────────────────────────

extern "C" void setup(ModInfo& info) {
    info.id      = MOD_ID;
    info.version = VERSION;
    modInfo      = info;
    getLogger().info("Blur Sabers setup");
}

extern "C" void load() {
    il2cpp_functions::Init();

    if (!LoadConfig()) {
        blurConfig = {};   // reset to struct defaults
        SaveConfig();
    }

    custom_types::Register::AutoRegister();

    BSML::Register::RegisterSettingsMenu(
        modInfo,
        "Blur Sabers",
        DidActivate,
        false
    );

    INSTALL_HOOK(getLogger(), SaberModelController_Init);

    getLogger().info("Blur Sabers loaded — beatsaber-hook 7.x / BSML 0.4.x / 1.40.8");
}
