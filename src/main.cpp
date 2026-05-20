#undef ID

#include "config.hpp"

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/IBladeMovementData.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

#include "custom-types/shared/register.hpp"
#include "custom-types/shared/macros.hpp"

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML-Lite.hpp"

#include "HMUI/ViewController.hpp"

#include "scotland2/shared/modloader.h"
#include "paper2_scotland2/shared/logger.hpp"

// ─── Logger / modinfo ────────────────────────────────────────────────────────

static constexpr auto PaperLogger = Paper::ConstLoggerContext<11>("BlurSabers");
static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

// ─── Config ──────────────────────────────────────────────────────────────────

BlurConfig blurConfig;

Configuration& getConfig() {
    static Configuration config(modInfo);
    config.Load();
    return config;
}

void SaveConfig() {
    auto& cfg = getConfig().config;
    cfg.RemoveAllMembers();
    auto& alloc = cfg.GetAllocator();
    cfg.AddMember("enabled",   blurConfig.enabled,   alloc);
    cfg.AddMember("smoothing", blurConfig.smoothing,  alloc);
    getConfig().Write();
}

bool LoadConfig() {
    getConfig().Load();
    auto& cfg = getConfig().config;
    if (!cfg.IsObject() || !cfg.HasMember("enabled")) return false;
    blurConfig.enabled   = cfg["enabled"].GetBool();
    blurConfig.smoothing = cfg["smoothing"].GetFloat();
    return true;
}

// ─── SaberSmoothing ──────────────────────────────────────────────────────────
//
// Attached to each saber model root. Every Update() it reads the *parent*
// transform (the real controller position) and lerps the visual saber toward
// it, creating the ReeSabers-style motion smoothing effect.

DECLARE_CLASS_CODEGEN(BlurSabers, SaberSmoothing, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, _parent);
    DECLARE_INSTANCE_METHOD(void, Awake);
    DECLARE_INSTANCE_METHOD(void, Update);
public:
    UnityEngine::Vector3    smoothPos;
    UnityEngine::Quaternion smoothRot;
    bool initialized = false;
};

DEFINE_TYPE(BlurSabers, SaberSmoothing);

void BlurSabers::SaberSmoothing::Awake() {
    // The saber model sits under a parent that tracks the real controller
    _parent     = get_transform()->get_parent();
    initialized = false;
}

void BlurSabers::SaberSmoothing::Update() {
    using namespace UnityEngine;

    if (!_parent) return;

    Vector3    targetPos = _parent->get_position();
    Quaternion targetRot = _parent->get_rotation();

    if (!blurConfig.enabled) {
        // Disabled — snap to real position with no lag
        get_transform()->set_position(targetPos);
        get_transform()->set_rotation(targetRot);
        return;
    }

    if (!initialized) {
        smoothPos   = targetPos;
        smoothRot   = targetRot;
        initialized = true;
    }

    // smoothing: 0 = instant snap, 1 = never moves
    // We convert to a per-frame lerp factor using deltaTime so it's
    // framerate-independent. Factor of ~8–15 gives a ReeSabers-like feel.
    float dt     = Time::get_deltaTime();
    float factor = 1.0f - smoothing_to_factor(blurConfig.smoothing, dt);

    smoothPos = Vector3::Lerp(smoothPos, targetPos, factor);
    smoothRot = Quaternion::Slerp(smoothRot, targetRot, factor);

    get_transform()->set_position(smoothPos);
    get_transform()->set_rotation(smoothRot);
}

// Free function used above — converts a 0–1 "smoothing" slider value into
// a per-frame lerp alpha that is framerate-independent.
//   smoothing=0  → factor=0  → lerp(pos, target, 1)  = instant snap
//   smoothing=1  → factor≈1  → lerp(pos, target, ~0) = nearly frozen
// We use exponential decay: alpha = exp(-speed * dt)
// speed maps smoothing 0→1 to speed 50→0.5
static float smoothing_to_factor(float s, float dt) {
    float speed = 50.0f * (1.0f - s) + 0.5f * s;  // lerp between 50 and 0.5
    return std::exp(-speed * dt);
}

// ─── Hook ────────────────────────────────────────────────────────────────────

MAKE_HOOK_MATCH(
    SaberModelController_Init,
    &GlobalNamespace::SaberModelController::Init,
    void,
    GlobalNamespace::SaberModelController* self,
    UnityEngine::Transform* parent,
    GlobalNamespace::SaberModelController::InitData* initData,
    GlobalNamespace::IBladeMovementData* movementData
) {
    SaberModelController_Init(self, parent, initData, movementData);

    auto* go = self->get_gameObject();
    if (!go->GetComponent<BlurSabers::SaberSmoothing*>())
        go->AddComponent<BlurSabers::SaberSmoothing*>();
}

// ─── Settings UI ─────────────────────────────────────────────────────────────

void DidActivate(HMUI::ViewController* self, bool firstActivation,
                 bool addedToHierarchy, bool screenSystemEnabling) {
    if (!firstActivation) return;
    auto* container = BSML::Lite::CreateScrollableSettingsContainer(self->get_transform());
    auto* t = container->get_transform();

    BSML::Lite::CreateToggle(t, "Enable Smoothing", blurConfig.enabled,
        [](bool v){ blurConfig.enabled = v; SaveConfig(); });

    // 0.0 = no lag (instant), 0.99 = very heavy lag
    BSML::Lite::CreateSliderSetting(t, "Smoothing Amount",
        0.01f, blurConfig.smoothing, 0.0f, 0.99f, 0.0f,
        [](float v){ blurConfig.smoothing = v; SaveConfig(); });
}

// ─── Entry points ────────────────────────────────────────────────────────────

extern "C" void setup(CModInfo* info) {
    info->id           = MOD_ID;
    info->version      = VERSION;
    info->version_long = 0;
    PaperLogger.info("Blur Sabers setup");
}

MOD_EXTERN_FUNC void late_load() {
    il2cpp_functions::Init();

    if (!LoadConfig()) {
        blurConfig = {};
        SaveConfig();
    }

    custom_types::Register::AutoRegister();

    BSML::Register::RegisterSettingsMenu(
        "Blur Sabers", DidActivate, false
    );

    INSTALL_HOOK(PaperLogger, SaberModelController_Init);
    PaperLogger.info("Blur Sabers loaded");
}
