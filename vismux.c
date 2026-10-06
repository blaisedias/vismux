/***************************************************************
 * FILENAME: vismux.c
 * DESCRIPTION: A low latency, real-time POSIX Shared Memory (SHM) replicator tailored specifically for Squeezelite audio visualizer data.
 * SUMMARY: Pushes visualizer data over a UDP network pipeline, allowing Jivelite and third-party tools like CAVA or projectM to run seamlessly on a completely remote machine on the same LAN.
 * AUTHOR: Paul Webster
 * DATE: 20/Sep/2026
 * MODIFICATION:
 * CHANGES: N/A
 * Copyright (C) 2026 Paul Webster
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Paul Webster - paul@dabdig.com
 ****************************************************************/
#include "vismux.h"

#if !defined(VISMUX_ALL) && !defined(VISMUX_DISC) && !defined(VISMUX_SRC) && !defined(VISMUX_DEST)
#define VISMUX_ALL
#endif
#if (defined(VISMUX_ALL) + defined(VISMUX_DISC) + defined(VISMUX_SRC) + defined(VISMUX_DEST)) != 1
#error "Define exactly one of VISMUX_ALL, VISMUX_DISC, VISMUX_SRC, or VISMUX_DEST"
#endif

char shm_path[128] = {0};
char global_mac[18] = {0};

#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
#define SLOT_COUNT  16
static destination_sink_t sinks[SLOT_COUNT];
#endif


#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
// For --destination
bool no_shm_output = false; // *** need to pass to run_source ***
bool wait_for_source = false;   // If running in destination mode but no sources configured or discovered then wait for one to appear
#endif /* VISMUX_DEST || VISMUX_ALL */
#if defined(VISMUX_SRC) || defined(VISMUX_ALL)
// For --source
int find_squeezelite_shm(void)
{
    DIR *shm_dir = opendir("/dev/shm");
    if (!shm_dir)
    {
        log_msg(0, "Failed to open /dev/shm: %s", strerror(errno));
        return -1;
    }

    char matches[MAX_DESTINATIONS][sizeof(shm_path)];
    int match_count = 0;
    char local_mac[18];
    struct dirent *entry;
    while ((entry = readdir(shm_dir)) != NULL)
    {
        if (strncmp(entry->d_name, SQUEEZELITE_SHM_PREFIX, SQUEEZELITE_SHM_PREFIX_LEN) != 0)
            continue;

        char formatted_path[sizeof(shm_path)];
        if (!validate_and_format_mac(entry->d_name + SQUEEZELITE_SHM_PREFIX_LEN, formatted_path, sizeof(formatted_path)) ||
            strcmp(entry->d_name, formatted_path + 1) != 0)
            continue;

        snprintf(local_mac, sizeof(local_mac), "%s", entry->d_name + SQUEEZELITE_SHM_PREFIX_LEN);
        if (match_count < MAX_DESTINATIONS)
            snprintf(matches[match_count], sizeof(matches[match_count]), "%s", formatted_path);
        match_count++;
    }
    closedir(shm_dir);

    if (match_count == 1)
    {
        snprintf(shm_path, sizeof(shm_path), "%s", matches[0]);
        snprintf(global_mac, sizeof(global_mac), "%s", local_mac);
        log_msg(2, "Automatically selected Squeezelite SHM: %s", shm_path);
        return 1;
    }
    if (match_count > 1)
    {
        log_msg(0, "Multiple Squeezelite SHM segments found; specify --mac to select one:");
        int listed_count = match_count < MAX_DESTINATIONS ? match_count : MAX_DESTINATIONS;
        for (int i = 0; i < listed_count; i++)
            log_msg(0, "  %s", matches[i]);
        if (match_count > MAX_DESTINATIONS)
            log_msg(0, "  ... and %d more", match_count - MAX_DESTINATIONS);
        return -1;
    }
    return 0;
}


