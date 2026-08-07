#include <switch.h>
#include <switch/services/set.h>
#include <switch/services/psm.h>
#include <switch/services/ts.h>
#include <switch/services/nifm.h>
#include <switch/services/fs.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <switch/services/spsm.h>
#include <switch/services/ns.h>
#include <switch/services/ncm.h>
#include <switch/services/applet.h>
#include <switch/services/apm.h>
#include <switch/services/pm.h>
#include <switch/services/lr.h>
#include <switch/services/hiddbg.h>
#include <fcntl.h>
#include <thread>
#include <string>
#include <atomic>
#include "include/json.hpp"

using json = nlohmann::json;

#ifndef APP_VERSION
#define APP_VERSION "0.2.4-dev"
#endif

extern "C" {
    u32 __check_mask_save;
    
    // Sysmodules must define their own heap
    void __libnx_initheap(void) {
        static char inner_heap[0x200000]; // 2MB is plenty for basic JSON/Curl
        extern char* fake_heap_start;
        extern char* fake_heap_end;
        fake_heap_start = inner_heap;
        fake_heap_end   = inner_heap + sizeof(inner_heap);
    }
    
    void __appInit(void) {
        Result rc = smInitialize();
        if (R_FAILED(rc)) fatalThrow(rc);
        
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc)) {
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            }
            setsysExit();
        }
    }
    
    void __appExit(void) {
        fsdevUnmountAll();
        fsExit();
        smExit();
    }
}

// Optimization flags for sysmodules
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

extern "C" {
    Result hiddbgSetAutoPilotVirtualPadState(s8 AbstractedVirtualPadId, const HiddbgAbstractedPadState *state);
}

#include "include/ConfigManager.h"
#include "include/Logger.h"
#include "include/SysmoduleConstants.h"

Logger g_logger;

#ifndef IP_ADD_MEMBERSHIP
#define IP_ADD_MEMBERSHIP 12
#endif

// Manually define ip_mreq since it might be missing in some libnx environments
struct switch_ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

void mdns_responder() {
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0) return;

    int reuse = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5353);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sd);
        return;
    }

    struct switch_ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    unsigned char buffer[1024];
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int n = recvfrom(sd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
        if (n > 12) {
            // Check for "_homeassistant" label in query
            if ((buffer[2] & 0x80) == 0 && strstr((char*)buffer + 12, "_homeassistant")) {
                u32 ip = 0; nifmGetCurrentIpAddress(&ip);
                unsigned char response[] = {
                    0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                    0x06, 's', 'w', 'i', 't', 'c', 'h', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
                    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 
                    (unsigned char)(ip & 0xFF), (unsigned char)((ip >> 8) & 0xFF), 
                    (unsigned char)((ip >> 16) & 0xFF), (unsigned char)((ip >> 24) & 0xFF)
                };
                struct sockaddr_in target_addr;
                target_addr.sin_family = AF_INET;
                target_addr.sin_port = htons(5353);
                target_addr.sin_addr.s_addr = inet_addr("224.0.0.251");
                sendto(sd, response, sizeof(response), 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
            }
        }
        svcSleepThread(500000000ULL); // 0.5s yield
    }
}

// trace() (defined near main(), below) is a debug-mode-gated logger: a no-op unless
// ConfigManager's "debug" flag is set in settings.json (toggle it + POST
// /reload_config to take effect without a reboot). Forward-declared here so the
// request-path functions above main() in the file can call it too.
void trace(const char* msg);

// The server handles one connection at a time (see the accept loop in main() - a
// std::thread per connection isn't usable here). A select()-with-timeout on just the
// first recv() only bounded waiting for the client's request to *arrive* - every
// subsequent recv()/write() in handle_client() (including all the response writes)
// used to be a plain blocking call with no timeout. A client that connects and then
// never reads its response (observed with a stalled `docker exec curl`) blocks write()
// forever, which - since there's only ever one connection being served - wedges the
// entire HTTP server for every other client until the console is rebooted.
//
// Tried SO_RCVTIMEO/SO_SNDTIMEO first (simpler - set once per connection instead of
// guarding every call). Verified on real hardware that it did NOT fix the hang - the
// Switch's BSD sockets service may not actually honor those options, even though
// libnx's headers define them. select() is what actually protected the original first
// read, so use it explicitly around every recv()/write() instead of trusting
// setsockopt() to do it implicitly.
//
// Found the actual root cause afterwards via trace()-instrumented builds: select()
// correctly reports the socket as writable, but the write() call right after it can
// still block indefinitely - not a select()/timeout bug at all, but tcp_tx_buf_size et
// al. in SocketInitConfig (see main()) having been sized at the bare minimum needed to
// fit this sysmodule's 2MB heap, with no headroom for the bsd service to reclaim a
// closed connection's send buffer before the next one needed it. Fixed by growing
// those buffers (there was plenty of unused heap to grow into). select()/timeout
// guarding here is kept regardless, since write()/recv() can still legitimately block
// on a slow or unresponsive client.
static ssize_t recv_timeout(int sock, void* buf, size_t len, int timeout_sec) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv = {timeout_sec, 0};
    if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) return -1;
    return recv(sock, buf, len, 0);
}

// write() can return fewer bytes than requested (routine with the 4KB tcp_tx_buf_size
// set for this sysmodule - see the note near SocketInitConfig in main()). The previous
// version returned after a single partial write, silently dropping the rest of the
// response - the client would see a truncated body or, if it kept expecting more, hang
// or reset. Loop until everything is sent, re-arming the select() timeout for each
// chunk so a client that stops reading mid-response still can't block forever.
static ssize_t send_timeout(int sock, const void* buf, size_t len, int timeout_sec = 2) {
    trace("send_timeout: enter");
    const u8* p = (const u8*)buf;
    size_t sent = 0;
    while (sent < len) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {timeout_sec, 0};
        int sel = select(sock + 1, NULL, &fds, NULL, &tv);
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "send_timeout: select returned %d", sel);
            trace(msg);
        }
        if (sel <= 0) return sent > 0 ? (ssize_t)sent : -1;
        ssize_t n = write(sock, p + sent, len - sent);
        if (n <= 0) return sent > 0 ? (ssize_t)sent : -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

// Where the pre-staged launcher binary (switch_app's `launcher` build target - see
// switch_app/Makefile) lives permanently on the SD card, deployed there the same way as
// every other release artifact (FTP), not fetched at request time. install/uninstall
// below just copy/remove these two files into/out of Album's real content-override
// slot, immediately around each launch that needs it (see ALBUM_OVERRIDE_PROGRAM_ID).
#define LAUNCHER_ASSET_DIR "sdmc:/config/HomeAssistantSwitch/launcher"
#define ALBUM_OVERRIDE_EXEFS_DIR "sdmc:/atmosphere/contents/" ALBUM_OVERRIDE_PROGRAM_ID "/exefs"

static bool copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    return ok;
}

static bool install_album_launcher_override() {
    mkdir("sdmc:/atmosphere/contents/" ALBUM_OVERRIDE_PROGRAM_ID, 0777);
    mkdir(ALBUM_OVERRIDE_EXEFS_DIR, 0777);
    bool ok1 = copy_file(LAUNCHER_ASSET_DIR "/main", ALBUM_OVERRIDE_EXEFS_DIR "/main");
    bool ok2 = copy_file(LAUNCHER_ASSET_DIR "/main.npdm", ALBUM_OVERRIDE_EXEFS_DIR "/main.npdm");
    if (!(ok1 && ok2)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "album_launcher: install failed (main=%d npdm=%d) - is %s staged?",
                 ok1, ok2, LAUNCHER_ASSET_DIR);
        LOG_E(msg);
    }
    return ok1 && ok2;
}

