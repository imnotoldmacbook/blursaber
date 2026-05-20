// Must be first — CMake passes -DID="blursaber" which collides with fmt's ID template param
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
    // _target is set externally after AddComponent
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

    using namespace UnityEngine;

    auto* saberRoot = self->get_transform();

    // ── Hide all existing mesh renderers on the default/custom saber ──────
    auto* renderers = self->get_gameObject()->GetComponentsInChildren<MeshRenderer*>();
    for (int i = 0; i < (int)renderers->get_Count(); i++)
        renderers->get_Item(i)->set_enabled(false);

    // ── Create a cylinder to act as our test blade ────────────────────────
    // CreatePrimitive spawns at world origin; we'll position it locally
    auto* blade = GameObject::CreatePrimitive(PrimitiveType::Cylinder);
    blade->set_name("BlurBlade");

    // Parent it to the saber root so it inherits controller movement
    auto* bladeT = blade->get_transform();
    bladeT->SetParent(saberRoot, false);

    // A Unity cylinder is 2 units tall and 1 unit wide by default.
    // Beat Saber sabers are ~1 m long and ~0.02 m wide.
    // Scale: x=0.02, y=0.5 (half-height gives 1m total), z=0.02
    bladeT->set_localScale(Vector3{0.02f, 0.5f, 0.02f});

    // Offset so the bottom of the cylinder sits at the hilt (local origin)
    // Unity cylinder pivot is centered, so shift up by half its height (0.5)
    bladeT->set_localPosition(Vector3{0.0f, 0.5f, 0.0f});

    // Beat Saber sabers point along local +Z, Unity cylinders along local +Y
    // Rotate 90° around X to align them
    bladeT->set_localEulerAngles(Vector3{90.0f, 0.0f, 0.0f});

    // ── Attach smoothing — track the saber root transform ─────────────────
    if (!blade->GetComponent<BlurSabers::SaberSmoothing*>()) {
        auto* smooth  = blade->AddComponent<BlurSabers::SaberSmoothing*>();
        smooth->_target = saberRoot;
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