// for --source
bool resolve_source_shm(void)
{
    for (;;)
    {
        int result = find_squeezelite_shm();
        if (result == 1)
            return true;
        if (result < 0 || !wait_for_shm)
        {
            if (result == 0)
                log_msg(0, "No Squeezelite SHM segment found in /dev/shm. Start Squeezelite with visualiser enabled (-v), or pass --wait-for-shm.");
            return false;
        }

        log_msg(1, "No Squeezelite SHM segment found; waiting for one to appear. Press q or Ctrl+C to stop.");
        while (keep_running)
        {
            sleep(2);
            result = find_squeezelite_shm();
            if (result != 0)
                break;
        }
        if (!keep_running)
        {
            log_msg(1, "Interrupted while waiting for Squeezelite SHM.");
            return false;
        }
    }
}
#endif /* VISMUX_SRC || VISMUX_ALL */


int main(int argc, char *argv[])
{
    bool is_source = false, is_dest = false, is_discover = false;
    #ifdef VISMUX_SRC
        is_source = true;
    #endif
    #ifdef VISMUX_DEST
        is_dest = true;
    #endif
    #ifdef VISMUX_DISC
        is_discover = true;
    #endif
   
    char *mac_input = NULL;
    char *peppymeter_fifo_path = NULL;
    bool disable_discovery_listener = false;
    (void)disable_discovery_listener;  /* usage might be removed by #ifdef so avoid compilation problem */
    int ix_dest = 0;
    (void)ix_dest;  /* usage might be removed by #ifdef so avoid compilation problem */
    bool daemonise = false;
    const char* logfile = NULL;
    uint8_t role_filter = 0;
    (void)role_filter;  /* usage might be removed by #ifdef so avoid compilation problem */
#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
//    bool discoverable = true;
    bool auto_add = false;
    int polling_wait_secs = 10;
    int destination_timeout_secs = 10;
#endif

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

#define ARG_AVAIL(n)  if ((i +n) >= argc) { fprintf(stderr, "invalid commandline"); exit(EXIT_FAILURE); }
    for (int i = 1; i < argc; i++)
    {
#ifdef VISMUX_ALL
        if (strcmp(argv[i], "--source") == 0) {
            is_source = true;
        } else if (strcmp(argv[i], "--destination") == 0) {
            is_dest = true;
        } else  if (strcmp(argv[i], "--discover") == 0) {
            is_discover = true;
        } else 
#endif
        if (strcmp(argv[i], "--no-discover") == 0) {
            disable_discovery_listener = true;
        } else 
#ifdef VISMUX_DISC
        if (strcmp(argv[i], "--discover-source") == 0 ) {
            role_filter = DISCOVER_ROLE_SOURCE;
            is_discover = true;
        } else if (strcmp(argv[i], "--discover-destination") == 0 ) {
            role_filter = DISCOVER_ROLE_DESTINATION;
            is_discover = true;
        } else 
#endif
        if (strcmp(argv[i], "--discover-timeout") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            discover_timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--discover-port") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            discover_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--discover-format") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            discover_format = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--wait-for-shm") == 0) {
            wait_for_shm = true;
        } else 
#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
            if (strcmp(argv[i], "--wait-for-source") == 0) {
            wait_for_source = true;
        } else 
#endif
#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
        if (strcmp(argv[i], "--server") == 0 ) {
            ARG_AVAIL(1);
            if (ix_dest < (int)(sizeof(sinks)/sizeof(sinks[0]))) {
                destination_sink_t* sink = sinks + ix_dest;
                destination_task_t* task = &sink->task;
                memset(task, 0, sizeof(*task));
                // duplicate the argument, we may need to tokkenise it.
                char* src_ip = strdup(argv[++i]);
                task->state.keep_running = true;
                // first look for MAC address segment separator, and split the string
                char *macp = strchr(src_ip, ',');
                if (macp) {
                    *macp = '\0';
                    ++macp;
                    if (strlen(macp) != (sizeof(task->spec.peer.mac) - 1)
                            || !validate_mac_spec(macp)) {
                        log_msg(-1, "Invalid MAC address %s", macp);
                        exit(EXIT_FAILURE);
                    }
                    strncpy(task->spec.peer.mac, macp, sizeof(task->spec.peer.mac)-1);
                }
                // then look for port segment separator, and split the string
                char *portp = strchr(src_ip, ':');
                if (portp) {
                    *portp = '\0';
                    ++portp;
                    task->spec.peer.port = atoi(portp);
                } else {
                    task->spec.peer.port = global_port;
                }
                // at this point src_ip should be IP address only, shorn of port and MAC segments
                if (inet_pton(AF_INET, src_ip, &task->spec.peer.ip) != 1) {
                    log_msg(-1, "invalid IP address %s", src_ip);
                    exit(EXIT_FAILURE);
                }
                strncpy(task->spec.server_ip, src_ip, sizeof(task->spec.server_ip)-1);
                sink->spec_setup = true;
                free(src_ip);
                ++ix_dest;
            } else {
                log_msg(-1, "too many sources, max sources = %d", (int)(sizeof(sinks)/sizeof(sinks[0])));
            }
        } else if (strcmp(argv[i], "--auto") == 0) {
            auto_add = true;
        } else if (strcmp(argv[i], "--polling-wait") == 0 ) {
            ARG_AVAIL(1);
            polling_wait_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--destination-timeout") == 0 ) {
            ARG_AVAIL(1);
            destination_timeout_secs = atoi(argv[++i]);
        } else 
#endif /* VISMUX_DEST || VISMUX_ALL */
        if (strcmp(argv[i], "--mac") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            mac_input = argv[++i];
        } else if (strcmp(argv[i], "--peppymeter-fifo") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            peppymeter_fifo_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            global_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            target_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mac-timeout") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            mac_timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            log_level = atoi(argv[++i]);
        } else 
#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
        if (strcmp(argv[i], "--proto-version") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            forced_proto_version = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--remove-shm") == 0) {
            keep_shm = false;
//        } else if (strcmp(argv[i], "--no-shm-output") == 0) {
//            no_shm_output = true;
        } else
#endif /* VISMUX_DEST || VISMUX_ALL*/
        if (strcmp(argv[i], "--stats-int") == 0 && i + 1 < argc) {
            ARG_AVAIL(1);
            stats_int = atoi(argv[++i]);
        } 
#if !defined NODAEMON && !defined VISMUX_DISC  // Only if daemonisation is not disabled (not available on macOS)
            else if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--daemonise") == 0) {
                daemonise = true;
        }
