#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/config/shared/monitor/MonitorRuleManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

namespace {
using namespace NColorManagement;
HANDLE handle = nullptr;
CFunctionHook* preferredHook = nullptr;
CFunctionHook* cmSettingsHook = nullptr;
WP<Config::IConfigManager> registeredConfigManager;
CHyprSignalListener monitorRulesListener;
CHyprSignalListener configReloadListener;
using PreferredFn = PImageDescription (*)(CWLSurfaceResource*);
using CMSettingsFn = Render::SCMSettings (*)(Render::IHyprRenderer*, Render::CRenderContext&, PImageDescription, PImageDescription, SP<CWLSurfaceResource>, bool, float, int, bool);
// The lookup matches the demangled name only, which omits the return type.
static_assert(std::is_same_v<decltype(&CWLSurfaceResource::getPreferredImageDescription), PImageDescription (CWLSurfaceResource::*)()>);
static_assert(std::is_same_v<decltype(&Render::IHyprRenderer::getCMSettings),
                             Render::SCMSettings (Render::IHyprRenderer::*)(Render::CRenderContext&, PImageDescription, PImageDescription, SP<CWLSurfaceResource>, bool, float, int, bool)>);

// Adapted from erikwb/Hyprland cc0c5094ffdee16c820162a66ce9dbeb77cdbe9e.
// Change client feedback only: the physical output's PQ description is untouched.
PImageDescription preferred(CWLSurfaceResource* surface) {
    const auto original = reinterpret_cast<PreferredFn>(preferredHook->m_original)(surface);
    if (!original)
        return original;
    const auto& source = original->value();
    const bool hdr = source.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ ||
                     source.transferFunction == CM_TRANSFER_FUNCTION_HLG ||
                     source.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR;
    if (!hdr || source.icc.present)
        return original;
    // Only rewrite descriptions belonging to a physical output. Fullscreen
    // passthrough and other custom descriptions keep the compositor's decision.
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor->m_imageDescription || monitor->m_imageDescription->id() != original->id())
            continue;
        // Do not retain a config-value cache across full-reset: Hyprland flushes
        // caches before preReload can restore the plugin's value in the new manager.
        const auto configured = *CConfigValue<Config::INTEGER>("plugin:hypr_sdr_white:reference_luminance");
        // Official 0.57 keeps SDR white separate from the HDR description's 203-nit reference.
        // Match getTFMaxLuminance's 80-nit fallback for a negative monitor value.
        const auto sdrWhite = monitor->m_sdrMaxLuminance >= 0 ? monitor->m_sdrMaxLuminance : static_cast<int>(SDR_MAX_LUMINANCE);
        const auto requested = configured > 0 ? configured : sdrWhite;
        const auto peak = std::max(1U, source.masteringLuminances.max > 0 ? source.masteringLuminances.max : source.luminances.max);
        const auto white = static_cast<uint32_t>(std::clamp<Config::INTEGER>(requested, 1, peak));
        auto result = source;
        result.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22;
        result.transferFunctionPower = 1.0f;
        result.primariesNameSet = true;
        result.primariesNamed = CM_PRIMARIES_SRGB;
        result.primaries = NColorPrimaries::BT709;
        // Same as the original patch: SDR clients encode black at 0 nits.
        result.luminances = SImageDescription::SPCLuminances{.min = 0.0f, .max = white, .reference = white};
        return CImageDescription::from(result);
    }
    return original;
}

bool isHDR(eTransferFunction tf) {
    return tf == CM_TRANSFER_FUNCTION_ST2084_PQ || tf == CM_TRANSFER_FUNCTION_HLG;
}

