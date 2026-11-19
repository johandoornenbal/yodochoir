# Source and license notes

## ToTape Studio UI and installation workflow

The rotary LookAndFeel (`Source/Look.h`), output needle display
(`Source/OutputMeter.h`), cream/charcoal/brass layout and staged Mac installer
are adapted from Johan Doornenbal's ToTape Studio, commit
`aae9dbe86de5334558ca6c9e3f7cd89ef5874499`:
https://github.com/johandoornenbal/tape

ToTape's MIT license and wrapper notices are reproduced in
`third_party/ToTape-LICENSE`. Its UI has ButterComp Studio origins.
The new choir engine and workflow are original to YodoChoir; this plugin does
not incorporate ToTape audio processing. Its room return uses Airwindows kWoodRoom,
as described below.

## JUCE 8.0.12

Pinned commit `29396c22c93392d6738e021b83196283d6e4d850`:
https://github.com/juce-framework/JUCE/tree/29396c22c93392d6738e021b83196283d6e4d850

JUCE is licensed separately under AGPLv3 or a commercial JUCE license. This
project's MIT license does not relicense JUCE. Comply with the selected license
when distributing the plugin, including source requirements when using AGPLv3.
Retain JUCE's bundled codec and VST3 notices.

## Demonstration audio

The built-in demo and `docs/audio` examples are generated mathematically by
`choir::demo`; they are synthetic vowels, not recordings of a singer. They are
included under the project's MIT license.

## Airwindows kWoodRoom

Copyright Chris Johnson / Airwindows, MIT. Original sources are retained verbatim
under `third_party/airwindows/kWoodRoom`, from upstream commit
`7fd5f5cae262a41e6dd7d9a83d60c46e84d3092a`:
https://github.com/airwindows/airwindows/tree/7fd5f5cae262a41e6dd7d9a83d60c46e84d3092a/plugins/WinVST/kWoodRoom

`Source/kWoodRoomEngine.*` is the same SDK-free adapter used in AirVerb. Native
initialization, delay constants and processing expressions are retained; host
metadata is replaced by reset/sample-rate access and clamped normalized setters.
The existing deterministic seeds (`0x12345678` / `0x87654321`) permit exact upstream
comparisons. The reference suite compiles the untouched VST2 implementation using
an independently written test shim, without the Steinberg VST2 SDK.

The choir's Room control is an external blend of the fully wet engine at its native
default settings. The render-tail bound and final fade are outside Airwindows DSP.
The original MIT notice is in `third_party/airwindows/LICENSE`.

## Reaper host integration

`Source/ReaperBridge.cpp` declares the small host-interface ABI documented in
[Cockos' Reaper VST3 interfaces](https://github.com/justinfrankel/reaper-sdk/blob/main/sdk/reaper_vst3_interfaces.h)
and resolves the public [Reaper API](https://www.reaper.fm/sdk/reascript/reascripthelp.html).
No Reaper implementation or SDK library is distributed. VST3 base interfaces come
from JUCE's bundled SDK and retain its notices.