#endif  // !NODAEMON && !VISMUX_DISC
            else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--logfile") == 0) {
            ARG_AVAIL(1);
            logfile = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("vismux version %s\n", APP_VERSION);
            return 0; // Clean exit immediately
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Squeezelite Replicator v%s\nUsage Options:\n", APP_VERSION);
#ifdef VISMUX_ALL
            printf("  Source Mode:      %s --source [--mac <mac_address>] [--wait-for-shm] [--port <p>] [--fps <f>] [--timeout <sec>] [--no-discover] [--discover-port <p>]\n", argv[0]);
#elif defined VISMUX_SRC
            printf("  %s [--mac <mac_address>] [--wait-for-shm] [--port <p>] [--fps <f>] [--timeout <sec>] [--no-discover] [--discover-port <p>]\n", argv[0]);
#endif
#ifdef VISMUX_ALL
            printf("  Destination Mode: %s --destination [--server <source_ip>[:<source_port>][,<mac_address>]] [--wait-for-source] [--mac-timeout <sec>] [--port <p>] [--proto-version <1|2>] [--remove-shm] [--stats-int <interval_secs>]\n", argv[0]);
#elif defined VISMUX_DEST
            printf("  %s [--server <source_ip>[:<source_port>][,<mac_address>]] [--wait-for-source] [--mac-timeout <sec>] [--port <p>] [--proto-version <1|2>] [--remove-shm] [--stats-int <interval_secs>]\n", argv[0]);
#endif
#if defined VISMUX_DEST || defined VISMUX_ALL
            printf("    --server can be repeated multiple times, once for each source\n");
            printf("    --auto add newly discovered sources");
            printf("    --polling-wait delay between discovery calls (default=%d)", polling_wait_secs);
            printf("    --destination-timeout period of disconnection on destinastion, after which to stop the destination (default=%d)", destination_timeout_secs);