// Restores the real Album applet. Deliberately removes just the two files (not the
// exefs/ or program-id directory) so a repeated install doesn't need to recreate them,
// and so a leftover empty directory here is harmless if this ever runs without a prior
// install (eg. on core's own boot, as a defensive cleanup - see main()).
static void uninstall_album_launcher_override() {
    remove(ALBUM_OVERRIDE_EXEFS_DIR "/main");
    remove(ALBUM_OVERRIDE_EXEFS_DIR "/main.npdm");
}

// How long to wait for switch_app's launcher build to report launched/error via
// launch_status.json after we hand it control (see run_launcher_mode() in
// switch_app/main.cpp) before giving up and restoring the real Album regardless.
#define ALBUM_LAUNCHER_TIMEOUT_SEC 20

// Tracked in-process (single-threaded server, no locking needed) so a pending
// Album-override launch can be finished opportunistically by whatever request happens
// to come in next - see poll_album_launcher_completion() below for why this can't just
// be a background thread waiting on it instead.
static bool g_album_override_active = false;
static u64 g_album_override_installed_tick = 0;

// Restores the real Album as soon as switch_app's launcher build reports a terminal
// state (or after ALBUM_LAUNCHER_TIMEOUT_SEC regardless, in case it never gets far
// enough to write one at all - eg. a bad npdm). Deliberately NOT a blocking wait loop:
// std::thread aborts the whole process here (see the note near mdns_responder_entry),
// and a native threadCreate() worker pool made concurrent requests actively worse on
// real hardware (see the note above handle_client()) - both already learned the hard
// way earlier in this project. Cheap enough (one bool check when idle) to just call
// unconditionally at the top of every request, piggybacking on however often the API is
// already being polled (HA's own GET /info interval, in practice) instead of adding any
// new blocking or background work.
static void poll_album_launcher_completion() {
    if (!g_album_override_active) return;

    bool done = false;
    FILE* sf = fopen(LAUNCH_STATUS_PATH, "r");
    if (sf) {
        char buf[256] = {0};
        fread(buf, 1, sizeof(buf) - 1, sf);
        fclose(sf);
        json st = json::parse(buf, nullptr, false);
        if (!st.is_discarded()) {
            std::string state = st.value("state", "");
            if (state == "launched" || state == "error") done = true;
        }
    }

    u64 elapsed_ticks = svcGetSystemTick() - g_album_override_installed_tick;
    if (!done && elapsed_ticks > (u64)ALBUM_LAUNCHER_TIMEOUT_SEC * armGetSystemTickFreq()) {
        LOG_E("album_launcher: timed out waiting for launch_status.json, restoring Album anyway");
        done = true;
    }

    if (done) {
        uninstall_album_launcher_override();
        g_album_override_active = false;
    }
}

// Gets a real Application applet session to launch `tid` correctly (base+patch layering
// included) by briefly taking over the Album applet with switch_app's launcher build -
// see ALBUM_OVERRIDE_PROGRAM_ID's comment in SysmoduleConstants.h for the full
// rationale. Only called as a fallback from launch_program_by_id() below, for the
// specific failure (NcaBaseStorageOutOfRangeC) this exists to work around - not the
// normal path for titles that already launch fine directly.
//
// Returns as soon as the launch is kicked off (rc reflects only that), not once the
// game has actually finished loading - poll GET /launch_status for the real outcome.
static Result request_launch_via_album(u64 tid) {
    poll_album_launcher_completion(); // finish any earlier pending launch first

    remove(LAUNCH_STATUS_PATH);

    char req_body[64];
    snprintf(req_body, sizeof(req_body), "{\"title_id\":\"%016llX\"}", (unsigned long long)tid);
    FILE* rf = fopen(LAUNCH_REQUEST_PATH, "w");
    if (!rf) {
        LOG_E("album_launcher: failed to write launch_request.json");
        return -1;
    }
    fwrite(req_body, 1, strlen(req_body), rf);
    fclose(rf);

    if (!install_album_launcher_override()) {
        remove(LAUNCH_REQUEST_PATH);
        return -1;
    }

    // Album is a real firmware system title (storage BuiltInSystem, unlike the
    // NcmStorageId_None case used for our own custom sysmodules) - the override above
    // swaps its code, not its launch storage.
    NcmProgramLocation loc = {strtoull(ALBUM_OVERRIDE_PROGRAM_ID, NULL, 16), NcmStorageId_BuiltInSystem, {0}};
    Result rc = pmshellLaunchProgram(0, &loc, NULL);
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "album_launcher: pmshellLaunchProgram(Album) rc=%08X", rc);
        LOG_I(msg);
    }
    if (R_FAILED(rc)) {
        uninstall_album_launcher_override();
        remove(LAUNCH_REQUEST_PATH);
        return rc;
    }

    g_album_override_active = true;
    g_album_override_installed_tick = svcGetSystemTick();
    return 0;
}

// Launches a program by title ID via pmshell. NcmStorageId_None (used here previously,
// in both callers below) makes pm try to resolve which storage the content lives on
// itself - on this firmware it doesn't, and pmshellLaunchProgram() always failed
// (rc module=Loader desc=5), confirmed independent of the JSON-body fix in POST
// /command above by reproducing the same failure through the pre-existing GET /launch
// endpoint too. Same fix as the /titles ncm rework: name the actual storage instead of
// leaving it to be inferred. Most sideloaded titles live on the SD card, so try that
// first and fall back to NAND user storage (matches the two storages /titles scans).
static Result launch_program_by_id(u64 tid) {
    NcmStorageId storages_to_try[2] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };

    // Diagnostic: pmshellLaunchProgram() (via ldr) still failed with
    // ProgramLocationEntryNotFound after naming the storage explicitly, which points at
    // lr (Location Resolver) - a separate database from the ncm ContentMetaDatabase
    // /titles reads - simply having no redirect entry for this program id. Query lr
    // directly first so GET /logs shows exactly which storage(s), if any, already have
    // one, instead of guessing further blind.
    Result rc_lr_init = lrInitialize();
    if (R_SUCCEEDED(rc_lr_init)) {
        for (int i = 0; i < 2; i++) {
            LrLocationResolver resolver;
            Result rc_open = lrOpenLocationResolver(storages_to_try[i], &resolver);
            char msg[192];
            if (R_SUCCEEDED(rc_open)) {
                char path_buf[FS_MAX_PATH] = {0};
                Result rc_resolve = lrLrResolveProgramPath(&resolver, tid, path_buf);
                snprintf(msg, sizeof(msg), "lr: storage=%d resolve rc=%08X path=%s",
                         storages_to_try[i], rc_resolve, path_buf[0] ? path_buf : "(none)");
                serviceClose(&resolver.s);
            } else {
                snprintf(msg, sizeof(msg), "lr: storage=%d open rc=%08X", storages_to_try[i], rc_open);
            }
            LOG_I(msg);
        }
        lrExit();
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "lr: init failed rc=%08X", rc_lr_init);
        LOG_E(msg);
    }

    Result rc = 0;
    // Tracks whether ANY attempt hit NcaBaseStorageOutOfRangeC specifically, not just
    // the last one - with two storages tried in sequence, a real NcaBaseStorageOutOfRangeC
    // on the SD attempt (the common case: title present, patch layering unresolved) gets
    // masked by the NAND attempt failing right after with a different, less interesting
    // error (ResultProgramNotFound, since the title usually isn't on NAND at all) if only
    // the final loop-exit rc were checked.
    bool hit_nca_error = false;
    for (int i = 0; i < 2; i++) {
        NcmProgramLocation loc = {tid, storages_to_try[i], {0}};
        rc = pmshellLaunchProgram(0, &loc, NULL);
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "pmshellLaunchProgram: storage=%d rc=%08X (module=%d desc=%d)",
                     storages_to_try[i], rc, R_MODULE(rc), R_DESCRIPTION(rc));
            LOG_I(msg);
        }
        if (R_SUCCEEDED(rc)) return rc;
        if (R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1004) hit_nca_error = true;
    }

    // Known limitation (see docs/api.md): titles with an installed update/patch fail
    // here with fs NcaBaseStorageOutOfRangeC (module=2 desc=1004) because this direct
    // pmshell launch doesn't resolve base+patch NCA layering the way am's own launch
    // path does. Fall back to the Album-override launcher (a real Application context)
    // only for that specific failure - titles failing for other reasons (eg. truly not
    // present on either storage) shouldn't pay the cost/risk of taking over Album.
    // Note: on this path, success here only means the fallback launch was kicked off,
    // not that the game has actually started - callers should poll GET /launch_status
    // for the real outcome (see request_launch_via_album's own note on why).
    if (hit_nca_error) {
        LOG_I("launch_program_by_id: NcaBaseStorageOutOfRangeC, falling back to Album-override launcher");
        rc = request_launch_via_album(tid);
    }
    return rc;
}

