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


discover_records_t* run_discovery_prober(uint8_t role_filter)
{
    int probe_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe_fd < 0)
    {
        perror("Discovery socket creation failed");
        exit(EXIT_FAILURE);
    }

    int broadcast_enable = 1;
    setsockopt(probe_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(probe_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in target_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(discover_port),
        .sin_addr.s_addr = inet_addr("255.255.255.255")};

    disc_req_packet_t probe_req;
    memcpy(probe_req.magic, DISCOVER_MAGIC, sizeof(DISCOVER_MAGIC));
    probe_req.type = PACKET_REQ;

    sendto(probe_fd, &probe_req, sizeof(disc_req_packet_t), 0,
           (struct sockaddr *)&target_addr, sizeof(target_addr));

    disc_resp_packet_t resp;
    struct sockaddr_in sender_addr;
    time_t start_time = time(NULL);

#define MAX_SEEN_PEERS 64
    peer_record_t seen_records[MAX_SEEN_PEERS];
    int seen_count = 0;
    memset(seen_records, 0, sizeof(seen_records));

    while (time(NULL) - start_time < discover_timeout_secs)
    {
        memset(&resp, 0, sizeof(resp));
        socklen_t addr_len = sizeof(sender_addr);
        ssize_t len = recvfrom(probe_fd, &resp, sizeof(disc_resp_packet_t), 0,
                               (struct sockaddr *)&sender_addr, &addr_len);

        if (len >= (ssize_t)offsetof(disc_resp_packet_t, version) && resp.type == PACKET_ACK)
        {
            if (strncmp(resp.magic, DISCOVER_MAGIC, sizeof(DISCOVER_MAGIC)) == 0)
            {
                uint32_t response_port = ntohl(resp.port);
                const char *response_version = len >= (ssize_t)sizeof(disc_resp_packet_t) && resp.version[0] != '\0'
                                                    ? resp.version
                                                    : "unknown";

                // Evaluate deduplication across the entire combined data snapshot
                bool is_duplicate = false;
                for (int i = 0; i < seen_count; i++)
                {
                    if (seen_records[i].ip == sender_addr.sin_addr.s_addr &&
                        seen_records[i].port == response_port &&
                        strcmp(seen_records[i].mac, resp.mac) == 0)
                    {
                        is_duplicate = true;
                        break;
                    }
                }

                if (is_duplicate)
                    continue; // Skip identical duplicate records cleanly

                // Save unique dataset entry to the cache register array
                if (seen_count < MAX_SEEN_PEERS)
                {
                    seen_records[seen_count].ip = sender_addr.sin_addr.s_addr;
                    seen_records[seen_count].port = response_port;
                    seen_records[seen_count].role = resp.role;
                    snprintf(seen_records[seen_count].mac, sizeof(seen_records[seen_count].mac), "%s", resp.mac);
                    snprintf(seen_records[seen_count].version, sizeof(seen_records[seen_count].version), "%s", response_version);
                    seen_count++;
                }
            }
        }
    }
    close(probe_fd);
    discover_records_t* dr = calloc(1, sizeof(*dr));
    if (dr) {
        for (int ix = 0; ix < seen_count && ix < MAX_SEEN_PEERS; ++ix) {
            if (role_filter == 0 || role_filter == seen_records[ix].role) {
                memcpy(dr->records + dr->count, seen_records+ix, sizeof(dr->records[0]));
                ++dr->count;
            }
        }
    } else {
        fprintf(stderr, "Out of memory\n");
    }
    return dr;
}
