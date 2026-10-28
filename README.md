# hypr-sdr-white

> [!WARNING]
> This plugin has not been audited or reviewed. It is a temporary workaround,
> not an official Hyprland fix or a stable, supported solution. It hooks an
> internal compositor function and must be rebuilt and checked after upgrades.

Experimental plugin for **gfhdhytghd/Hyprland fork v0.57.0**, commit
`f6725e43eed50eaa061aa25fe7c9f8cdc9d355e2`, adapting the preferred-client-description
part of [erikwb's patch](https://github.com/erikwb/Hyprland/commit/cc0c5094ffdee16c820162a66ce9dbeb77cdbe9e).
This branch does not target upstream hyprwm v0.57.0 (`0aa6745f`) or Hyprland 0.56.2.
The fork and upstream release have different ABIs despite sharing a version number.

On HDR outputs, Wayland surface feedback uses sRGB primaries, gamma 2.2,
and min/max/reference luminance 0/N/N. By default N is the output's
`reference_luminance` (203 nits when unset), the reference white the fork uses
for both SDR and HDR content, capped at the output mastering peak.
Physical HDR output metadata remains unchanged.
It does not infer SDR white from EDID: existing monitor configuration is retained.

Like the original patch, preferred encoding has max equal to reference white;
the original target/mastering peak and CLL/FALL remain intact. Chromium can
still infer HDR headroom from target peak versus reference white. The local
0.56.2 protocol test observed Chromium switch to PQ after receiving this feedback.
This is not proof of HDR video playback: test real HDR content separately.
SDR outputs and fullscreen custom descriptions are not rewritten.

### Tonemapping on fork 0.57.0

The fork anchors decoded pixels and source peak to the target reference white
using the same target/source ratio. It also uses that anchored peak to decide
whether tonemapping is needed. The old plugin correction, which undid only the
peak scaling, is therefore removed. This branch hooks only preferred surface
feedback and leaves the fork's tonemapping, SDR adjustment and capture paths
unchanged. No `getCMSettings` symbol lookup is performed.

The fork still returns the output's PQ/BT.2020 description for ordinary HDR
surface feedback; its protocol helper only substitutes an sRGB description for
ICC outputs. The gamma 2.2 / sRGB feedback hook is therefore still needed for
this plugin's encoding policy. See `PORT_057_REPORT.md` for source evidence.

Use the fork's window rules to select its native tonemapping behavior. This
port has no runtime or on-screen HDR validation; the historical 0.56.2 results
below do not establish 0.57.0 behavior.

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

The plugin rejects a compositor/header commit mismatch. It hooks one C++
function, so rebuild after any Hyprland update. Unload restores original
feedback and notifies connected clients.

## Configuration

No plugin configuration is needed: advertised client SDR white follows each
output's `reference_luminance`, capped at its mastering peak.
Changing it, through the config, a reload or `hl.monitor(...)` at runtime, resends client feedback automatically.

The plugin-specific option below overrides the white with a fixed value. Leave it at 0 unless you have a reason:
The override changes advertised client encoding white only; it does not change
the output reference white. The fork still anchors incoming content to the
output's `reference_luminance`. Change the monitor option to change output white.

```lua
hl.config({
    plugin = { hypr_sdr_white = { reference_luminance = 0 } }, -- nits; 0 follows monitor reference_luminance
})
```

After changing `reference_luminance` at runtime without a reload, call `hl.plugin.hypr_sdr_white.refresh()`.
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
