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

#define SLOT_COUNT  16
static destination_spec_t specs[SLOT_COUNT];
static pthread_t* threads[SLOT_COUNT];

int main(int argc, char *argv[])
{
    int ix_dest = 0;
    bool daemonise = false;
    const char* logfile = NULL;
    pthread_t* ui_thread = NULL;
    bool discoverable = true;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

#define ARG_AVAIL(n)  if ((i +n) >= argc) { fprintf(stderr, "invalid commandline"); exit(EXIT_FAILURE); }
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--source") == 0 ) {
            ARG_AVAIL(2);
            if (ix_dest < (int)(sizeof(specs)/sizeof(specs[0]))) {
                const char* src_ip = argv[++i];
                const char* src_mac = argv[++i];
                struct in_addr server_addr;
                if (inet_pton(AF_INET, src_ip, &server_addr) != 1) {
                    log_msg(-1, "invalid IP address %s", src_ip);
                    exit(EXIT_FAILURE);
                }
                if (!validate_mac_spec(src_mac)) {
                    log_msg(-1, "invalid mac address %s", src_mac);
                    exit(EXIT_FAILURE);
                }
                specs[ix_dest].server_ip = strdup(src_ip);
                specs[ix_dest].mac = strdup(src_mac);
                specs[ix_dest].keep_running = true;
                ++ix_dest;
            } else {
                log_msg(-1, "too many sources, max sources = %d", (int)(sizeof(specs)/sizeof(specs[0])));
            }
        }
        else if (strcmp(argv[i], "--port") == 0 ) {
            ARG_AVAIL(1);
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 ) {
            ARG_AVAIL(1);
            target_fps = atoi(argv[++i]);
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
        } else if (strcmp(argv[i], "--no-discover") == 0) {
            discoverable = false;
        } else if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--daemonise") == 0) {
            daemonise = true;
        } else if ( (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--logfile") == 0)
                && i + 1 < argc) {
            ++i;
            logfile = argv[i];
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("vismux version %s\n", APP_VERSION);
            return 0; // Clean exit immediately
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

//    if (ix_dest) {
//        run_destination(specs + 0);
//    }
    for(int ix =0; ix < (int)(sizeof(specs)/sizeof(specs[0])); ++ix) {
        destination_spec_t* spec = specs + ix;
        spec->discoverable = discoverable;
        if (spec->keep_running && spec->server_ip && spec->mac) {
            threads[ix] = create_thread(NULL, run_destination_thread, spec);
        }
    }

    for(int ix =0; ix < (int)(sizeof(threads)/sizeof(threads[0])); ++ix) {
        join_thread(threads + ix);
    }
    join_thread(&ui_thread);

    return 0;
}
