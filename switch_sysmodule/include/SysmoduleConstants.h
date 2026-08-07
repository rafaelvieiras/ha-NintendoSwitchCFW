#ifndef SYSMODULE_CONSTANTS_H
#define SYSMODULE_CONSTANTS_H

// The Program ID for the sysmodule ("core" - the always-on HTTP API).
// Atmosphere expects the NSO to be at sdmc:/atmosphere/contents/<ID>/exefs/main
#define SYSMODULE_PROGRAM_ID "010000000000CAFE"

// The Program ID for the orchestrator (switch_sysmodule_orchestrator/) - the process
// Atmosphere's boot2 actually launches. It launches and supervises core, which is no
// longer boot2-launched directly (see switch_sysmodule_orchestrator/flags/boot2.flag
// vs. the absence of one under switch_sysmodule/).
#define ORCHESTRATOR_PROGRAM_ID "010000000000CAF0"

// Standard filenames for the release artifacts
#define NRO_FILENAME "homeassistant.nro"
#define NSO_FILENAME "homeassistant_sysmodule.nso"

#endif // SYSMODULE_CONSTANTS_H
