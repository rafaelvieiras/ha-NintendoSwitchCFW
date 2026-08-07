#include <switch.h>
#include <switch/services/set.h>
#include <switch/services/pm.h>
#include <switch/services/ncm.h>
#include <switch/services/fs.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include "../switch_sysmodule/include/ConfigManager.h"
#include "../switch_sysmodule/include/SysmoduleConstants.h"

// Same low-level startup overrides core uses (see the matching block in
// switch_sysmodule/main.cpp) - required for ANY boot2-launched sysmodule, not optional
// boilerplate. Without __nx_applet_type = AppletType_None here, libnx's default
// __appInit tries to set up an applet/am session that doesn't exist yet this early in
// boot, which is what actually caused the crash loop the first time this was deployed
// without this block.
extern "C" {
    u32 __check_mask_save;

    void __libnx_initheap(void) {
        // 1MB - generous headroom for the socket transfer memory (same buffer sizing
        // as core's SocketInitConfig below) even though this process does far less
        // than core otherwise (no curl/mbedtls/json).
        static char inner_heap[0x100000];
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

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// Minimal always-on supervisor for the real HomeAssistant sysmodule ("core", program id
// SYSMODULE_PROGRAM_ID). This process is what Atmosphere's boot2 launches now - it
// launches core itself right after boot, then just watches it and owns the one thing
// core must never be responsible for policing itself: restarting after a bad deploy.
// Deliberately does NOT proxy core's actual HTTP traffic (HA/curl talk to core's port
// directly, unchanged) - that would double the exact recv/send-timeout code paths this
// project has already found real bugs in, without adding any real resilience (a proxy
// can't recover from core's own logic hanging, only from core being fully gone).

#define CONTROL_PORT 8276
#define CORE_PROGRAM_ID 0x010000000000CAFEULL
#define HEALTH_CHECK_INTERVAL_SEC 15
#define HEALTH_CHECK_FAIL_THRESHOLD 3 // ~45s of no response before we act

static void log_line(const char* msg) {
    FILE *f = fopen("sdmc:/config/HomeAssistantSwitch/ha_orchestrator_boot.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static Result launch_core() {
    pmshellInitialize();
    // storage=None: same Atmosphere-content-override case as any other custom system
    // module (see the note on this exact distinction in core's launch_program_by_id) -
    // core lives under /atmosphere/contents/, not the ncm/lr install database.
    NcmProgramLocation loc = {CORE_PROGRAM_ID, NcmStorageId_None, {0}};
    Result rc = pmshellLaunchProgram(0, &loc, NULL);
    pmshellExit();
    return rc;
}

static void terminate_core() {
    pmshellInitialize();
    // Ignore the result - this is expected to fail harmlessly if core already exited
    // on its own (nothing to terminate).
    pmshellTerminateProgram(CORE_PROGRAM_ID);
    pmshellExit();
}

static void restart_core(const char* reason) {
    char msg[96];
    snprintf(msg, sizeof(msg), "[RESTART] %s", reason);
    log_line(msg);
    terminate_core();
    svcSleepThread(500000000ULL); // let the port free up before relaunching
    Result rc = launch_core();
    snprintf(msg, sizeof(msg), "[RESTART] launch_core rc=%08X", rc);
    log_line(msg);
}

// Plain, non-blocking TCP connect to core's own port on loopback - true even when core
// is alive but its request-handling logic is stuck, unlike just checking the process
// list (a hung process is still "running").
static bool core_is_alive(int core_port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(core_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    bool ok = false;
    int res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (res == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {2, 0};
        if (select(sock + 1, NULL, &fds, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            ok = (err == 0);
        }
    }
    close(sock);
    return ok;
}

static int create_control_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 5) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Deliberately tiny: this is the one piece of the whole project that must keep working
// even when core doesn't, so it stays as simple as possible on purpose - no JSON, no
// request bodies, nothing beyond "is the token right, and is this /self_restart".
static void handle_control_client(int sock) {
    char buffer[512] = {0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv = {2, 0};
    if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
        recv(sock, buffer, sizeof(buffer) - 1, 0);
    }

    char token_header[160];
    snprintf(token_header, sizeof(token_header), "X-API-Token: %s", ConfigManager::getInstance().getApiToken());
    bool authed = strstr(buffer, token_header) != NULL;
    bool is_restart = strstr(buffer, "POST /self_restart") != NULL;

    const char* resp;
    if (!authed) {
        resp = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n{\"error\": \"Unauthorized\"}";
    } else if (is_restart) {
        resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\": \"ok\"}";
    } else {
        resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
    }
    send(sock, resp, strlen(resp), 0);
    close(sock);

    if (authed && is_restart) {
        restart_core("self_restart requested");
    }
}

int main(int, char**) {
    // Same network/OS settle wait core used to do at boot - core no longer needs it
    // (it's only ever launched by us, well after this point), so this is now the only
    // place in the whole project paying this cost, and only once per real console boot.
    svcSleepThread(15000000000ULL);

    fsInitialize();
    fsdevMountSdmc();
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/HomeAssistantSwitch", 0777);
    ConfigManager::getInstance().load();

    log_line("[BOOT] Orchestrator started");

    // Same minimal buffer sizing core uses (see the note near SocketInitConfig in
    // core's main()) - this process only ever handles one tiny control request at a
    // time plus a loopback health-check connect, nothing close to game-sized traffic.
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
    if (R_FAILED(socketInitialize(&sock_cfg))) {
        log_line("[ERR] socketInitialize failed");
        return 1;
    }

    Result rc = launch_core();
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "[BOOT] launch_core rc=%08X", rc);
        log_line(msg);
    }

    int server_fd = create_control_socket();
    if (server_fd < 0) {
        log_line("[ERR] Failed to create control socket");
        return 1;
    }

    int consecutive_health_failures = 0;
    u64 last_health_check = svcGetSystemTick();
    const u64 ticks_per_sec = armGetSystemTickFreq();

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = {HEALTH_CHECK_INTERVAL_SEC, 0};
        int sel = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (sel > 0) {
            int client_sock = accept(server_fd, NULL, NULL);
            if (client_sock >= 0) {
                handle_control_client(client_sock);
            }
        }

        u64 now = svcGetSystemTick();
        if (now - last_health_check >= HEALTH_CHECK_INTERVAL_SEC * ticks_per_sec) {
            last_health_check = now;
            if (core_is_alive(ConfigManager::getInstance().getPort())) {
                consecutive_health_failures = 0;
            } else {
                consecutive_health_failures++;
                char msg[64];
                snprintf(msg, sizeof(msg), "[HEALTH] core unreachable (%d/%d)",
                         consecutive_health_failures, HEALTH_CHECK_FAIL_THRESHOLD);
                log_line(msg);
                if (consecutive_health_failures >= HEALTH_CHECK_FAIL_THRESHOLD) {
                    restart_core("health check failed");
                    consecutive_health_failures = 0;
                }
            }
        }
    }

    return 0;
}