#endif
#ifdef VISMUX_ALL
            printf("  Discovery Mode:   %s --discover[-source|-destination] [--discover-format <fmt>] [--discover-timeout <sec>] [--discover-port <p>\n", argv[0]);
#elif defined VISMUX_DISC
            printf("%s --discover[-source|-destination] [--discover-format <fmt>] [--discover-timeout <sec>] [--discover-port <p>\n", argv[0]);
#endif
#if defined VISMUX_DISC || defined VISMUX_ALL
            printf("    --discover-source and  --discover-destination provide filtering of the discovery result\n");
            printf("    --discover-format <fmt> 0=simple csv, 1=titles in csv\n");
#endif
            printf("\nGlobal Flags:\n");
			printf("  -h, --help        Display this help message\n");
            printf("  -v, --version     Display application version details\n");
#if !defined NODAEMON && !defined VISMUX_DISC   // Only if daemonisation is not disabled (not available on macOS)
            printf("  -z, --daemonise   Detach from terminal to run in background (daemon)\n");
#endif  // !NODAEMON && !VISMUX_DISC
            printf("  --log-level <0-3> Filter verbosity (0=ERR, 1=WARN, 2=INFO, 3=DBG)\n");
//            printf("  --peppymeter-fifo <path> Destination output to a PeppyMeter FIFO\n");
//            printf("  --no-shm-output         Do not create, map, or update destination SHM\n");
#ifndef VISMUX_DISC
            printf("\nInteractive Controls (Enter not required):\n");
            printf("  Press 'v'         Version - Display application version details\n");
            printf("  Press 'q'         Quit - Request shutdown\n");
            printf("  Press 'l'         Log Level - Cycle log levels dynamically (0=ERROR -> 1=WARN -> 2=INFO -> 3=DEBUG)\n");
			printf("  Press 's'         Stats - Request stats summary on next data reception\n");
#endif
            return 0;
        } else {
            fprintf(stderr, "Unrecognized option: %s. %s\n", argv[i], HELP_HINT);
            return 1;
        }
    }

    int mode_count = (int)is_source + (int)is_dest + (int)is_discover;
    if (mode_count != 1)
    {
        fprintf(stderr, "Specify exactly one mode: --source, --destination, or --discover. %s\n", HELP_HINT);
        return 1;
    }

    if (peppymeter_fifo_path && peppymeter_fifo_path[0] == '\0')
    {
        fprintf(stderr, "--peppymeter-fifo requires a non-empty FIFO path. %s\n", HELP_HINT);
        return 1;
    }
    if (peppymeter_fifo_path && !is_dest)
    {
        fprintf(stderr, "--peppymeter-fifo is only available in destination mode. %s\n", HELP_HINT);
        return 1;
    }
//    if (no_shm_output && !is_dest)
//    {
//        fprintf(stderr, "--no-shm-output is only available in destination mode. %s\n", HELP_HINT);
//        return 1;
//    }

#if defined(VISMUX_DISC) || defined(VISMUX_ALL)
    if (is_discover)
    {
        discover_records_t* discovery  = run_discovery_prober(role_filter);
        if (discovery) {
            for (uint8_t role=1; role <3; ++role) {
                for (int ix =0; ix < discovery->count; ++ix) {
                    if ((discovery->records[ix].role != role)) {
                        continue;
                    }
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &discovery->records[ix].ip, ip_str, INET_ADDRSTRLEN);

                    if (discover_format == 0) {
                        printf("%s,%s,%u,%s,%s\n",
                            (discovery->records[ix].role == DISCOVER_ROLE_SOURCE) ? "SOURCE" : "DESTINATION",
                            ip_str,
                            discovery->records[ix].port,
                            discovery->records[ix].mac,
                            discovery->records[ix].version);
                    }
                    else {
                        printf("type:%s, IP address:%s, port:%u, MAC address:%s, version: %s\n",
                            (discovery->records[ix].role == DISCOVER_ROLE_SOURCE) ? "SOURCE" : "DESTINATION",
                            ip_str,
                            discovery->records[ix].port,
                            discovery->records[ix].mac,
                            discovery->records[ix].version);
                    }
                }
            }
            free(discovery);
        }
        return 0; // Turnkey exit immediately when the prober pass wraps up
    }