void handle_client(int client_sock) {
    trace("handle_client: entered");
    poll_album_launcher_completion();
    char buffer[2048] = {0};
    int bytes_read = recv_timeout(client_sock, buffer, sizeof(buffer) - 1, 2);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "handle_client: recv_timeout returned %d", bytes_read);
        trace(msg);
    }
    if (bytes_read <= 0) {
        close(client_sock);
        trace("handle_client: early return (no data)");
        return;
    }

    // curl (and most HTTP clients) routinely write the request headers and the POST
    // body as two separate send() calls, which can land as two separate TCP segments.
    // The single recv() above can return with just the headers and zero body bytes
    // even though the client already sent both - leaving POST handlers below (eg.
    // POST /command's launch_app) looking at an empty body and misreporting "Missing
    // action" even though one was sent. Keep pulling more data in until we've seen the
    // full Content-Length worth of body, or run out of buffer/time.
    {
        char* body_start = strstr(buffer, "\r\n\r\n");
        if (body_start) {
            char* cl_hdr = strstr(buffer, "Content-Length:");
            if (!cl_hdr) cl_hdr = strstr(buffer, "content-length:");
            if (cl_hdr) {
                long content_length = strtol(cl_hdr + 15, NULL, 10);
                long have_body = bytes_read - ((body_start + 4) - buffer);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "body-wait: initial bytes_read=%d have_body=%ld content_length=%ld",
                             bytes_read, have_body, content_length);
                    LOG_I(msg);
                }
                while (have_body < content_length && bytes_read < (int)(sizeof(buffer) - 1)) {
                    int more = recv_timeout(client_sock, buffer + bytes_read, sizeof(buffer) - 1 - bytes_read, 2);
                    {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "body-wait: retry recv_timeout returned %d", more);
                        LOG_I(msg);
                    }
                    if (more <= 0) break;
                    bytes_read += more;
                    have_body += more;
                }
            }
        }
    }

    bool requires_auth = true;
    if (strncmp(buffer, "GET /info", 9) == 0 || strncmp(buffer, "GET /health", 11) == 0) {
        requires_auth = false;
    }

    if (requires_auth) {
        const char* api_token = ConfigManager::getInstance().getApiToken();
        char token_header[128];
        snprintf(token_header, sizeof(token_header), "X-API-Token: %s", api_token);
        
        if (!strstr(buffer, token_header)) {
            ConfigManager::getInstance().load();
            api_token = ConfigManager::getInstance().getApiToken();
            snprintf(token_header, sizeof(token_header), "X-API-Token: %s", api_token);
            if (!strstr(buffer, token_header)) {
                const char *resp = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n{\"error\": \"Unauthorized\"}";
                send_timeout(client_sock, resp, strlen(resp));
                close(client_sock);
                return;
            }
        }
    }

    if (strstr(buffer, "GET /info")) {
        trace("info: start");
        char response[2048];
        // SetSysFirmwareVersion is a ~0x100 byte struct (major/minor/micro plus
        // platform/version_hash/display_version/display_title strings) - the previous
        // code cast a plain u32 to SetSysFirmwareVersion* and passed that to
        // setsysGetFirmwareVersion(), which wrote the entire struct into 4 bytes of
        // stack, corrupting ~250 bytes past it. It happened to not crash visibly since
        // nothing on this stack frame is used before /info sends its response and
        // returns, but corrupting adjacent stack memory on every single request is not
        // something to leave in place. It also explains the wrong firmware_version
        // ("0.5.22" instead of "22.5.0"): raw struct bytes major,minor,micro,padding1
        // landed in the u32's 4 bytes, and the old (>>16)/(>>8)/(&0xFF) extraction -
        // written for a real MAKEHOSVERSION-packed u32 - read them back in the wrong
        // order.
        SetSysFirmwareVersion fw_ver = {};
        setsysGetFirmwareVersion(&fw_ver);
        trace("info: after setsys");

        u32 battery_percent = 0;
        psmGetBatteryChargePercentage(&battery_percent);
        u32 charger_type = 0;
        psmGetChargerType((PsmChargerType*)&charger_type);
        trace("info: after psm");

        s32 cpu_temp = 0, gpu_temp = 0, skin_temp = 0;
        tsGetTemperature(TsLocation_Internal, &cpu_temp);
        gpu_temp = cpu_temp;
        skin_temp = cpu_temp;
        trace("info: after ts");

        u64 uptime_s = svcGetSystemTick() / 19200000;
        u32 rssi = 0;
        nifmGetInternetConnectionStatus(NULL, &rssi, NULL);
        trace("info: after nifm");

        u64 total_mem = 0, used_mem = 0;
        svcGetInfo(&total_mem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used_mem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        trace("info: after svcGetInfo");

        u64 sd_total = 0;
        s64 sd_free = 0;
        FsDeviceOperator fs_op;
        if (R_SUCCEEDED(fsOpenDeviceOperator(&fs_op))) {
            trace("info: fsOpenDeviceOperator ok");
            s64 temp_total = 0;
            if (R_SUCCEEDED(fsDeviceOperatorGetSdCardUserAreaSize(&fs_op, &temp_total))) {
                sd_total = (u64)temp_total;
            }
            fsDeviceOperatorClose(&fs_op);
        } else {
            trace("info: fsOpenDeviceOperator FAILED");
        }
        trace("info: after fs_op block");

        FsFileSystem sd_fs;
        if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd_fs))) {
            trace("info: fsOpenSdCardFileSystem ok");
            fsFsGetFreeSpace(&sd_fs, "/", &sd_free);
            fsFsClose(&sd_fs);
        } else {
            trace("info: fsOpenSdCardFileSystem FAILED");
        }
        trace("info: after sd_fs block");

        u64 title_id = 0;
        char title_name[512] = "None";
        u64 app_pid = 0;
        // pdmqryQueryRecentlyPlayedApplication() (used here previously) reports the last
        // application recorded in the play-log database, not what's actually running -
        // same class of bug as the ns ApplicationRecord/ncm mismatch fixed for /titles.
        // pmdmnt is the same mechanism the home menu/other homebrew use to know what's
        // running right now: it fails outright (no PID) when at the home menu, so a
        // failure here correctly means "nothing running", not "let's report stale data".
        if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&app_pid))) {
            trace("info: pmdmnt - application running");
            if (R_SUCCEEDED(pmdmntGetProgramId(&title_id, app_pid))) {
                NsApplicationControlData* controlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
                if (controlData) {
                    u64 actual_size = 0;
                    trace("info: before nsGetApplicationControlData");
                    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, title_id, controlData, sizeof(NsApplicationControlData), &actual_size))) {
                        trace("info: after nsGetApplicationControlData ok");
                        NacpLanguageEntry* langentry = NULL;
                        if (R_SUCCEEDED(nacpGetLanguageEntry(&controlData->nacp, &langentry))) {
                            strncpy(title_name, langentry->name, sizeof(title_name) - 1);
                            title_name[sizeof(title_name) - 1] = '\0';
                        }
                    } else {
                        trace("info: nsGetApplicationControlData FAILED");
                    }
                    free(controlData);
                }
            }
        } else {
            trace("info: pmdmnt - no application running (home menu)");
        }
        trace("info: after title block");

        // apm is a standalone, low-level service - unlike appletGetOperationMode() it
        // doesn't need an applet-type client session with am/omm to answer this (see the
        // note on the fix below, near appletInitialize in main()).
        ApmPerformanceMode perf_mode = ApmPerformanceMode_Normal;
        trace("info: before apmGetPerformanceMode");
        Result apm_rc = apmGetPerformanceMode(&perf_mode);
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "info: after apmGetPerformanceMode rc=%08X", apm_rc);
            trace(msg);
        }
        bool is_docked = (perf_mode == ApmPerformanceMode_Boost);
        bool is_sleeping = false;

        trace("info: before snprintf");
        snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n"
            "{\"firmware_version\": \"%u.%u.%u\", \"app_version\": \"" APP_VERSION "\", \"battery_level\": %u, \"charging\": %s, "
            "\"cpu_temp\": %d, \"gpu_temp\": %d, \"skin_temp\": %d, "
            "\"uptime\": %lu, \"wifi_rssi\": %u, \"mem_total\": %lu, \"mem_used\": %lu, "
            "\"sd_total\": %lu, \"sd_free\": %lu, \"current_title_id\": \"0x%016lX\", "
            "\"current_game\": \"%s\", \"docked\": %s, \"sleep_mode\": %s, \"error_count\": %u}",
            (unsigned int)fw_ver.major, (unsigned int)fw_ver.minor, (unsigned int)fw_ver.micro,
            (unsigned int)battery_percent, (charger_type != 0) ? "true" : "false",
            // tsGetTemperature() (used above) already returns whole-degree Celsius, not
            // milliC — the milliC variant (tsGetTemperatureMilliC) is only valid up to
            // firmware 13.2.1, unusable on current firmware. Dividing by 1000 here
            // silently truncated every real reading (e.g. 42C) down to 0.
            (int)cpu_temp, (int)gpu_temp, (int)skin_temp,
            (unsigned long)uptime_s, (unsigned int)rssi, (unsigned long)total_mem, (unsigned long)used_mem,
            (unsigned long)sd_total, (unsigned long)sd_free, (unsigned long)title_id,
            title_name, (is_docked) ? "true" : "false", (is_sleeping) ? "true" : "false",
            (unsigned int)Logger::getInstance().getErrorCount()
        );
        trace("info: after snprintf, before send_timeout");
        {
            ssize_t sent = send_timeout(client_sock, response, strlen(response));
            char msg[64];
            snprintf(msg, sizeof(msg), "handle_client: /info send_timeout returned %ld", (long)sent);
            trace(msg);
        }
        close(client_sock);
        trace("handle_client: /info done, closed");
        return;
    }

    if (strstr(buffer, "GET /titles")) {
        // NOTE: nsListApplicationRecord() (the ns "ApplicationRecord" database) was tried
        // here first, but it consistently returned 0 entries on this hardware even though
        // the home menu clearly shows installed games - it tracks eShop-style
        // install/update bookkeeping, which sideloaded/dumped NSPs on a CFW SD card don't
        // necessarily go through. The home menu itself, and title managers like
        // Tinfoil/DBI, actually enumerate installed titles via the ncm content-meta
        // database (which just reflects what NCA content physically exists in each
        // storage), so that's what we use here instead - across both SD card and NAND
        // user storage, since a title can live in either.
        std::string json_out = "[";
        bool first = true;
        u64 seen_ids[128];
        int seen_count = 0;

        NcmStorageId storages_to_scan[2] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
        for (int s = 0; s < 2; s++) {
            NcmContentMetaDatabase db;
            Result rc_open = ncmOpenContentMetaDatabase(&db, storages_to_scan[s]);
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "titles: ncm open storage=%d rc=%08X", storages_to_scan[s], rc_open);
                trace(msg);
            }
            if (R_FAILED(rc_open)) continue;

            s32 total = 0, written = 0;
            Result rc_count = ncmContentMetaDatabaseListApplication(&db, &total, &written, NULL, 0, NcmContentMetaType_Application);
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "titles: ncm list count storage=%d rc=%08X total=%d", storages_to_scan[s], rc_count, (int)total);
                trace(msg);
            }
            if (R_SUCCEEDED(rc_count) && total > 0) {
                NcmApplicationContentMetaKey* keys = (NcmApplicationContentMetaKey*)malloc(sizeof(NcmApplicationContentMetaKey) * total);
                if (keys) {
                    Result rc_list = ncmContentMetaDatabaseListApplication(&db, &total, &written, keys, total, NcmContentMetaType_Application);
                    if (R_SUCCEEDED(rc_list)) {
                        for (s32 i = 0; i < written; i++) {
                            u64 app_id = keys[i].application_id;

                            bool dup = false;
                            for (int j = 0; j < seen_count; j++) {
                                if (seen_ids[j] == app_id) { dup = true; break; }
                            }
                            if (dup) continue;
                            if (seen_count < (int)(sizeof(seen_ids) / sizeof(seen_ids[0]))) {
                                seen_ids[seen_count++] = app_id;
                            }

                            NsApplicationControlData* controlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
                            if (controlData) {
                                u64 actual_size = 0;
                                Result rc_ctrl = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, controlData, sizeof(NsApplicationControlData), &actual_size);
                                if (R_SUCCEEDED(rc_ctrl)) {
                                    char title_name[0x201] = {0};
                                    NacpLanguageEntry* langentry = NULL;
                                    if (R_SUCCEEDED(nacpGetLanguageEntry(&controlData->nacp, &langentry)) && langentry) {
                                        snprintf(title_name, sizeof(title_name), "%s", langentry->name);
                                    }
                                    if (!first) json_out += ",";
                                    char entry[1024];
                                    snprintf(entry, sizeof(entry), "{\"title_id\": \"0x%016lX\", \"name\": \"%s\"}", (unsigned long)app_id, title_name);
                                    json_out += entry;
                                    first = false;
                                }
                                free(controlData);
                            }
                        }
                    }
                    free(keys);
                }
            }
            ncmContentMetaDatabaseClose(&db);
        }
        json_out += "]";
        char resp_head[256];
        snprintf(resp_head, sizeof(resp_head), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", json_out.length());
        send_timeout(client_sock, resp_head, strlen(resp_head));
        send_timeout(client_sock, json_out.c_str(), json_out.length());
        close(client_sock);
        return;
    }

    if (strstr(buffer, "GET /screenshot")) {
        extern bool g_capssc_ready;
        if (!g_capssc_ready) {
            const char *resp = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n{\"error\": \"Screenshots disabled (capssc failed)\"}";
            send_timeout(client_sock, resp, strlen(resp));
            close(client_sock);
            return;
        }
        u8* screen_buf = (u8*)malloc(1280 * 720 * 4);
        if (screen_buf) {
            if (R_SUCCEEDED(capsscCaptureRawImageWithTimeout(screen_buf, 1280 * 720 * 4, (ViLayerStack)0, 1280, 720, 1, 0, 1000000000LL))) {
                #pragma pack(push, 1)
                struct BMPHeader {
                    uint16_t bfType;
                    uint32_t bfSize;
                    uint16_t bfReserved1;
                    uint16_t bfReserved2;
                    uint32_t bfOffBits;
                } bmp;
                struct BMPInfoHeader {
                    uint32_t biSize;
                    int32_t  biWidth;
                    int32_t  biHeight;
                    uint16_t biPlanes;
                    uint16_t biBitCount;
                    uint32_t biCompression;
                    uint32_t biSizeImage;
                    int32_t  biXPelsPerMeter;
                    int32_t  biYPelsPerMeter;
                    uint32_t biClrUsed;
                    uint32_t biClrImportant;
                } info;
                #pragma pack(pop)

                memset(&bmp, 0, sizeof(bmp));
                bmp.bfType = 0x4D42;
                bmp.bfSize = sizeof(bmp) + sizeof(info) + (1280 * 720 * 3);
                bmp.bfOffBits = sizeof(bmp) + sizeof(info);
                
                memset(&info, 0, sizeof(info));
                info.biSize = sizeof(info);
                info.biWidth = 1280;
                info.biHeight = -720; // Top-down
                info.biPlanes = 1;
                info.biBitCount = 24;
                info.biCompression = 0;
                info.biSizeImage = 1280 * 720 * 3;

                const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nConnection: close\r\n\r\n";
                send_timeout(client_sock, hdr, strlen(hdr));
                send_timeout(client_sock, &bmp, sizeof(bmp));
                send_timeout(client_sock, &info, sizeof(info));

                u8* row_buf = (u8*)malloc(1280 * 3);
                if (row_buf) {
                    for (int y = 0; y < 720; y++) {
                        u8* src = &screen_buf[y * 1280 * 4];
                        for (int x = 0; x < 1280; x++) {
                            row_buf[x * 3 + 0] = src[x * 4 + 2]; // B
                            row_buf[x * 3 + 1] = src[x * 4 + 1]; // G
                            row_buf[x * 3 + 2] = src[x * 4 + 0]; // R
                        }
                        send_timeout(client_sock, row_buf, 1280 * 3);
                    }
                    free(row_buf);
                }
            } else {
                const char *resp = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n{\"error\": \"Capture failed\"}";
                send_timeout(client_sock, resp, strlen(resp));
            }
            free(screen_buf);
        } else {
            const char *resp = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n{\"error\": \"OOM\"}";
            send_timeout(client_sock, resp, strlen(resp));
        }
        close(client_sock);
        return;
    }

    if (strstr(buffer, "POST /command")) {
        const char* body = strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            json j = json::parse(body, nullptr, false);
            if (!j.is_discarded() && j.contains("action")) {
                std::string action = j.value("action", "");
                Result rc = 0;
                if (action == "reboot") {
                    // See the note near POST /reboot below for why this is spsm, not bpc.
                    spsmInitialize();
                    spsmShutdown(true);
                    spsmExit();
                } else if (action == "shutdown") {
                    spsmInitialize();
                    spsmShutdown(false);
                    spsmExit();
                } else if (action == "launch_app" && j.contains("title_id")) {
                    std::string tid_str = j.value("title_id", "");
                    u64 tid = strtoull(tid_str.c_str(), NULL, 16);
                    // appletRequestLaunchApplication() (used here previously) asks am to
                    // launch on behalf of the CALLING application applet session - same
                    // class of bug as appletRequestToSleep() (see the note on POST
                    // /sleep): this sysmodule has no such session (AppletType_None), so
                    // it always failed with LibnxError_NotInitialized. pmshell is the
                    // lower-level service qlaunch/hbmenu actually use to start a program
                    // (already used identically by POST /launch below) and doesn't need
                    // one.
                    pmshellInitialize();
                    rc = launch_program_by_id(tid);
                    pmshellExit();
                }
                
                std::string resp_body = "{\"status\": \"ok\", \"rc\": " + std::to_string(rc) + "}";
                char resp[256];
                snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", resp_body.length());
                send_timeout(client_sock, resp, strlen(resp));
                send_timeout(client_sock, resp_body.c_str(), resp_body.length());
            } else {
                const char* resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n{\"error\": \"Missing action\"}";
                send_timeout(client_sock, resp, strlen(resp));
            }
        }
        close(client_sock);
        return;
    }

    if (strstr(buffer, "GET /launch_status")) {
        // Reflects launch_status.json as-is - written by switch_app's launcher build
        // during an Album-override fallback (see request_launch_via_album above), or
        // absent/stale between launches. No in-memory state kept here: this is the only
        // reader, and the file is small enough to just re-read on every request.
        std::string json_out;
        FILE* sf = fopen(LAUNCH_STATUS_PATH, "r");
        if (sf) {
            char buf[256] = {0};
            fread(buf, 1, sizeof(buf) - 1, sf);
            fclose(sf);
            json st = json::parse(buf, nullptr, false);
            json_out = st.is_discarded() ? "{\"state\": \"unknown\"}" : st.dump();
        } else {
            json_out = "{\"state\": \"idle\"}";
        }
        char resp_head[128];
        snprintf(resp_head, sizeof(resp_head), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", json_out.length());
        send_timeout(client_sock, resp_head, strlen(resp_head));
        send_timeout(client_sock, json_out.c_str(), json_out.length());
        close(client_sock);
        return;
    }

    if (strstr(buffer, "GET /logs")) {
        u32 count = Logger::getInstance().getLogCount();
        std::string json_out = "[";
        for (u32 i = 0; i < count; i++) {
            if (i > 0) json_out += ",";
            const LogEntry* log = Logger::getInstance().getLog(i);
            char entry[1024];
            const char* level_str = (log->level == LOG_LEVEL_INFO) ? "INFO" : (log->level == LOG_LEVEL_WARN) ? "WARN" : "ERROR";
            snprintf(entry, sizeof(entry), "{\"level\": \"%s\", \"message\": \"%s\", \"timestamp\": \"%s\"}", 
                     level_str, log->message, log->timestamp);
            json_out += entry;
        }
        json_out += "]";
        char resp_head[256];
        snprintf(resp_head, sizeof(resp_head), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", json_out.length());
        send_timeout(client_sock, resp_head, strlen(resp_head));
        send_timeout(client_sock, json_out.c_str(), json_out.length());
        close(client_sock);
        return;
    }

    if (strstr(buffer, "POST /reboot")) {
        LOG_I("System reboot requested");
        const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
        send_timeout(client_sock, resp, strlen(resp));
        // bpcRebootSystem()/bpcShutdownSystem() (bpc = Board Power Controller) trigger
        // an immediate, low-level hardware reboot/power-off with no orchestrated
        // shutdown of running processes first. A reboot triggered this way from the HA
        // "Reboot" button produced a fresh omm crash report (same 0x2A5 User Break
        // signature as the original applet/am bug) and a visible boot-menu-appears-
        // twice/black-screen glitch on real hardware - omm (or another system process)
        // getting cut off mid-operation by the abrupt reboot signal, not anything
        // specific to our applet/apm usage this time. spsm (Sleep/Power State Manager)
        // is the higher-level service that Horizon's own System Settings "Restart" uses
        // - spsmShutdown() orchestrates a graceful shutdown of running processes before
        // the actual power event, avoiding that race.
        spsmInitialize();
        spsmShutdown(true);
        spsmExit();
    } else if (strstr(buffer, "POST /shutdown")) {
        LOG_I("System shutdown requested");
        const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
        send_timeout(client_sock, resp, strlen(resp));
        spsmInitialize();
        spsmShutdown(false);
        spsmExit();
    } else if (strstr(buffer, "POST /sleep")) {
        LOG_I("System sleep requested");
        const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
        send_timeout(client_sock, resp, strlen(resp));
        // appletRequestToSleep() was tried first, but it silently did nothing: it asks
        // omm to sleep on behalf of the CALLING applet session, and this sysmodule has
        // no foreground/application applet context (AppletType_None) for omm to act on.
        // spsm (the same lower-level service already used for reboot/shutdown below) has
        // its own "EnterSleep" command (id 1) that puts the whole system to sleep
        // directly, the same way the physical power button does, without needing an
        // active application session. libnx's spsm.h doesn't wrap this one, so it's
        // dispatched by hand via the session spsmGetServiceSession() exposes.
        spsmInitialize();
        Result rc_sleep = serviceDispatch(spsmGetServiceSession(), 1);
        spsmExit();
        if (R_FAILED(rc_sleep)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "spsm EnterSleep failed rc=%08X", rc_sleep);
            LOG_E(msg);
        }
    } else if (strstr(buffer, "POST /reload_config")) {
        ConfigManager::getInstance().load();
        LOG_I("Config reloaded");
        const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
        send_timeout(client_sock, resp, strlen(resp));
    } else if (strstr(buffer, "POST /launch")) {
        char* id_ptr = strstr(buffer, "title_id=0x");
        if (id_ptr) {
            u64 tid = strtoull(id_ptr + 9, NULL, 16);
            const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
            send_timeout(client_sock, resp, strlen(resp));
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "Launching title ID: 0x%016lX", (unsigned long)tid);
            LOG_I(log_msg);
            pmshellInitialize();
            if (R_FAILED(launch_program_by_id(tid))) {
                snprintf(log_msg, sizeof(log_msg), "Failed to launch title ID: 0x%016lX", (unsigned long)tid);
                LOG_E(log_msg);
            }
            pmshellExit();
        } else {
            const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            send_timeout(client_sock, resp, strlen(resp));
        }
    } else if (strstr(buffer, "POST /update_app")) {
        const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\", \"message\": \"App update triggered\"}";
        send_timeout(client_sock, resp, strlen(resp));
        // See the note near POST /reboot above for why this is spsm, not bpc.
        spsmInitialize();
        spsmShutdown(true);
        spsmExit();
    } else if (strstr(buffer, "POST /button")) {
        char* body = strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            if (body[0] == '{') {
                json req = json::parse(body, nullptr, false);
                if (!req.is_discarded() && req.contains("sequence") && req["sequence"].is_array()) {
                    for (auto& step : req["sequence"]) {
                        std::string btn_name = step.value("button", "");
                        int duration = step.value("duration_ms", 100);
                        u64 btn_bit = 0;
                        if (btn_name == "A") btn_bit = HidNpadButton_A;
                        else if (btn_name == "B") btn_bit = HidNpadButton_B;
                        else if (btn_name == "X") btn_bit = HidNpadButton_X;
                        else if (btn_name == "Y") btn_bit = HidNpadButton_Y;
                        else if (btn_name == "L") btn_bit = HidNpadButton_L;
                        else if (btn_name == "R") btn_bit = HidNpadButton_R;
                        else if (btn_name == "ZL") btn_bit = HidNpadButton_ZL;
                        else if (btn_name == "ZR") btn_bit = HidNpadButton_ZR;
                        else if (btn_name == "PLUS") btn_bit = HidNpadButton_Plus;
                        else if (btn_name == "MINUS") btn_bit = HidNpadButton_Minus;
                        else if (btn_name == "LEFT") btn_bit = HidNpadButton_Left;
                        else if (btn_name == "RIGHT") btn_bit = HidNpadButton_Right;
                        else if (btn_name == "UP") btn_bit = HidNpadButton_Up;
                        else if (btn_name == "DOWN") btn_bit = HidNpadButton_Down;
                        else if (btn_name == "HOME") btn_bit = HiddbgNpadButton_Home;
                        else if (btn_name == "CAPTURE") btn_bit = HiddbgNpadButton_Capture;
                        
                        if (btn_bit != 0) {
                            HiddbgAbstractedPadState state;
                            memset(&state, 0, sizeof(state));
                            state.state.buttons = btn_bit;
                            hiddbgSetAutoPilotVirtualPadState(0, &state);
                            svcSleepThread(duration * 1000000ULL);
                            state.state.buttons = 0;
                            hiddbgSetAutoPilotVirtualPadState(0, &state);
                            svcSleepThread(50000000ULL); // 50ms pause
                        }
                    }
                    const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
                    send_timeout(client_sock, resp, strlen(resp));
                    close(client_sock);
                    return;
                }
            }
        }

        char* name_ptr = strstr(buffer, "name=");
        if (name_ptr) {
            char btn_name[32] = {0};
            sscanf(name_ptr + 5, "%31s", btn_name);
            // Remove potential & or space if it's there
            char* end = strpbrk(btn_name, " \r\n&");
            if (end) *end = '\0';

            u64 btn_bit = 0;
            if (strcmp(btn_name, "A") == 0) btn_bit = HidNpadButton_A;
            else if (strcmp(btn_name, "B") == 0) btn_bit = HidNpadButton_B;
            else if (strcmp(btn_name, "X") == 0) btn_bit = HidNpadButton_X;
            else if (strcmp(btn_name, "Y") == 0) btn_bit = HidNpadButton_Y;
            else if (strcmp(btn_name, "L") == 0) btn_bit = HidNpadButton_L;
            else if (strcmp(btn_name, "R") == 0) btn_bit = HidNpadButton_R;
            else if (strcmp(btn_name, "ZL") == 0) btn_bit = HidNpadButton_ZL;
            else if (strcmp(btn_name, "ZR") == 0) btn_bit = HidNpadButton_ZR;
            else if (strcmp(btn_name, "PLUS") == 0) btn_bit = HidNpadButton_Plus;
            else if (strcmp(btn_name, "MINUS") == 0) btn_bit = HidNpadButton_Minus;
            else if (strcmp(btn_name, "LEFT") == 0) btn_bit = HidNpadButton_Left;
            else if (strcmp(btn_name, "RIGHT") == 0) btn_bit = HidNpadButton_Right;
            else if (strcmp(btn_name, "UP") == 0) btn_bit = HidNpadButton_Up;
            else if (strcmp(btn_name, "DOWN") == 0) btn_bit = HidNpadButton_Down;
            else if (strcmp(btn_name, "HOME") == 0) btn_bit = HiddbgNpadButton_Home;
            else if (strcmp(btn_name, "CAPTURE") == 0) btn_bit = HiddbgNpadButton_Capture;

            if (btn_bit != 0) {
                HiddbgAbstractedPadState state;
                memset(&state, 0, sizeof(state));
                state.state.buttons = btn_bit;
                hiddbgSetAutoPilotVirtualPadState(0, &state);
                svcSleepThread(100000000ULL); // 100ms hold
                state.state.buttons = 0;
                hiddbgSetAutoPilotVirtualPadState(0, &state);
                
                const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
                send_timeout(client_sock, resp, strlen(resp));
            } else {
                const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n{\"error\": \"Invalid button\"}";
                send_timeout(client_sock, resp, strlen(resp));
            }
        } else {
            const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n{\"error\": \"Button name missing\"}";
            send_timeout(client_sock, resp, strlen(resp));
        }
    } else {
        const char *resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send_timeout(client_sock, resp, strlen(resp));
    }
    close(client_sock);
}

bool g_capssc_ready = false;

// std::thread aborts the whole process in this environment (libnx + -fno-exceptions:
// a failed thread creation hits a throw, which becomes std::terminate/abort with no
// crash report). Use libnx's native threadCreate/threadStart instead, which is the
// supported mechanism for threads in sysmodules.
Thread g_mdnsThread;
void mdns_responder_entry(void*) {
    mdns_responder();
}

// REVERTED (see the note near the accept() loop in main()): a small pool of native
// worker threads (threadCreate/threadStart, same mechanism as mdns_responder above)
// was tried here to let a few connections be served concurrently. On real hardware it
// made things categorically worse - the very first request after boot succeeded, but
// every request after that reset instantly (curl "Recv failure: Connection reset by
// peer", ~2-5ms, no crash report generated for our program ID, process stayed up).
// Root cause not identified (no debugger available on-device); suspect either a
// resource limit hit on the 2nd/3rd concurrent threadCreate() (silently falling into
// the close(client_sock)-with-unread-request-data path below, which sends RST instead
// of FIN) or a service/session that isn't safe to call from multiple native threads in
// this constrained sysmodule environment. Don't retry this approach without proper
// on-device debug tooling (this file's diagnostics are limited to log files + physical
// reboots - see "Metodologia de diagnóstico" in the project doc).

void log_boot_status(const char* service, Result rc) {
    FILE *fb = fopen("sdmc:/config/HomeAssistantSwitch/ha_sysmodule_boot.log", "a");
    if (fb) {
        if (R_SUCCEEDED(rc)) {
            fprintf(fb, "[OK] %s initialized\n", service);
        } else {
            fprintf(fb, "[ERROR] %s failed: %08X\n", service, rc);
        }
        fflush(fb);
        fclose(fb);
    }
}

// No-op unless ConfigManager's "debug" flag is set (settings.json, or POST
// /reload_config after editing it live - no reboot needed). Verbose per-request
// tracing like this is what pinned down the HTTP hang (see the note above
// recv_timeout/send_timeout): kept permanently instead of ad-hoc instrumentation added
// and stripped out on every investigation, but gated so it doesn't cost a fopen/fprintf
// per request (and unbounded log growth) during normal operation.
void trace(const char* msg) {
    if (!ConfigManager::getInstance().getDebug()) return;
    FILE *f = fopen("sdmc:/config/HomeAssistantSwitch/ha_sysmodule_boot.log", "a");
    if (f) { fprintf(f, "[TRACE] %s\n", msg); fflush(f); fclose(f); }
}

// Builds a fresh listening socket bound to the configured port. Split out of main() so
// the accept() loop can call it again to replace a socket that stopped working after a
// real system sleep/wake cycle (see the note where this is called from the loop).
static int create_listen_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_E("Failed to create socket");
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(ConfigManager::getInstance().getPort());
    address.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        FILE *f = fopen("sdmc:/config/HomeAssistantSwitch/ha_sysmodule_boot.log", "a");
        if (f) { fprintf(f, "[ERR] bind failed: %d\n", errno); fclose(f); }
        LOG_E("Failed to bind socket");
        close(fd);
        return -1;
    }
    if (listen(fd, 5) < 0) {
        FILE *f = fopen("sdmc:/config/HomeAssistantSwitch/ha_sysmodule_boot.log", "a");
        if (f) { fprintf(f, "[ERR] listen failed\n"); fclose(f); }
        LOG_E("Failed to listen on socket");
        close(fd);
        return -1;
    }

    FILE *f = fopen("sdmc:/config/HomeAssistantSwitch/ha_sysmodule_boot.log", "a");
    if (f) { fprintf(f, "[OK] Server listening on port %d\n", ConfigManager::getInstance().getPort()); fclose(f); }
    return fd;
}

int main(int, char **) {
    // Wait for the OS and network to fully initialize before opening sockets (15s)
    svcSleepThread(15000000000ULL);

    Result rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        fsdevMountSdmc();
    }

    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/HomeAssistantSwitch", 0777);

    // Loaded this early specifically so the "debug" flag (see trace() above) is known
    // before any of the boot-sequence trace() calls below run. The two later
    // ConfigManager::getInstance().load() calls further down are unrelated/pre-existing
    // and left as-is (harmless to load more than once).
    ConfigManager::getInstance().load();

    rc = setsysInitialize(); log_boot_status("setsys", rc);
    rc = psmInitialize(); log_boot_status("psm", rc);
    rc = tsInitialize(); log_boot_status("ts", rc);
    rc = nifmInitialize(NifmServiceType_User); log_boot_status("nifm", rc);
    rc = pmdmntInitialize(); log_boot_status("pmdmnt", rc);
    rc = nsInitialize(); log_boot_status("ns", rc);
    rc = ncmInitialize(); log_boot_status("ncm", rc);
    // NOT appletInitialize() here: it opens a client session with am/omm (the Operation
    // Mode Manager, a Horizon OS system process) that's meant for a real, actively
    // participating application - not a background sysmodule with __nx_applet_type ==
    // AppletType_None. On real hardware, holding that session open long-term crashed
    // omm roughly every 10-15 minutes (Atmosphere crash report: omm, Result 0x2A5, User
    // Break). Draining it via appletMainLoop() made it *worse* (crashes every ~3
    // minutes instead), so the actual fix is to avoid the applet/am session entirely
    // for anything long-lived. apm is a separate, low-level service that answers
    // dock-state queries (see ApmPerformanceMode usage in the /info handler below)
    // without any of this. appletInitialize()/appletExit() are still used, but only
    // wrapped tightly around the two rare user-triggered actions that genuinely need
    // them (POST /control launch_app, POST /sleep) - open right before the call, close
    // right after, so the process holds that session for milliseconds, not its entire
    // lifetime.
    rc = apmInitialize(); log_boot_status("apm", rc);
    rc = hiddbgInitialize(); log_boot_status("hiddbg", rc);

    rc = capsscInitialize(); log_boot_status("capssc", rc);
    if (R_SUCCEEDED(rc)) g_capssc_ready = true;

    // The bsd socket service is initialized on the main thread before any other
    // thread touches sockets, so nothing else can race ahead of it.
    //
    // socketGetDefaultInitConfig() sizes its TCP/UDP buffers for a full game
    // (~2.2MB of transfer memory, via sb_efficiency=4). __libnx_initheap() above
    // only reserves a 2MB heap for this whole sysmodule, and tmemCreate() backs
    // that transfer memory with a plain heap allocation — so socketInitialize()
    // always failed here with LibnxError_OutOfMemory (0x559), confirmed via the
    // logged Result code. Use a config sized for this sysmodule's actual needs
    // (one small HTTP request/response at a time, tiny mDNS packets) instead.
    // Diagnostic instrumentation (trace() calls throughout handle_client()/send_timeout())
    // pinned this down precisely: on the ~3rd HTTP request after boot, select() reports
    // the socket writable ("select returned 1") but the write() call right after it
    // blocks forever anyway. num_bsd_sessions (raised 3->8 in an earlier attempt) had
    // zero effect on this - it only bounds how many socket-like objects can be open at
    // once, not buffer/flow-control space. The real suspect is that tcp_tx_buf_size et
    // al. below were sized at the bare minimum (4KB/16KB) purely to fit inside this
    // sysmodule's 2MB heap after the original OOM fix (see the note above them) -
    // nowhere near enough headroom for the bsd service to reclaim a closed connection's
    // send buffer before the next one needs it. There's plenty of unused heap between
    // that minimal sizing and the 2MB ceiling to grow into.
    trace("before socketInitialize");
    SocketInitConfig sock_cfg = {
        .tcp_tx_buf_size = 0x2000,
        .tcp_rx_buf_size = 0x2000,
        .tcp_tx_buf_max_size = 0x8000,
        .tcp_rx_buf_max_size = 0x8000,
        .udp_tx_buf_size = 0x800,
        .udp_rx_buf_size = 0x800,
        .sb_efficiency = 1,
        .num_bsd_sessions = 4,
        .bsd_service_type = BsdServiceType_User,
    };

    rc = socketInitialize(&sock_cfg);
    if (R_FAILED(rc)) {
        FILE *f = fopen("sdmc:/config/HomeAssistantSwitch/ha_sysmodule_boot.log", "a");
        if (f) { fprintf(f, "[ERR] socketInitialize failed: 0x%08X\n", rc); fclose(f); }
        LOG_E("Failed to initialize sockets");
        return 1;
    }
    trace("after socketInitialize");

    trace("before mdns thread spawn");
    rc = threadCreate(&g_mdnsThread, mdns_responder_entry, nullptr, nullptr, 0x2000, 0x2C, -2);
    log_boot_status("mdns_thread_create", rc);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&g_mdnsThread);
        log_boot_status("mdns_thread_start", rc);
    }
    trace("after mdns thread spawn");

    g_logger.init();
    trace("after logger init");
    LOG_I("Home Assistant Sysmodule started (v" APP_VERSION ")");
    trace("after LOG_I");

    // Heartbeat log for physical Switch troubleshooting
    ConfigManager::getInstance().load();
    trace("after config load 1");

    ConfigManager::getInstance().load();
    trace("after config load 2");
    FILE *fb = fopen("sdmc:/config/HomeAssistantSwitch/ha_sys_boot.log", "a");
    trace("after fopen heartbeat");

    if (fb) {
        time_t t = time(NULL);
        fprintf(fb, "[%ld] Sysmodule Main Started (v%s)\n", t, APP_VERSION);
        fprintf(fb, "[%ld] Port: %d\n", t, ConfigManager::getInstance().getPort());
        fprintf(fb, "[%ld] API Token: %s\n", t, ConfigManager::getInstance().getApiToken()[0] ? "SET" : "MISSING");
        fclose(fb);
    }

    int server_fd, client_sock;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server_fd = create_listen_socket();
    if (server_fd < 0) {
        return 1;
    }

    // Power Optimization: Leave server_fd in blocking mode.
    // This allows the server thread to spend most of its time suspended, saving battery.
    // (No applet/am session is held open here anymore - see the note near apmInitialize
    // above - so there's no notification queue that needs periodic draining.)
    // TEMPORARY diagnostic instrumentation: every successful request after the very
    // first one post-boot either hangs or resets instantly, regardless of the
    // threading model, Connection: close, or num_bsd_sessions - none of which changed
    // the symptom at all. That points at the accept() loop itself getting stuck after
    // exactly one iteration. Logging every step to find out exactly where.
    int consecutive_accept_failures = 0;
    while (true) {
        trace("accept: waiting");
        client_sock = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_sock >= 0) {
            consecutive_accept_failures = 0;
            char msg[64];
            snprintf(msg, sizeof(msg), "accept: got client_sock=%d", client_sock);
            trace(msg);
            // A std::thread per connection hit the same abort as the mDNS thread above,
            // and a native-thread worker pool caused a worse regression on real
            // hardware (see the note above handle_client()). handle_client() already
            // has a 2s timeout (select()) on every recv()/write(), so handling requests
            // synchronously here is the safest known-working option.
            handle_client(client_sock);
            trace("handle_client: returned");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "accept: failed errno=%d", errno);
            trace(msg);
            // Seen on real hardware right after boot/Wi-Fi reassociation: accept() can
            // return spurious network errors (e.g. EHOSTUNREACH) instead of blocking,
            // which without a backoff turns this into a tight, non-blocking spin -
            // pegging the CPU and flooding the trace log at hundreds of lines/sec until
            // the network settles. Sleep briefly so a transient condition just waits it
            // out instead of busy-looping.
            svcSleepThread(200000000ULL); // 200ms
            consecutive_accept_failures++;

            // Also seen on real hardware after a real system sleep/wake cycle (unlike
            // the transient post-boot Wi-Fi reassociation case above, this one does NOT
            // recover on its own - confirmed by leaving it spinning on errno=113 for
            // several minutes straight): the listening socket itself doesn't survive
            // the network teardown/rebuild that happens around sleep, and every accept()
            // on it fails forever afterwards. There's no clean way for a background
            // sysmodule with no applet/am session to be notified of the sleep/wake
            // transition directly (that's exactly the session we avoid holding - see the
            // note near apmInitialize above), so instead: after ~5s of nothing but
            // failures, assume the socket is dead and just replace it.
            if (consecutive_accept_failures >= 25) {
                trace("accept: too many consecutive failures, recreating listen socket");
                close(server_fd);
                server_fd = create_listen_socket();
                if (server_fd < 0) {
                    // Can't recover without a listening socket at all; give the network
                    // more time to settle and try building a fresh one again.
                    svcSleepThread(1000000000ULL); // 1s
                    server_fd = create_listen_socket();
                }
                consecutive_accept_failures = 0;
            }
        }
    }

    // Restarting core (after a redeploy, or after it stops responding) is the
    // orchestrator's job now (switch_sysmodule_orchestrator/) - it terminates and
    // relaunches this program from outside, which also covers the case this process
    // can no longer handle itself: a broken deploy that can't even reach this code.
    if (g_capssc_ready) capsscExit();
    hiddbgExit();
    apmExit();
    ncmExit();
    nsExit();
    pmdmntExit();
    nifmExit();
    tsExit();
    setsysExit();
    psmExit();
    socketExit();
    return 0;
}
