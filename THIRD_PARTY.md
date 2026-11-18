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
not incorporate ToTape or Airwindows audio processing.

## JUCE 8.0.12

Pinned commit `29396c22c93392d6738e021b83196283d6e4d850`:
https://github.com/juce-framework/JUCE/tree/29396c22c93392d6738e021b83196283d6e4d850

JUCE is licensed separately under AGPLv3 or a commercial JUCE license. This
project's MIT license does not relicense JUCE. Comply with the selected license
when distributing the plugin, including source requirements when using AGPLv3.
Retain JUCE's bundled codec and VST3 notices.

## Product reference

https://amiaudio.io/ inspired the import → choir → export workflow. YodoChoir
uses its own sample-based ensemble DSP. No I AM CHOIR software, models,
recordings, assets or branding are included. There is no affiliation.

## Demonstration audio

The built-in demo and `docs/audio` examples are generated mathematically by
`choir::demo`; they are synthetic vowels, not recordings of a singer. They are
included under the project's MIT license.
