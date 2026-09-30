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

typedef struct {
    char shm_path[128];
    int  shm_fd;
    vis_t* shm_ptr;
    int sock_fd;
} source_context_t;

static void release_system_resources(source_context_t* ctxt)
{
    if (ctxt->shm_ptr != MAP_FAILED)
        munmap(ctxt->shm_ptr, sizeof(vis_t));
    if (ctxt->shm_fd != -1)
        close(ctxt->shm_fd);
    if (ctxt->sock_fd != -1)
        close(ctxt->sock_fd);
    log_msg(1, "Source Engine offline.");
}

void run_source(const char* _shm_path, const char* mac, bool discoverable)
{
    if (0 == strlen(_shm_path)) {
        log_msg(0, "Empty shared memory path");
        exit(EXIT_FAILURE);
    }
    source_context_t ctxt = {
        .shm_path = {0},
        .shm_fd = -1,
        .shm_ptr = MAP_FAILED,
        .sock_fd = -1,
    };
    strcpy(ctxt.shm_path, _shm_path);
    ctxt.shm_fd = shm_open(ctxt.shm_path, O_RDWR, 0666);
    if (ctxt.shm_fd == -1)
    {
        log_msg(0, "Failed to open SHM %s. Is Squeezelite running with visualiser enabled (-v)?", ctxt.shm_path);
        exit(EXIT_FAILURE);
    }
    ctxt.shm_ptr = (vis_t *)mmap(0, sizeof(vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, ctxt.shm_fd, 0);
    ctxt.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(ctxt.sock_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (bind(ctxt.sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)))
    {
        log_msg(1, "port %d is already in use! ", (int)server_addr.sin_port);
        release_system_resources(&ctxt);
        exit(EXIT_FAILURE);
    }

    destination_t clients[MAX_DESTINATIONS];
    memset(clients, 0, sizeof(clients));
    log_msg(2, "Source Engine Online (%s) tracking SHM: %s", APP_VERSION, ctxt.shm_path);

    char source_mac[18] = {0};
    char *source_mac_ptr = strchr(ctxt.shm_path, '-');
    if (source_mac_ptr)
        snprintf(source_mac, sizeof(source_mac), "%s", source_mac_ptr + 1);

    uint32_t seq_counter = 0;
    useconds_t sleep_interval = 1000000 / target_fps;
    char net_buf[sizeof(msg_hdr_t) + sizeof(vis_t)];
    pthread_t* disc_thread = NULL;
    if (discoverable) {
        disc_thread = run_discovery_responder(1, mac);
    }

    while (keep_running)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        msg_hdr_t incoming_hdr;

        while (recvfrom(ctxt.sock_fd, &incoming_hdr, sizeof(msg_hdr_t), 0, (struct sockaddr *)&client_addr, &addr_len) > 0)
        {
            time_t now = time(NULL);
            int match_idx = -1, free_idx = -1;
            for (int i = 0; i < MAX_DESTINATIONS; i++)
            {
                if (clients[i].active && clients[i].addr.sin_addr.s_addr == client_addr.sin_addr.s_addr && clients[i].addr.sin_port == client_addr.sin_port)
                {
                    match_idx = i;
                    break;
                }
                if (!clients[i].active && free_idx == -1)
                    free_idx = i;
            }
            if (incoming_hdr.type == PACKET_REQ)
            {
                if (match_idx != -1)
                    clients[match_idx].last_seen = now;
                else if (free_idx != -1)
                {
                    clients[free_idx].addr = client_addr;
                    clients[free_idx].last_seen = now;
                    clients[free_idx].proto_version = incoming_hdr.protocol_version;
                    clients[free_idx].last_sent_index = 0;
                    clients[free_idx].last_update_time = 0;
                    clients[free_idx].was_running_cache = true;
                    clients[free_idx].force_initial_sync = true;
                    clients[free_idx].active = true;

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
                    log_msg(3, "New active visualiser subscription established from: %s:%d (Proto v%d)",
                            ip_str, ntohs(client_addr.sin_port), incoming_hdr.protocol_version);
                }

                if (match_idx != -1 || free_idx != -1)
                {
                    subscription_response_t response_payload;
                    memcpy(response_payload.mac, source_mac, sizeof(response_payload.mac));
                    msg_hdr_t response_hdr = {
                        .protocol_version = incoming_hdr.protocol_version,
                        .type = PACKET_SUB_ACK,
                        .sequence = 0,
                        .payload_len = sizeof(response_payload)};
                    char response_buf[sizeof(msg_hdr_t) + sizeof(response_payload)];
                    memcpy(response_buf, &response_hdr, sizeof(response_hdr));
                    memcpy(response_buf + sizeof(response_hdr), &response_payload, sizeof(response_payload));
                    sendto(ctxt.sock_fd, response_buf, sizeof(response_buf), 0,
                           (struct sockaddr *)&client_addr, sizeof(client_addr));
                }
            }
            else if (incoming_hdr.type == PACKET_ACK && match_idx != -1)
            {
                clients[match_idx].last_seen = now;
            }
        }

        time_t now = time(NULL);
        bool has_active_clients = false;
        for (int i = 0; i < MAX_DESTINATIONS; i++)
        {
            if (clients[i].active)
            {
                if (now - clients[i].last_seen > timeout_secs)
                {
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &clients[i].addr.sin_addr, ip_str, sizeof(ip_str));
                    log_msg(3, "Visualiser client timed out: %s:%d (after %d seconds)",
                            ip_str, ntohs(clients[i].addr.sin_port), timeout_secs);
                    clients[i].active = false;
                }
                else
                    has_active_clients = true;
            }
        }
        if (has_active_clients)
        {
            vis_wire_hdr_t current_hdr;
            int16_t current_buffer[VIS_BUF_SIZE];
            pthread_rwlock_rdlock(&ctxt.shm_ptr->rwlock);
            current_hdr.buf_size = htonl(ctxt.shm_ptr->buf_size);
            current_hdr.buf_index = htonl(ctxt.shm_ptr->buf_index);
            uint32_t current_idx = ctxt.shm_ptr->buf_index;
            current_hdr.running = htonl((int)ctxt.shm_ptr->running);
            bool current_running = (int)ctxt.shm_ptr->running;
            current_hdr.rate = htonl(ctxt.shm_ptr->rate);
            memcpy(current_buffer, ctxt.shm_ptr->buffer, sizeof(current_buffer));
            time_t current_update_time = ctxt.shm_ptr->updated;
            pthread_rwlock_unlock(&ctxt.shm_ptr->rwlock);

            for (int i = 0; i < MAX_DESTINATIONS; i++)
            {
                if (!clients[i].active)
                    continue;
                bool send_packet = false;
                bool force_full_sync = false;
                size_t wire_headers_sz = sizeof(vis_wire_hdr_t);
                size_t payload_audio_bytes = 0;

                if (!current_running)
                {
                    if (clients[i].was_running_cache)
                    {
                        clients[i].was_running_cache = false;
                        send_packet = true;
                        force_full_sync = true;
                        payload_audio_bytes = sizeof(ctxt.shm_ptr->buffer);
                    }
                }
                else
                {
                    clients[i].was_running_cache = true;
                    if (current_idx != clients[i].last_sent_index || current_update_time != clients[i].last_update_time)
                    {
                        send_packet = true;
                        uint32_t delta_samples = (current_idx - clients[i].last_sent_index + VIS_BUF_SIZE) % VIS_BUF_SIZE;
                        if (clients[i].proto_version == 1 || clients[i].force_initial_sync || delta_samples == 0 || delta_samples >= (VIS_BUF_SIZE - 512))
                        {
                            force_full_sync = true;
                            payload_audio_bytes = sizeof(ctxt.shm_ptr->buffer);
                            clients[i].force_initial_sync = false;
                        }
                        else
                        {
                            force_full_sync = false;
                            payload_audio_bytes = delta_samples * sizeof(int16_t);
                        }
                    }
                }

                if (send_packet)
                {
                    size_t total_payload_len = wire_headers_sz + payload_audio_bytes;
                    msg_hdr_t out_hdr = {.protocol_version = clients[i].proto_version, .type = PACKET_DATA, .sequence = seq_counter++, .payload_len = total_payload_len};
                    memcpy(net_buf, &out_hdr, sizeof(msg_hdr_t));

                    memcpy(net_buf + sizeof(msg_hdr_t), &current_hdr, wire_headers_sz);

                    char *wire_audio_ptr = net_buf + sizeof(msg_hdr_t) + wire_headers_sz;
                    if (current_running)
                    {
                        if (force_full_sync)
                        {
                            memcpy(wire_audio_ptr, current_buffer, payload_audio_bytes);
                        }
                        else
                        {
                            uint32_t samples_to_copy = payload_audio_bytes / sizeof(int16_t);
                            int16_t *net_buf_audio_ptr = (int16_t *)wire_audio_ptr;
                            for (uint32_t s = 0; s < samples_to_copy; s++)
                            {
                                net_buf_audio_ptr[s] = current_buffer[(clients[i].last_sent_index + s) % VIS_BUF_SIZE];
                            }
                        }
                        clients[i].last_sent_index = current_idx;
                        clients[i].last_update_time = current_update_time;
                    }
                    else
                    {
                        memset(wire_audio_ptr, 0, payload_audio_bytes);
                        clients[i].last_sent_index = current_idx;
                        clients[i].last_update_time = current_update_time;
                    }
                    sendto(ctxt.sock_fd, net_buf, sizeof(msg_hdr_t) + total_payload_len, 0, (struct sockaddr *)&clients[i].addr, sizeof(struct sockaddr_in));
                }
            }
        }
        usleep(sleep_interval);
    }

    join_thread(&disc_thread);
    release_system_resources(&ctxt);
}
