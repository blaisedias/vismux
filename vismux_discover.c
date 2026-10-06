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

int main(int argc, char *argv[])
{
    bool fmt_cmdline = false;
    uint8_t role_filter = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--source") == 0 ) {
            role_filter = DISCOVER_ROLE_SOURCE;
        } else if (strcmp(argv[i], "--destination") == 0 ) {
            role_filter = DISCOVER_ROLE_DESTINATION;
        } else if (strcmp(argv[i], "--discover-timeout") == 0 && i + 1 < argc) {
            discover_timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--discover-port") == 0 && i + 1 < argc) {
            discover_port = atoi(argv[++i]);
        } else if (0 == strcmp(argv[i], "--format-commandline")) {
            fmt_cmdline = true;
        } else {
            fprintf(stderr, "Unrecognized option: %s.\n", argv[i]);
            return 1;
        }
    }

    discover_records_t* discovery  = run_discovery_prober(role_filter);
    if (discovery) {
        for (uint8_t role=1; role <3; ++role) {
            for (int ix =0; ix < discovery->count; ++ix) {
                if ((discovery->records[ix].role != role)) {
                    continue;
                }
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &discovery->records[ix].ip, ip_str, INET_ADDRSTRLEN);

                if (role_filter == DISCOVER_ROLE_SOURCE && fmt_cmdline) {
                    printf("--source '%s:%u,%s'\n",
                        ip_str,
                        discovery->records[ix].port,
                        discovery->records[ix].mac);
                } else {
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
}
