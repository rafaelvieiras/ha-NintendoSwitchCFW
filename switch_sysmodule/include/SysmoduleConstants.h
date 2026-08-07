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

// The real, firmware-owned Program ID for the Album applet. Atmosphere content override
// (/atmosphere/contents/<id>/) works the same way for real system titles as it does for
// custom sysmodules - dropping our own exefs.nsp+main.npdm there makes anything that
// launches this program id (including pmshellLaunchProgram, unconditionally, no key
// combo needed) run our code instead. This is the same mechanism Homebrew Loader uses to
// take over Album for hbmenu, just triggered by core instead of a held button. Used only
// as a short-lived install (see switch_app's launcher-mode build target) to get a
// genuine AppletType_Application context long enough to call
// appletRequestLaunchApplication for titles core's own pmshellLaunchProgram can't launch
// correctly (patched titles - see the NcaBaseStorageOutOfRangeC note near
// launch_program_by_id in switch_sysmodule/main.cpp).
#define ALBUM_OVERRIDE_PROGRAM_ID "010000000000100D"

// Request/status handoff files for the Album-override launcher flow. core writes the
// request before installing the override and launching Album; switch_app (launcher
// build) reads it on boot, does the real launch, and writes status back. core polls the
// status file (there's no other IPC between these two independent processes).
#define LAUNCH_REQUEST_PATH "sdmc:/config/HomeAssistantSwitch/launch_request.json"
#define LAUNCH_STATUS_PATH "sdmc:/config/HomeAssistantSwitch/launch_status.json"

#endif // SYSMODULE_CONSTANTS_H
