#include <algorithm>
#include <stdexcept>
#include <memory>
#include <type_traits>
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
// The lookup matches the demangled name only, which omits the return type.
static_assert(std::is_same_v<decltype(&CWLSurfaceResource::getPreferredImageDescription), PImageDescription (CWLSurfaceResource::*)()>);

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
        // In fork 0.57, reference_luminance anchors both HDR and SDR white.
        // Use the accessor so an unset monitor value follows the compositor's 203-nit default.
        const auto requested = configured > 0 ? configured : monitor->referenceLuminance();
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

// No getCMSettings hook on fork 0.57: CCMSettingsCache anchors both the source
// peak and the decoded pixels to the target reference white. Undoing only the
// peak scaling would make it disagree with the pixels again.

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
            "plugin:hypr_sdr_white:reference_luminance", "Client SDR white in nits; 0 follows output reference luminance", Config::INTEGER{0})))
        throw std::runtime_error("hypr-sdr-white: failed to register reference white");
    referenceWhite = std::make_unique<CConfigValue<Config::INTEGER>>("plugin:hypr_sdr_white:reference_luminance");
    void* address = findFunction("getPreferredImageDescription", "CWLSurfaceResource::getPreferredImageDescription()");
    if (!address)
        throw std::runtime_error("hypr-sdr-white: preferred-description symbol unavailable");
    preferredHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(&preferred));
    if (!preferredHook || !preferredHook->hook())
        throw std::runtime_error("hypr-sdr-white: failed to attach preferred-description hook");
    if (!HyprlandAPI::addLuaFunction(handle, "hypr_sdr_white", "refresh", refresh))
        throw std::runtime_error("hypr-sdr-white: failed to register refresh API");
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
    monitorRulesListener.reset();
    referenceWhite.reset();
    notifyClients();
}
