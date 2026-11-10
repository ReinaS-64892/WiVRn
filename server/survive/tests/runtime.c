// SPDX-License-Identifier: GPL-3.0-or-later
#include "survive.h"
#include <stdio.h>

extern survive_driver_fn GetDriver(const char *name);

int main(void)
{
    // No context or USB access: constructors must register these in the private
    // DSO itself, with no runtime plugin discovery or linker dead-stripping.
    const char *required[] = {"DriverRegHTCVive", "DriverRegPlayback",
        "DriverRegGlobalSceneSolver", "DisambiguatorStateBased", "PoserMPFIT"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!GetDriver(required[i])) {
            fprintf(stderr, "Bundled libsurvive component is missing: %s\n", required[i]);
            return 1;
        }
    }
    puts("Bundled libsurvive transport, playback, scene solver, disambiguator and poser: passed");
    return 0;
}
