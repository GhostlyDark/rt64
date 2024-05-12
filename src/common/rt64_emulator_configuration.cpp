//
// RT64
//

#include "rt64_emulator_configuration.h"

namespace RT64 {
    // EmulatorConfiguration

    EmulatorConfiguration::EmulatorConfiguration() {
        framebuffer.renderToRAM = false;
        framebuffer.copyWithGPU = true;
    }
};