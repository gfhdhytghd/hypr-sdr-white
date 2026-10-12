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

namespace {
using namespace NColorManagement;
HANDLE handle = nullptr;
CFunctionHook* preferredHook = nullptr;
std::unique_ptr<CConfigValue<Config::INTEGER>> referenceWhite;
CHyprSignalListener monitorRulesListener;
using PreferredFn = PImageDescription (*)(CWLSurfaceResource*);

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
    void* address = nullptr;
    for (const auto& match : HyprlandAPI::findFunctionsByName(handle, "getPreferredImageDescription")) {
        if (match.demangled == "CWLSurfaceResource::getPreferredImageDescription()") {
            address = match.address;
            break;
        }
    }
    if (!address)
        throw std::runtime_error("hypr-sdr-white: preferred-description symbol unavailable");
    preferredHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(&preferred));
    if (!preferredHook || !preferredHook->hook())
        throw std::runtime_error("hypr-sdr-white: failed to attach preferred-description hook");
    if (!HyprlandAPI::addLuaFunction(handle, "hypr_sdr_white", "refresh", refresh))
        throw std::runtime_error("hypr-sdr-white: failed to register refresh API");
    // Fires after every monitor rule application, config reloads included. sdr_max_luminance is
    // not part of the output description, so changing it alone never triggers preferred_changed.
    monitorRulesListener = Config::monitorRuleMgr()->m_events.stateReloaded.listen([] { notifyClients(); });
    notifyClients();
    return {"hypr-sdr-white", "SDR blending-space feedback for HDR Wayland clients", "wilf", "0.3.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (preferredHook)
        preferredHook->unhook();
    preferredHook = nullptr;
    monitorRulesListener.reset();
    referenceWhite.reset();
    notifyClients();
}
