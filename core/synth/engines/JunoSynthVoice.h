#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
std::unique_ptr<InstrumentType> createJunoSynth();
} // namespace polysynth
