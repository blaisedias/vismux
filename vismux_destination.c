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

static void release_system_resources(destination_context_t* ctxt)
{
    if (ctxt->shm_ptr != MAP_FAILED)
        munmap(ctxt->shm_ptr, sizeof(vis_t));
    if (ctxt->shm_fd != -1)
        close(ctxt->shm_fd);
    if (!keep_shm && ctxt->shm_path[0] != '\0')
    {
        shm_unlink(ctxt->shm_path);
    }
    if (ctxt->sock_fd != -1)
        close(ctxt->sock_fd);
    log_msg(1, "%s: Destination Engine offline.", ctxt->mac);
    if (ctxt->mac) {
        free(ctxt->mac);
        ctxt->mac = NULL;
    }
}

void init_destination_shm(vis_t *shm_ptr)
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&shm_ptr->rwlock, &attr);
    pthread_rwlockattr_destroy(&attr);

    // Nests variables inside .hdr target layout
    shm_ptr->buf_size = VIS_BUF_SIZE;
    shm_ptr->buf_index = 0;
    shm_ptr->running = false;
    shm_ptr->rate = 44100;
    shm_ptr->updated = (uint64_t)time(NULL);
    memset(shm_ptr->buffer, 0, sizeof(shm_ptr->buffer));
}

static bool setup_destination_shm(destination_context_t* ctxt)
{
    int oflag = O_CREAT | O_RDWR;
    if (!keep_shm) {
        oflag |= O_EXCL;
    }
    ctxt->shm_fd = shm_open(ctxt->shm_path, oflag, 0666);
    if (ctxt->shm_fd == -1)
    {
        perror("SHM open error");
        return false;
    }

    // Check if the memory size has already been configured
    struct stat shm_stat;
    if (fstat(ctxt->shm_fd, &shm_stat) == -1)
    {
        perror("SHM fstat error");
        close(ctxt->shm_fd);
        ctxt->shm_fd = -1;
        return false;
    }

    // Only ftruncate if the segment is brand new (size is 0) - because macOS does not support setting size on pre-existing SHM
	bool is_creator = (shm_stat.st_size == 0);
    if (is_creator)
    {
        if (ftruncate(ctxt->shm_fd, sizeof(vis_t)) == -1)
        {
            perror("SHM truncate error");
            close(ctxt->shm_fd);
            ctxt->shm_fd = -1;
            return false;
        }
    }

    ctxt->shm_ptr = (vis_t *)mmap(0, sizeof(vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, ctxt->shm_fd, 0);
    if (ctxt->shm_ptr == MAP_FAILED)
    {
        perror("SHM mapping error");
        close(ctxt->shm_fd);
        ctxt->shm_fd = -1;
        return false;
    }

    // Initialise if we created it - with risk of corrupted data from earlier run being present
	// or fighting with some other application (Squezelite is obvious candidate) - expect issues if local Squeezelite using same memory
	if (is_creator)
    {
        init_destination_shm(ctxt->shm_ptr);
    }

    return true;
}

void *heartbeat_loop(void *arg)
{
    hb_ctx_t *ctx = (hb_ctx_t *)arg;
    msg_hdr_t req_hdr = {.protocol_version = forced_proto_version, .type = PACKET_REQ, .sequence = 0, .payload_len = 0};

    while (keep_running && *ctx->keep_running)
    {
        // Dynamically read the active socket pointer directly out of the context wrapper
        int current_fd = *(ctx->sock_fd_ptr);
        if (current_fd >= 0)
        {
            sendto(current_fd, &req_hdr, sizeof(msg_hdr_t), 0, (struct sockaddr *)&ctx->server_addr, sizeof(ctx->server_addr));
        }
        for (int i = 0; i < 10 && keep_running && *ctx->keep_running; i++)
            usleep(100000);
    }
    free(ctx);
    return NULL;
}

void run_destination(destination_spec_t* spec)
{
    pthread_t* hb_thread = NULL;
    pthread_t* disc_thread = NULL;

    destination_context_t ctxt = {
        .shm_fd = -1,
        .sock_fd = -1,
        .shm_ptr = NULL,
    };
    if (!spec->keep_running) {
        log_msg(-1, "%s spec set to not run", spec->mac);
        return;
    }
    if (!validate_and_format_mac(spec->mac, ctxt.shm_path, sizeof(ctxt.shm_path))) {
        log_msg(-1, "Invalid MAC address %s", spec->mac);
    }
    ctxt.mac = strdup(spec->mac);
    bool shm_ready = ctxt.shm_path[0] != '\0';
    if (shm_ready && !setup_destination_shm(&ctxt)) {
        exit(EXIT_FAILURE);
    }

    log_msg(2, "%s: Destination Engine Online (%s) expecting data from: %s:%d", ctxt.mac, APP_VERSION, spec->server_ip, port);

    ctxt.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, spec->server_ip, &server_addr.sin_addr);

    struct sockaddr_in local_bound_addr;
    socklen_t local_bound_len = sizeof(local_bound_addr);
    struct sockaddr_in any_addr = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY};
    if (bind(ctxt.sock_fd, (struct sockaddr *)&any_addr, sizeof(any_addr)))
    {
        log_msg(0, "%s: bind to ephemeral port failed (1)", ctxt.mac);
        release_system_resources(&ctxt);
        exit(EXIT_FAILURE);
    }
    if (getsockname(ctxt.sock_fd, (struct sockaddr *)&local_bound_addr, &local_bound_len) == 0)
        log_msg(3, "%s: Outbound ephemeral port: %d", ctxt.mac, ntohs(local_bound_addr.sin_port));

    hb_ctx_t *hb_ctx = (hb_ctx_t *)malloc(sizeof(hb_ctx_t));
    hb_ctx->sock_fd_ptr = &ctxt.sock_fd;
    hb_ctx->server_addr = server_addr;
    hb_ctx->keep_running = &spec->keep_running;

    hb_thread = create_thread(NULL, heartbeat_loop, hb_ctx);

    if (spec->discoverable) {
        disc_thread = run_discovery_responder(2, spec->mac);
    }

    char rx_window[sizeof(msg_hdr_t) + sizeof(vis_t)];
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(ctxt.sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint64_t total_received_frames = 0;
    uint64_t total_dropped_frames = 0;
    uint64_t total_full_frames = 0;
    uint32_t last_network_seq = 0;
    uint64_t total_received_bytes = 0;
    bool first_frame = true;
    time_t last_stats_log_time = time(NULL), last_success_packet_time = time(NULL);
    bool was_connection_logged_down = false;
    msg_hdr_t instant_req = {.protocol_version = forced_proto_version, .type = PACKET_REQ, .sequence = 0, .payload_len = 0};

    time_t last_rotation_time = 0;
    time_t last_mac_response_time = time(NULL);
    bool is_link_active = shm_ready;

    while (keep_running && spec->keep_running)
    {
        time_t now_check = time(NULL);
        ssize_t bytes_in = recvfrom(ctxt.sock_fd, rx_window, sizeof(rx_window), 0, NULL, NULL);
        if (bytes_in < 0)
        {
            bool mac_negotiation_timed_out = !shm_ready &&
                                             (now_check - last_mac_response_time >= mac_timeout_secs);
            bool link_timed_out = is_link_active &&
                                  (now_check - last_success_packet_time >= timeout_secs);
            if (mac_negotiation_timed_out || link_timed_out)
            {
                int rotation_interval = mac_negotiation_timed_out ? mac_timeout_secs : timeout_secs;
                if (now_check - last_rotation_time >= rotation_interval)
                {
                    if (!was_connection_logged_down)
                    {
                        if (mac_negotiation_timed_out)
                            log_msg(3, "%s: No valid MAC received from source. Rotating ports...", ctxt.mac);
                        else
                            log_msg(1, "%s: Source pipeline stopped. Rotating ports...", ctxt.mac);
                        was_connection_logged_down = true;
                    }
                    last_rotation_time = now_check;
                    close(ctxt.sock_fd);
                    ctxt.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
                    if (ctxt.sock_fd >= 0)
                    {
                        if (bind(ctxt.sock_fd, (struct sockaddr *)&any_addr, sizeof(any_addr)))
                        {
                            log_msg(0, "%s: bind to ephemeral port failed (2)", ctxt.mac);
                            release_system_resources(&ctxt);
                            exit(EXIT_FAILURE);
                        }
                        setsockopt(ctxt.sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        local_bound_len = sizeof(local_bound_addr);
                        if (getsockname(ctxt.sock_fd, (struct sockaddr *)&local_bound_addr, &local_bound_len) == 0)
                            log_msg(3, "%s: Recovery port switch: %d", ctxt.mac, ntohs(local_bound_addr.sin_port));

                        first_frame = true;
                        last_network_seq = 0;

                        sendto(ctxt.sock_fd, &instant_req, sizeof(msg_hdr_t), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
                    }
                    last_mac_response_time = now_check;
                    last_success_packet_time = now_check;
                }
                continue;
            }
            if (errno == ECONNREFUSED)
            {
                close(ctxt.sock_fd);
                ctxt.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (ctxt.sock_fd >= 0)
                {
                    if (bind(ctxt.sock_fd, (struct sockaddr *)&any_addr, sizeof(any_addr)))
                    {
                        log_msg(0, "%s: bind to ephemeral port failed (3)", ctxt.mac);
                        release_system_resources(&ctxt);
                        exit(EXIT_FAILURE);
                    }
                    setsockopt(ctxt.sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                    // Re-inject instant request frame on connection refusal recovery triggers
                    sendto(ctxt.sock_fd, &instant_req, sizeof(msg_hdr_t), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
                }
                last_success_packet_time = now_check;
            }
            continue;
        }

        if (bytes_in < (ssize_t)sizeof(msg_hdr_t))
            continue;
        msg_hdr_t *msg = (msg_hdr_t *)rx_window;
        size_t wire_headers_sz = sizeof(vis_wire_hdr_t);

        if (msg->type == PACKET_SUB_ACK)
        {
            if (msg->payload_len == sizeof(subscription_response_t) &&
                (size_t)bytes_in >= sizeof(msg_hdr_t) + msg->payload_len)
            {
                subscription_response_t *response = (subscription_response_t *)(rx_window + sizeof(msg_hdr_t));
                if (!shm_ready)
                {
                    char negotiated_shm_path[sizeof(ctxt.shm_path)];
                    if (validate_and_format_mac(response->mac, negotiated_shm_path, sizeof(negotiated_shm_path)))
                    {
                        snprintf(ctxt.shm_path, sizeof(ctxt.shm_path), "%s", negotiated_shm_path);
                        if (setup_destination_shm(&ctxt))
                        {
                            shm_ready = true;
                            first_frame = true;
                            last_network_seq = 0;
                            log_msg(2, "%s: Destination SHM initialized from source MAC: %s", ctxt.mac, response->mac);
                            free(ctxt.mac);
                            ctxt.mac = strdup(response->mac);
                        }
                        else
                        {
                            ctxt.shm_path[0] = '\0';
                        }
                    }
                    else
                    {
                        log_msg(1, "%s: Source subscription response contained an invalid MAC address.", ctxt.mac);
                    }
                }
                if (shm_ready)
                {
                    last_success_packet_time = now_check;
                    is_link_active = true;
                }
            }
            continue;
        }

        if (msg->type == PACKET_DATA)
        {
            if (!shm_ready || (!first_frame && msg->sequence < last_network_seq && (last_network_seq - msg->sequence) < 100000))
            {
                total_dropped_frames++;
                continue;
            }
            last_network_seq = msg->sequence;

            last_success_packet_time = now_check;
            if (was_connection_logged_down)
            {
                log_msg(1, "%d: Connection re-established.", ctxt.mac);
                was_connection_logged_down = false;
            }
            total_received_frames++;
            total_received_bytes += msg->payload_len;
            bool is_full_frame = msg->payload_len >= wire_headers_sz &&
                                 msg->payload_len - wire_headers_sz == sizeof(ctxt.shm_ptr->buffer);
            if (is_full_frame)
                total_full_frames++;

			int current_level = STATS_LOG_LEVEL;

			if ( atomic_load(&force_stats_log) != 0 )
			{
				current_level = -1;	// NOTIFY
			}

            time_t current_time = time(NULL);
            if (first_frame || current_time - last_stats_log_time >= stats_int || current_level != STATS_LOG_LEVEL)
            {
				
				
                log_msg(current_level, "%s: Processing updates: [Total frames: captured: %" PRIu64 ", dropped: %" PRIu64 ", full: %" PRIu64 "] [Total Data: %.2f MB]",
                        ctxt.mac,
                        total_received_frames,
                        total_dropped_frames,
                        total_full_frames,
                        (double)total_received_bytes / (1024.0 * 1024.0)
                        );
				
                last_stats_log_time = current_time;
            }
			
			if (current_level != STATS_LOG_LEVEL)
			{
				atomic_store(&force_stats_log, 0);
			}

            if (msg->payload_len <= wire_headers_sz || msg->payload_len > (wire_headers_sz + sizeof(ctxt.shm_ptr->buffer)) || (size_t)bytes_in < (sizeof(msg_hdr_t) + msg->payload_len))
                continue;

            pthread_rwlock_wrlock(&ctxt.shm_ptr->rwlock);
            char *incoming_audio_payload = rx_window + sizeof(msg_hdr_t) + wire_headers_sz;
            size_t received_audio_bytes = msg->payload_len - wire_headers_sz;
            if (received_audio_bytes == sizeof(ctxt.shm_ptr->buffer))
            {
                memcpy(ctxt.shm_ptr->buffer, incoming_audio_payload, received_audio_bytes);
            }
            else
            {
                uint32_t incoming_samples = received_audio_bytes / sizeof(int16_t);
                for (uint32_t i = 0; i < incoming_samples; i++)
                {
                    uint32_t target_idx = (ctxt.shm_ptr->buf_index - incoming_samples + i + VIS_BUF_SIZE) % VIS_BUF_SIZE;
                    ctxt.shm_ptr->buffer[target_idx] = ((int16_t *)incoming_audio_payload)[i];
                }
            }

            vis_wire_hdr_t* wirehdr = (vis_wire_hdr_t*)(rx_window + sizeof(msg_hdr_t));
            ctxt.shm_ptr->buf_size = ntohl(wirehdr->buf_size);
            ctxt.shm_ptr->buf_index = ntohl(wirehdr->buf_index);
            ctxt.shm_ptr->running = (bool)ntohl(wirehdr->running);
            ctxt.shm_ptr->rate = ntohl(wirehdr->rate);
            ctxt.shm_ptr->updated = (uint64_t)time(NULL);

            first_frame = false;
            pthread_rwlock_unlock(&ctxt.shm_ptr->rwlock);
            msg_hdr_t ack_hdr = {.protocol_version = msg->protocol_version, .type = PACKET_ACK, .sequence = msg->sequence, .payload_len = 0};
            sendto(ctxt.sock_fd, &ack_hdr, sizeof(msg_hdr_t), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
        }
    }
    join_thread(&hb_thread);
    join_thread(&disc_thread);
    release_system_resources(&ctxt);
}

void* run_destination_thread(void* arg) {
    if (arg ) {
        destination_spec_t spec;
        memcpy(&spec, arg, sizeof(spec));
        run_destination(&spec);
    } else {
        log_msg(-1, "run_destination_thread with NULL argument");
    }
    return NULL;
}

