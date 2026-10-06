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

static destination_sink_t sinks[MAX_SEEN_PEERS];

int main(int argc, char *argv[])
{
    int dest_count = 0;
    bool daemonise = false;
    const char* logfile = NULL;
    pthread_t* ui_thread = NULL;
//    bool discoverable = true;
    bool auto_add = false;
    int polling_wait_secs = 2;
    int destination_timeout_secs = 10;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

#define ARG_AVAIL(n)  if ((i +n) >= argc) { fprintf(stderr, "invalid commandline"); exit(EXIT_FAILURE); }
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--source") == 0 ) {
            ARG_AVAIL(1);
            if (dest_count < (int)(sizeof(sinks)/sizeof(sinks[0]))) {
                destination_sink_t* sink = sinks + dest_count;
                destination_task_t* task = &sink->task;
                memset(task, 0, sizeof(*task));
                // duplicate the argument, we may need to tokkenise it.
                char* src_ip = strdup(argv[++i]);
                task->spec.peer.port = -1;
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
                ++dest_count;
            } else {
                log_msg(-1, "too many sources, max sources = %d", (int)(sizeof(sinks)/sizeof(sinks[0])));
            }
        }
        else if (strcmp(argv[i], "--port") == 0 ) {
            ARG_AVAIL(1);
            global_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 ) {
            ARG_AVAIL(1);
            timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mac-timeout") == 0 ) {
            ARG_AVAIL(1);
            mac_timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-level") == 0 ) {
            ARG_AVAIL(1);
            log_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--proto-version") == 0 ) {
            ARG_AVAIL(1);
            forced_proto_version = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--remove-shm") == 0) {
            keep_shm = false;
        } else if (strcmp(argv[i], "--stats-int") == 0 ) {
            ARG_AVAIL(1);
            stats_int = atoi(argv[++i]);
//        } else if (strcmp(argv[i], "--not-discoverable") == 0) {
//            discoverable = false;
        } else if (strcmp(argv[i], "--auto") == 0) {
            auto_add = true;
        } else if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--daemonise") == 0) {
            daemonise = true;
        } else if ( (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--logfile") == 0)
                && i + 1 < argc) {
            ++i;
            logfile = argv[i];
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("vismux version %s\n", APP_VERSION);
            return 0; // Clean exit immediately
        } else if (strcmp(argv[i], "--polling-wait") == 0 ) {
            ARG_AVAIL(1);
            polling_wait_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--destination-timeout") == 0 ) {
            ARG_AVAIL(1);
            destination_timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Squeezelite Replicator Destination v%s\nUsage Options:\n", APP_VERSION);
            printf("%s  <--source <source_ip>[:port][,mac_address]> ", argv[0]);
//            printf("  [--mac-timeout <sec>] [--port <p>] [--proto-version <1|2>]  [--not-discoverable] [--remove-shm] [--stats-int <interval_secs>]\n\n");
            printf("  [--mac-timeout <sec>] [--port <p>] [--proto-version <1|2>] [--remove-shm] [--stats-int <interval_secs>]\n\n");
            printf(" --source can be repeated multiple times, once for each source\n");
            printf(" --auto add newly discovered sources\n");
            printf(" --polling-wait delay between discovery calls (default=%d)\n", polling_wait_secs);
            printf(" --destination-timeout period of disconnection on destinastion, after which to stop the destination (default=%d)\n", destination_timeout_secs);
            printf("Global Flags:\n");
            printf("  -h, --help        Display this help message\n");
            printf("  -v, --version     Display application version details\n");
            printf("  --log-level <0-3> Filter verbosity (0=ERR, 1=WARN, 2=INFO, 3=DBG)\n\n");
            printf("Interactive Controls (does not require Enter):\n");
            printf("  Press 'v'         Version - Display application version details\n");
            printf("  Press 'q'         Quit - Request shutdown\n");
            printf("  Press 'l'         Log Level - Cycle log levels dynamically (0=ERROR -> 1=WARN -> 2=INFO -> 3=DEBUG)\n");
            printf("  Press 's'         Stats - Request stats summary on next data reception\n");
            return 1;
        } else {
            fprintf(stderr, "Unrecognized option: %s. %s\n", argv[i], HELP_HINT);
            return 1;
        }
    }

    if (NULL != logfile) {
        if (daemonise && !freopen(logfile, "a", stdout)) {
            fprintf(stderr, "Error opening log file %s: %s\n", logfile, strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (!freopen(logfile, "a", stderr)) {
            fprintf(stderr, "Error opening log file %s: %s\n", logfile, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (daemonise) {
        if (daemon(0, logfile ? 1: 0)) {
            fprintf(stderr, "Failed to run as daemon: %s\n", strerror(errno));
        }
    } else {
        if (isatty(STDIN_FILENO)) {
            ui_thread = create_thread(NULL, console_listener_thread, NULL);
        }
    }

    for (int i = 0; i < argc; ++i) {
        if (i) {
            printf(" ");
        }
        printf("%s", argv[i]);
    }
    puts("");
    fflush(stdout);

    if (dest_count == 0) {
        log_msg(-1, "No sources specified, turning on auto");
        destination_sink_manager(sinks, (int)(sizeof(sinks)/sizeof(sinks[0])), polling_wait_secs, destination_timeout_secs, true);
    } else {
        destination_sink_manager(sinks, (int)(sizeof(sinks)/sizeof(sinks[0])), polling_wait_secs, destination_timeout_secs,  auto_add);
    }

    log_msg(2, "Terminating no destination threads are running");
    keep_running = 0;
    join_thread(&ui_thread);

    return 0;
}
