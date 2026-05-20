// Must be first — CMake passes -DID="blursaber" which collides with fmt's ID template param
#undef ID

#include "config.hpp"

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/Saber.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/PrimitiveType.hpp"

#include "custom-types/shared/register.hpp"
#include "custom-types/shared/macros.hpp"

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML-Lite.hpp"

#include "HMUI/ViewController.hpp"

#include "scotland2/shared/modloader.h"
#include "paper2_scotland2/shared/logger.hpp"

#include <cmath>

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

// ─── Smoothing helper ────────────────────────────────────────────────────────

static float smoothing_to_factor(float s, float dt) {
    float speed = 50.0f * (1.0f - s) + 0.5f * s;
    return std::exp(-speed * dt);
}

// ─── SaberSmoothing ──────────────────────────────────────────────────────────

DECLARE_CLASS_CODEGEN(BlurSabers, SaberSmoothing, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, _target);
    DECLARE_INSTANCE_METHOD(void, Awake);
    DECLARE_INSTANCE_METHOD(void, Update);
public:
    UnityEngine::Vector3    smoothPos;
    UnityEngine::Quaternion smoothRot;
    bool initialized = false;
};

DEFINE_TYPE(BlurSabers, SaberSmoothing);

void BlurSabers::SaberSmoothing::Awake() {
    initialized = false;
}

void BlurSabers::SaberSmoothing::Update() {
    using namespace UnityEngine;

    if (!_target) return;

    Vector3    targetPos = _target->get_position();
    Quaternion targetRot = _target->get_rotation();

    if (!blurConfig.enabled) {
        get_transform()->set_position(targetPos);
        get_transform()->set_rotation(targetRot);
        return;
    }

    if (!initialized) {
        smoothPos   = targetPos;
        smoothRot   = targetRot;
        initialized = true;
    }

    float dt     = Time::get_deltaTime();
    float factor = 1.0f - smoothing_to_factor(blurConfig.smoothing, dt);

    smoothPos = Vector3::Lerp(smoothPos, targetPos, factor);
    smoothRot = Quaternion::Slerp(smoothRot, targetRot, factor);

    get_transform()->set_position(smoothPos);
    get_transform()->set_rotation(smoothRot);
}

// ─── Hook ────────────────────────────────────────────────────────────────────
// Real signature from 1.40.8 codegen:
// void SaberModelController::Init(Transform* parent, Saber* saber, Color color)

MAKE_HOOK_MATCH(
    SaberModelController_Init,
    &GlobalNamespace::SaberModelController::Init,
    void,
    GlobalNamespace::SaberModelController* self,
    UnityEngine::Transform* parent,
    GlobalNamespace::Saber* saber,
    UnityEngine::Color color
) {
    SaberModelController_Init(self, parent, saber, color);

    using namespace UnityEngine;

    // saberRoot tracks the real controller
    auto saberRoot = self->get_transform();

    // Hide all existing mesh renderers
    auto renderers = self->get_gameObject()->GetComponentsInChildren<MeshRenderer*>();
    for (int i = 0; i < (int)renderers.size(); i++)
        renderers[i]->set_enabled(false);

    // Spawn cylinder blade
    auto blade = GameObject::CreatePrimitive(PrimitiveType::Cylinder);
    blade->set_name("BlurBlade");

    auto bladeT = blade->get_transform();
    bladeT->SetParent(saberRoot, false);

    // Scale to saber proportions: 1m long, 2cm wide
    bladeT->set_localScale(Vector3{0.02f, 0.5f, 0.02f});
    // Shift up so bottom sits at hilt
    bladeT->set_localPosition(Vector3{0.0f, 0.5f, 0.0f});
    // Rotate to align with +Z (Beat Saber saber direction)
    bladeT->set_localEulerAngles(Vector3{90.0f, 0.0f, 0.0f});

    // Attach smoothing
    if (!blade->GetComponent<BlurSabers::SaberSmoothing*>()) {
        auto smooth   = blade->AddComponent<BlurSabers::SaberSmoothing*>();
        smooth->_target = saberRoot.ptr();
    }
}

// ─── Settings UI ─────────────────────────────────────────────────────────────

void DidActivate(HMUI::ViewController* self, bool firstActivation,
                 bool addedToHierarchy, bool screenSystemEnabling) {
    if (!firstActivation) return;
    auto* container = BSML::Lite::CreateScrollableSettingsContainer(self->get_transform());
    auto* t = container->get_transform();

    BSML::Lite::CreateToggle(t, "Enable Smoothing", blurConfig.enabled,
        [](bool v){ blurConfig.enabled = v; SaveConfig(); });
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
