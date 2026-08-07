# Home Assistant Switch API Documentation (v2.0)

This document describes the HTTP API provided by the Home Assistant Sysmodule.

## General Information
- **Port**: 1337 (Default)
- **Protocol**: HTTP/1.1
- **Auth**: Requires `X-API-Token` header for all routes except `/info` and `/health`.

## Endpoints

### GET /info
Returns system information. No authentication required.
**Response**:
```json
{
  "version": "0.2.1",
  "battery": 85,
  "charging": true,
  "net": "Online"
}
```

### GET /screenshot
Captures a live screenshot of the Switch screen.
**Auth**: Required
**Response**: Binary `image/bmp` (1280x720, 24-bit).

### POST /button
Simulates controller inputs.
**Auth**: Required
**Payload**:
- Single button: `{"button": "A"}`
- Macro sequence: `{"sequence": [{"button": "A", "hold_ms": 100}, {"button": "WAIT", "hold_ms": 200}, {"button": "B"}]}`
- Joystick: `{"left_stick": {"x": 32767, "y": 0}}` (Range -32767 to 32767)

### GET /titles
Returns the list of installed titles (SD card + NAND user storage), resolved via the
`ncm` content-meta database rather than the `ns` ApplicationRecord list (which doesn't
reflect CFW-sideloaded titles).
**Auth**: Required
**Response**:
```json
[{"title_id": "0x0100000000010000", "name": "SUPER MARIO ODYSSEY"}]
```

### POST /sleep
Puts the console into sleep mode.
**Auth**: Required

### POST /command
Executes advanced system-level actions.
**Auth**: Required
**Payload**:
- Reboot: `{"action": "reboot"}`
- Shutdown: `{"action": "shutdown"}`
- Launch Title: `{"action": "launch_app", "title_id": "0100000000010000"}`
  - Known limitation: titles with an installed update/patch currently fail to launch
    directly (`fs` result `NcaBaseStorageOutOfRangeC`) - the storage-resolution this uses
    doesn't account for patch layering the way `am`'s own launch path does. Titles with
    no update installed launch fine.
  - **EXPERIMENTAL, currently broken**: for the patched-title case above, this
    automatically falls back to a second mechanism (the "Album-override launcher") that
    briefly takes over the Album applet to get a real Application context and launch
    correctly. The takeover itself works (confirmed on hardware), but the *next* HTTP
    request handled by core afterwards intermittently receives a corrupted request
    body and fails to parse (`{"error": "Missing action"}` even for a trivial, correctly
    received payload) - not yet root-caused. See the note in `SysmoduleConstants.h` near
    `ALBUM_OVERRIDE_PROGRAM_ID` and the project doc for the investigation so far. Until
    this is fixed, expect `POST /command`/`GET /launch_status` to occasionally need a
    retry after triggering `launch_app` on a patched title.

### GET /launch_status
Reflects the state of the last Album-override launch fallback (see the `launch_app`
note above) - `{"state": "idle"}` when none is pending, `{"state": "launching", ...}`
while switch_app's launcher build is handling the handoff, `{"state": "launched", ...}`
or `{"state": "error", ...}` once it reports a terminal result.
**Auth**: Required

### GET /logs
Returns the latest sysmodule logs in JSON format.
**Auth**: Required

## Orchestrator control port (8276)

Since `switch_sysmodule_orchestrator/`, the sysmodule ("core", this document's API) is
launched and supervised by a separate, minimal orchestrator process instead of being
launched directly by Atmosphere's boot2. The orchestrator exposes its own tiny port,
independent of core's, so it keeps working even when a bad core deploy can't:

### POST /self_restart
Terminates and relaunches core. Same `X-API-Token` auth as the main API.
```
curl -X POST -H "X-API-Token: <token>" http://<switch-ip>:8276/self_restart
```
No physical console reboot is needed to pick up a redeployed core binary - upload the
new `exefs.nsp` to `sdmc:/atmosphere/contents/010000000000CAFE/exefs.nsp` and call this.

The orchestrator also runs its own health check against core's main port every 15s and
restarts core automatically after 3 consecutive failures, independent of this endpoint.

**Fix history:** earlier builds of the orchestrator terminated core and relaunched it
after a fixed 500ms sleep, which was too short - `pm:shell` doesn't guarantee a
terminated process is fully torn down (and its `program_id` slot released) the instant
`TerminateProgram` returns, so the immediately-following `LaunchProgram` for the same
`program_id` would silently no-op against the still-alive old process. Fixed by
bounding the wait on `pm:shell`'s process-event queue (up to ~3s, polling
`pmshellGetProcessEventInfo`/`pmshellCleanupProcess`) before relaunching. In practice,
on this Atmosphere build the process-exit event never actually arrives on that queue
for this kind of Atmosphere-content-override process (the wait always times out, and
`CleanupProcess` returns a Libnx-side `IncompatSysVer`-class error) - so what actually
fixes it is the wait itself taking ~3-4s instead of 500ms, not the theoretically-correct
cleanup call succeeding. Validated on hardware: 3 consecutive `self_restart` calls with
a distinct rebuilt binary each time, all picked up correctly, no reboot needed.
