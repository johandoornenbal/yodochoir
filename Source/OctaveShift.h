#pragma once
#include <vector>
#include <functional>
namespace choir {
enum class PitchEngine {rubberBand=0,signalsmith=1};
// Offline mono processing. Empty result means cancellation. Output has precisely
// the input frame count; latency is compensated before mixing into the choir.
std::vector<float> shiftOctave(const std::vector<float>& input,double rate,int semitones,
                              PitchEngine engine,int seed,const std::function<bool()>& cancelled={});
}