#endif /* VISMUX_DISC || VISMUX_ALL */

#if defined(VISMUX_SRC) || defined(VISMUX_ALL)
    if (is_source && mac_input && !validate_and_format_mac(mac_input, shm_path, sizeof(shm_path)))
    {
        fprintf(stderr, "Invalid MAC parameter. %s\n", HELP_HINT);
        return 1;
    }
#endif /* VISMUX_SRC || VISMUX_ALL */
#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
    if (is_dest && mac_input && !validate_and_format_mac(mac_input, shm_path, sizeof(shm_path)))
    {
        fprintf(stderr, "Invalid MAC parameter. %s\n", HELP_HINT);
        return 1;
    }
#endif /* VISMUX_DEST || VISMUX_ALL */

//    if (is_dest && ix_dest ==0)
//    {
//        fprintf(stderr, "Destination mode requires --server <source_ip>. %s\n", HELP_HINT);
//        return 1;
//    }
//
    if (stats_int < 1)
    {
        fprintf(stderr, "--stats-int must be numbers only and greater than or equal to 1. %s\n", HELP_HINT);
        return 1;
    }
    if (mac_timeout_secs < 1)
    {
        fprintf(stderr, "--mac-timeout must be numbers only and greater than or equal to 1. %s\n", HELP_HINT);
        return 1;
    }

#if defined(VISMUX_SRC) || defined(VISMUX_ALL)
    if (is_source && !mac_input && !resolve_source_shm())
    {
        return 1;
    }
#endif /* VISMUX_SRC || VISMUX_ALL */

    if (!mac_input) {
        mac_input = global_mac;
    }

    pthread_t* ui_thread=NULL;

    if (NULL != logfile) {
#ifndef NODAEMON    // Only if daemonisation is not disabled (not available on macOS)
        if (daemonise && !freopen(logfile, "a", stdout)) {
            fprintf(stderr, "Error opening log file %s: %s\n", logfile, strerror(errno));
            exit(EXIT_FAILURE);
        }
#else
        (void)daemonise;  // Set so that compiler does not error on variable not being used
#endif  // NODAEMON
        if (!freopen(logfile, "a", stderr)) {
            fprintf(stderr, "Error opening log file %s: %s\n", logfile, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

#ifndef NODAEMON    // Only if daemonisation is not disabled (not available on macOS)
    if (daemonise) {
        if (daemon(0, logfile ? 1: 0)) {
            fprintf(stderr, "Failed to run as daemon: %s\n", strerror(errno));
        }
    } else {
        if (isatty(STDIN_FILENO)) {
            ui_thread = create_thread(NULL, console_listener_thread, NULL);
        }
    }
#else
        if (isatty(STDIN_FILENO)) {
            ui_thread = create_thread(NULL, console_listener_thread, NULL);
        }
#endif// NODAEMON

#if defined(VISMUX_SRC) || defined(VISMUX_ALL)
    if (is_source)
    {
        run_source(shm_path, mac_input, !disable_discovery_listener);
    }
    else 
#endif /* VISMUX_SRC || VISMUX_ALL */
#if defined(VISMUX_DEST) || defined(VISMUX_ALL)
    if (is_dest)
    {
        if (ix_dest == 0) {
            log_msg(-1, "No sources specified, using discovery to add sources.");
            destination_sink_manager(sinks, (int)(sizeof(sinks)/sizeof(sinks[0])), polling_wait_secs, destination_timeout_secs, true);
          } else {
            destination_sink_manager(sinks, (int)(sizeof(sinks)/sizeof(sinks[0])), polling_wait_secs, destination_timeout_secs, auto_add);
          }
        keep_running = 0;
    }
#endif /* VISMUX_DEST || VISMUX_ALL */

    join_thread(&ui_thread);

//    release_system_resources();
    return 0;
}
