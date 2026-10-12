#include <algorithm>
#include <stdexcept>
#include <memory>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
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
std::unique_ptr<CConfigValue<Config::INTEGER>> referenceWhite;
CHyprSignalListener monitorRulesListener;
using PreferredFn = PImageDescription (*)(CWLSurfaceResource*);
using CMSettingsFn = Render::SCMSettings (*)(Render::IHyprRenderer*, PImageDescription, PImageDescription, SP<CWLSurfaceResource>, bool, float, int, bool);

bool isHDR(eTransferFunction tf) {
    return tf == CM_TRANSFER_FUNCTION_ST2084_PQ || tf == CM_TRANSFER_FUNCTION_HLG;
}

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
        const auto configured = **referenceWhite;
        const auto requested = configured > 0 ? configured : monitor->m_sdrMaxLuminance;
        const auto peak = std::max(1U, source.masteringLuminances.max > 0 ? source.masteringLuminances.max : source.luminances.max);
        const auto white = static_cast<uint32_t>(std::clamp<Config::INTEGER>(requested, 1, peak));
        auto result = source;
        result.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22;
        result.transferFunctionPower = 1.0f;
        result.primariesNameSet = true;
        result.primariesNamed = CM_PRIMARIES_SRGB;
        result.primaries = NColorPrimaries::BT709;
        // Same as the original patch: SDR clients encode black at 0 nits.
        result.luminances = {.min = 0, .max = white, .reference = white};
        return CImageDescription::from(result);
    }
    return original;
}

// Hyprland decides whether to tonemap from the source peak, but hands the shader that peak scaled
// by dstRef/srcRef while leaving pixels in absolute nits. Once a PQ client's reference white sits
// well above the output's (Chrome follows our preferred white), the scaled peak drops under the
// output max and the shader hard-clips highlights instead of rolling them off. Undo the scaling.
Render::SCMSettings cmSettings(Render::IHyprRenderer* renderer, PImageDescription source, PImageDescription target, SP<CWLSurfaceResource> surface,
                               bool modifySDR, float sdrMinLuminance, int sdrMaxLuminance, bool shouldUseSurface) {
    auto settings = reinterpret_cast<CMSettingsFn>(cmSettingsHook->m_original)(renderer, source, target, surface, modifySDR, sdrMinLuminance, sdrMaxLuminance,
                                                                                shouldUseSurface);
    // Only HDR client content onto an HDR target (the output, or the linear work buffer HDR outputs
    // composite into); HDR-to-SDR paths (screencopy, SDR outputs) are left alone.
    // Mode 2 (clamp) already passes dstMaxLuminance unscaled.
    const bool hdrTarget = (isHDR(settings.targetTF) || settings.targetTF == CM_TRANSFER_FUNCTION_EXT_LINEAR) && settings.dstMaxLuminance > settings.dstRefLuminance;
    if (settings.needsTonemap && settings.tonemapMode != 2 && settings.dstRefLuminance > 0 && isHDR(settings.sourceTF) && hdrTarget)
        settings.maxLuminance = settings.maxLuminance * settings.srcRefLuminance / settings.dstRefLuminance;
    return settings;
}

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
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE pluginHandle) {
    handle = pluginHandle;
    if (HyprlandAPI::getHyprlandVersion(handle).hash != GIT_COMMIT_HASH)
        throw std::runtime_error("hypr-sdr-white: compositor/header commit mismatch");
    if (!HyprlandAPI::addConfigValueV2(handle, makeShared<Config::Values::CIntValue>(
            "plugin:hypr_sdr_white:reference_luminance", "Client SDR white in nits; 0 follows output SDR max", Config::INTEGER{0})))
        throw std::runtime_error("hypr-sdr-white: failed to register reference white");
    referenceWhite = std::make_unique<CConfigValue<Config::INTEGER>>("plugin:hypr_sdr_white:reference_luminance");
    void* address = findFunction("getPreferredImageDescription", "CWLSurfaceResource::getPreferredImageDescription()");
    if (!address)
        throw std::runtime_error("hypr-sdr-white: preferred-description symbol unavailable");
    preferredHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(&preferred));
    if (!preferredHook || !preferredHook->hook())
        throw std::runtime_error("hypr-sdr-white: failed to attach preferred-description hook");
    address = findFunction("getCMSettings",
                           "Render::IHyprRenderer::getCMSettings(Hyprutils::Memory::CWeakPointer<NColorManagement::CImageDescription const>, "
                           "Hyprutils::Memory::CWeakPointer<NColorManagement::CImageDescription const>, Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, "
                           "bool, float, int, bool)");
    if (!address)
        throw std::runtime_error("hypr-sdr-white: CM settings symbol unavailable");
    cmSettingsHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(&cmSettings));
    if (!cmSettingsHook || !cmSettingsHook->hook())
        throw std::runtime_error("hypr-sdr-white: failed to attach CM settings hook");
    if (!HyprlandAPI::addLuaFunction(handle, "hypr_sdr_white", "refresh", refresh))
        throw std::runtime_error("hypr-sdr-white: failed to register refresh API");
    // Fires after every monitor rule application, config reloads included. sdr_max_luminance is
    // not part of the output description, so changing it alone never triggers preferred_changed.
    monitorRulesListener = Config::monitorRuleMgr()->m_events.stateReloaded.listen([] { notifyClients(); });
    notifyClients();
    return {"hypr-sdr-white", "SDR blending-space feedback for HDR Wayland clients", "wilf", "0.3.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (preferredHook)
        preferredHook->unhook();
    preferredHook = nullptr;
    if (cmSettingsHook)
        cmSettingsHook->unhook();
    cmSettingsHook = nullptr;
    monitorRulesListener.reset();
    referenceWhite.reset();
    notifyClients();
}
