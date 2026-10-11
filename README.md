# hypr-sdr-white

> [!WARNING]
> This plugin has not been audited or reviewed. It is a temporary workaround,
> not an official Hyprland fix or a stable, supported solution. It hooks an
> internal compositor function and must be rebuilt and checked after upgrades.

Experimental Hyprland 0.56.2 plugin adapting the preferred-client-description
part of [erikwb's patch](https://github.com/erikwb/Hyprland/commit/cc0c5094ffdee16c820162a66ce9dbeb77cdbe9e).

On HDR outputs, Wayland surface feedback uses sRGB primaries, gamma 2.2,
and min/max/reference luminance 0/N/N. N is the plugin's independent
`reference_luminance` (default 203 nits), capped
at the output mastering peak. Set 0 to follow output `sdr_max_luminance`.
Physical HDR output metadata remains unchanged.
It does not infer SDR white from EDID: existing monitor configuration is retained.

Like the original patch, preferred encoding has max equal to reference white;
the original target/mastering peak and CLL/FALL remain intact. Chromium can
still infer HDR headroom from target peak versus reference white. The local
protocol test observed Chromium switch to PQ after receiving this feedback.
This is not proof of HDR video playback: test real HDR content separately.
SDR outputs and fullscreen custom descriptions are not rewritten.

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

Lua configuration, after the plugin has loaded:

```lua
hl.config({ plugin = { hypr_sdr_white = { reference_luminance = 203 } } })
```

After changing the value, refresh existing clients without restarting them:

```lua
hl.plugin.hypr_sdr_white.refresh()
``` It applies to all color-managed Wayland clients
on HDR outputs, not just Chrome. Desktop SDR mapping stays independent.

Local validation on Hyprland 0.56.2 / Chrome 154:

- Build and plugin load succeeded with matching compositor/header commit.
- Physical output remains PQ, max 630 / reference 203, 10 bit.
- Client preferred feedback becomes gamma 2.2, sRGB, 0/350/350, with original
  target max 630 and CLL/FALL preserved.
- Chrome submits PQ with reference 350; after display initialization, the
  test page reports `dynamic-range: high` and `color-gamut: p3` both true.
- Physical brightness acceptance remains dependent on user observation.