// Official 0.57 scales the peak by dstRef/srcRef but feeds absolute-nit pixels
// to tonemap. Restore the absolute peak, including when the original used its cache.
Render::SCMSettings cmSettings(Render::IHyprRenderer* renderer, Render::CRenderContext& ctx, PImageDescription source, PImageDescription target,
                               SP<CWLSurfaceResource> surface, bool modifySDR, float sdrMinLuminance, int sdrMaxLuminance, bool shouldUseSurface) {
    auto settings = reinterpret_cast<CMSettingsFn>(cmSettingsHook->m_original)(renderer, ctx, source, target, surface, modifySDR, sdrMinLuminance, sdrMaxLuminance,
                                                                              shouldUseSurface);
    // EXT_LINEAR can describe an SDR work buffer; require HDR headroom only for
    // that ambiguous target. Explicit PQ/HLG targets need no headroom heuristic.
    const bool hdrTarget = isHDR(settings.targetTF) ||
                           (settings.targetTF == CM_TRANSFER_FUNCTION_EXT_LINEAR && settings.dstMaxLuminance > settings.dstRefLuminance);
    const bool hdrSource = isHDR(settings.sourceTF) || settings.sourceTF == CM_TRANSFER_FUNCTION_EXT_LINEAR;
    // Mode 2 already uses an unscaled destination peak. Direct SDR targets stay unchanged.
    if (settings.needsTonemap && settings.tonemapMode != 2 && settings.srcRefLuminance > 0 && settings.dstRefLuminance > 0 && hdrSource && hdrTarget)
        settings.maxLuminance *= settings.srcRefLuminance / settings.dstRefLuminance;
    return settings;
}
static_assert(std::is_same_v<decltype(&cmSettings), CMSettingsFn>);
static_assert(std::is_same_v<decltype(&preferred), PreferredFn>);

void* findFunction(const std::string& name, const std::string& demangled) {
    for (const auto& match : HyprlandAPI::findFunctionsByName(handle, name)) {
        if (match.demangled == demangled)
            return match.address;
    }
    return nullptr;
}

void notifyClients() {
    if (!PROTO::colorManagement)
        return;
    for (const auto& monitor : State::monitorState()->monitors())
        PROTO::colorManagement->onMonitorImageDescriptionChanged(monitor);
}

int refresh(lua_State*) {
    notifyClients();
    return 0;
}

bool registerConfig() {
    if (registeredConfigManager == Config::mgr())
        return true;
    if (!HyprlandAPI::addConfigValueV2(handle, makeShared<Config::Values::CIntValue>(
            "plugin:hypr_sdr_white:reference_luminance", "Client SDR white in nits; 0 follows output sdr_max_luminance", Config::INTEGER{0})))
        return false;
    if (!HyprlandAPI::addLuaFunction(handle, "hypr_sdr_white", "refresh", refresh))
        return false;
    registeredConfigManager = Config::mgr();
    return true;
}
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE pluginHandle) {
    handle = pluginHandle;
    if (HyprlandAPI::getHyprlandVersion(handle).hash != GIT_COMMIT_HASH)
        throw std::runtime_error("hypr-sdr-white: compositor/header commit mismatch");
    if (!registerConfig())
        throw std::runtime_error("hypr-sdr-white: failed to register config and refresh API");
    void* address = findFunction("getPreferredImageDescription", "CWLSurfaceResource::getPreferredImageDescription()");
    if (!address)
        throw std::runtime_error("hypr-sdr-white: preferred-description symbol unavailable");
    preferredHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(&preferred));
    if (!preferredHook || !preferredHook->hook())
        throw std::runtime_error("hypr-sdr-white: failed to attach preferred-description hook");
    address = findFunction("getCMSettings",
                           "Render::IHyprRenderer::getCMSettings(Render::CRenderContext&, "
                           "Hyprutils::Memory::CWeakPointer<NColorManagement::CImageDescription const>, "
                           "Hyprutils::Memory::CWeakPointer<NColorManagement::CImageDescription const>, "
                           "Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, bool, float, int, bool)");
    if (!address)
        throw std::runtime_error("hypr-sdr-white: CM settings symbol unavailable");
    cmSettingsHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(&cmSettings));
    if (!cmSettingsHook || !cmSettingsHook->hook())
        throw std::runtime_error("hypr-sdr-white: failed to attach CM settings hook");
    // Normal reload retains the manager's registrations; full-reset replaces it.
    configReloadListener = Event::bus()->m_events.config.preReload.listen([] {
        if (!registerConfig())
            LOG(Log::ERR, "hypr-sdr-white: failed to restore config and refresh API");
    });
    // Fires after every monitor rule application, config reloads included.
    // Also refresh the plugin override, which is not part of the output description.
    monitorRulesListener = Config::monitorRuleMgr()->m_events.stateReloaded.listen([] { notifyClients(); });
    notifyClients();
    return {"hypr-sdr-white", "SDR blending-space feedback for HDR Wayland clients", "wilf", "0.4.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (preferredHook)
        preferredHook->unhook();
    preferredHook = nullptr;
    if (cmSettingsHook)
        cmSettingsHook->unhook();
    cmSettingsHook = nullptr;
    configReloadListener.reset();
    monitorRulesListener.reset();
    registeredConfigManager.reset();
    notifyClients();
}
