# hypr-sdr-white

> [!WARNING]
> This plugin is an experimental workaround, not an official Hyprland fix or a
> stable, supported solution. It hooks internal compositor functions and must
> be rebuilt and checked after upgrades. Physical HDR appearance is not validated.

The **master** branch targets the latest official **hyprwm/Hyprland release,
v0.57.0**, commit `0aa6745f705f2c4b0a5004eb019ef76e5bbdb426`, adapting the preferred-client-description
part of [erikwb's patch](https://github.com/erikwb/Hyprland/commit/cc0c5094ffdee16c820162a66ce9dbeb77cdbe9e).
Users of the gfhdhytghd/Hyprland fork must use the **my-hyprland-master** branch
instead. This branch does not target that fork or Hyprland 0.56.2.
The fork and upstream release have different ABIs despite sharing a version number.

On HDR outputs, Wayland surface feedback uses sRGB primaries, gamma 2.2,
and min/max/reference luminance 0/N/N. By default N is the output's
`sdr_max_luminance` (80 nits when unset), capped at the output mastering peak.
Official 0.57 keeps this SDR white separate from the HDR description's
203-nit reference; it has no monitor `reference_luminance` option.
Physical HDR output metadata remains unchanged.
It does not infer SDR white from EDID: existing monitor configuration is retained.

Like the original patch, preferred encoding has max equal to reference white;
the original target/mastering peak and CLL/FALL remain intact. Chromium can
still infer HDR headroom from target peak versus reference white. The local
0.56.2 protocol test observed Chromium switch to PQ after receiving this feedback.
This is not proof of HDR video playback: test real HDR content separately.
SDR outputs and fullscreen custom descriptions are not rewritten.

### Tonemapping on official 0.57.0

Official 0.57 still compares the unscaled source peak with the target peak to
decide whether tonemapping is needed, then scales the shader's peak by
target-reference / source-reference. The shader receives absolute-nit pixels
without that scaling. For example, source peak/reference 1000/446 and target
peak/reference 630/203 produce a shader peak of about 455 nits, which takes
the shader's hard-clamp path instead of its highlight rolloff.

This branch therefore restores the `getCMSettings` hook with the official
`CRenderContext&` argument and preserves the absolute peak for PQ, HLG and
EXT_LINEAR sources onto HDR targets. Explicit PQ/HLG targets are covered;
an EXT_LINEAR target must have HDR headroom. Disabled tonemapping, clamp mode
and direct SDR-target calculations are left unchanged. Mirror/capture paths
that consume a shared post-tonemap result can inherit the corrected result.

Source evidence at the pinned official release:

- [SDR monitor option](https://github.com/hyprwm/Hyprland/blob/0aa6745f705f2c4b0a5004eb019ef76e5bbdb426/src/config/lua/bindings/LuaBindingsConfigRules.cpp#L158-L160)
- [Surface description selection](https://github.com/hyprwm/Hyprland/blob/0aa6745f705f2c4b0a5004eb019ef76e5bbdb426/src/protocols/core/Compositor.cpp#L710-L732)
- [CM settings and peak scaling](https://github.com/hyprwm/Hyprland/blob/0aa6745f705f2c4b0a5004eb019ef76e5bbdb426/src/render/Renderer.cpp#L2138-L2207)
- [Pixel conversion before tonemapping](https://github.com/hyprwm/Hyprland/blob/0aa6745f705f2c4b0a5004eb019ef76e5bbdb426/src/render/shaders/glsl/cm_helpers.glsl#L234-L249)

Use Hyprland's window rules to select tonemapping behavior. Nested lifecycle
and numerical settings checks do not validate on-screen HDR appearance.
The historical 0.56.2 client results below do not establish 0.57.0 behavior.

## Installation with hyprpm

Run these commands as your normal desktop user in the Hyprland session:

```sh
hyprpm add https://github.com/gfhdhytghd/hypr-sdr-white
hyprpm enable hypr-sdr-white
hyprpm reload
```

`hyprpm` builds the plugin against the running Hyprland version and requests administrator authentication when needed.
Do not run the entire command as root or change the ownership of the hyprpm cache directory.
If a manually loaded copy is already active, unload it before enabling the hyprpm-managed copy and remove its startup load command.
Do not load both copies at once.

Run `hyprpm reload` at login through your existing plugin startup configuration.
Apply the configuration below only after the plugin has loaded.
After a Hyprland upgrade, run `hyprpm update` to rebuild before loading the plugin again; compatibility with newer Hyprland APIs is not guaranteed.

To disable the plugin:

```sh
hyprpm disable hypr-sdr-white
```

## Manual build for development

Build with headers matching the running compositor exactly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j 2
hyprctl plugin load "$PWD/build/libhypr-sdr-white.so"
hyprctl plugin unload "$PWD/build/libhypr-sdr-white.so"
```

The plugin rejects a compositor/header commit mismatch. It hooks two C++
functions with compile-time signature checks, so rebuild after any Hyprland
update. Unload restores original feedback and tonemapping settings and
notifies connected clients. The Lua refresh API and plugin option are restored
after `hyprctl reload full-reset` and remain available after ordinary reloads.

## Configuration

No plugin configuration is needed: advertised client SDR white follows each
output's `sdr_max_luminance`, capped at its mastering peak.
Changing it, through the config, a reload or `hl.monitor(...)` at runtime, resends client feedback automatically.

The plugin-specific option below overrides the white with a fixed value. Leave it at 0 unless you have a reason:
The override changes advertised client encoding white only; it does not change
the monitor's SDR conversion settings or its HDR description's reference.
Change the monitor's `sdr_max_luminance` option to change compositor SDR white.

```lua
hl.config({
    plugin = { hypr_sdr_white = { reference_luminance = 0 } }, -- nits; 0 follows monitor sdr_max_luminance
})
```

After changing the plugin's `reference_luminance` override at runtime without
a reload, call `hl.plugin.hypr_sdr_white.refresh()`.
The feedback applies to all color-managed Wayland clients on HDR outputs, not just Chrome.
Existing clients may need a restart if they retain the old description after feedback refresh.

Historical local validation on Hyprland 0.56.2 / Chrome 154 (not rerun on this branch):

- Build and plugin load succeeded with matching compositor/header commit.
- Physical output remains PQ, max 630 / reference 203, 10 bit.
- Client preferred feedback becomes gamma 2.2, sRGB, 0/350/350, with original
  target max 630 and CLL/FALL preserved.
- Chrome submits PQ with reference 350; after display initialization, the
  test page reports `dynamic-range: high` and `color-gamut: p3` both true.
- Physical brightness acceptance remains dependent on user observation.
