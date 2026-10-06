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

volatile sig_atomic_t keep_running = 1;
atomic_int log_level = 2;	// Atomic because it can be written to from console_listener_thread and read from other threads
atomic_int force_stats_log = 0;	// Atomic because it can be written to from console_listener_thread and read/reset from other threads
#define STATS_LOG_LEVEL 3
#define LOG_BUF_SIZE 1024	// Maximum length of single log message
#define TRUNC_TAG " ... [TRUNCATED]"
int global_port = DEFAULT_PORT;
int target_fps = DEFAULT_FPS;
int stats_int = DEFAULT_STATS_INTERVAL;
int timeout_secs = 2;
int mac_timeout_secs = DEFAULT_MAC_TIMEOUT;
int forced_proto_version = 2;
bool keep_shm = true;
bool wait_for_shm = false;
bool has_interactive_tty = false;	// set in main
int discover_timeout_secs = 2;
int discover_port = DISCOVER_PORT; // Overridable runtime discovery port descriptor
int discover_format = 0;

// Allocate pthread_t struct, and creates a thread
// Returns  pointer to pthread_t or  NULL if memory allocation or thread creation failed
pthread_t* create_thread(const pthread_attr_t*  attr,
                        void* (*start_routine)(void*),
                        void* arg) {
    pthread_t* pt = calloc(1, sizeof(*pt));
    if (pt) {
        if (pthread_create(pt, attr, start_routine, arg)) {
            free(pt);
            pt = NULL;
        }
    }
    return pt;
}

// perform a join on a thread, previously created by create_thread.
// Takes a pointer to pthread_t*,
//  checks contents for NULL,
//  if non NULL:
//    - performs a join,
//    - frees associated memory
//    - sets the pointer to pthread_t to NULL
void join_thread(pthread_t** ppt) {
    if (*ppt) {
        pthread_t* pt = *ppt;
        pthread_join(*pt, NULL);
        free((void*)pt);
        *ppt = NULL;
    }
}

void log_msg(int level, const char *fmt, ...)
{	// Thread safe print of log entry
    if (level <= atomic_load(&log_level))
    {
        time_t now = time(NULL);
        char t_str[32];
        struct tm *tm_info = localtime(&now);
        strftime(t_str, sizeof(t_str), "%Y-%m-%d %H:%M:%S", tm_info);

        const char *lbl = "INFO";
        if (level == -1)
            lbl = "NOTICE";
        else if (level == 0)
            lbl = "ERROR";
        else if (level == 1)
            lbl = "WARN";
        else if (level == 3)
            lbl = "DEBUG";
        else if (level == 4)
            lbl = "VERBOSE";

        va_list args;
        va_start(args, fmt);

        char msg_buffer[LOG_BUF_SIZE];
        // vsnprintf returns the length the string *would* have been
        int result = vsnprintf(msg_buffer, sizeof(msg_buffer), fmt, args);

        va_end(args);

        // Check if the message was truncated
        if (result >= (int)sizeof(msg_buffer))
        {
            // Calculate where to overlay the truncation tag at the end of the buffer
            size_t overwrite_pos = sizeof(msg_buffer) - strlen(TRUNC_TAG) - 1;

            // Append the tag cleanly, ensuring a null terminator is kept
			snprintf(&msg_buffer[overwrite_pos], strlen(TRUNC_TAG) + 1, "%s", TRUNC_TAG);
        }

        // Print everything out in ONE atomic call
        printf("[%s] [%s] %s\n", t_str, lbl, msg_buffer);
        fflush(stdout);
    }
}

void handle_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "\nGot signal %d. Terminating\n", sig);
    keep_running = 0;
}

bool validate_and_format_mac(const char *mac_in, char *shm_out, size_t out_len)
{
    int hex_chars = 0;
    char clean_mac[12] = {0};
    for (int i = 0; mac_in[i] != '\0'; i++)
    {
        if (isxdigit((unsigned char)mac_in[i]))
        {
            if (hex_chars < 12)
                clean_mac[hex_chars++] = tolower((unsigned char)mac_in[i]);
            else
                return false;
        }
        else if (mac_in[i] != ':' && mac_in[i] != '-')
            return false;
    }
    if (hex_chars != 12)
        return false;
    snprintf(shm_out, out_len, "/%s%c%c:%c%c:%c%c:%c%c:%c%c:%c%c", SQUEEZELITE_SHM_PREFIX,
             clean_mac[0], clean_mac[1], clean_mac[2], clean_mac[3],
             clean_mac[4], clean_mac[5], clean_mac[6], clean_mac[7],
             clean_mac[8], clean_mac[9], clean_mac[10], clean_mac[11]);
    return true;
}

bool validate_mac_spec(const char *mac_in)
{
    int hex_chars = 0;
    for (int i = 0; mac_in[i] != '\0'; i++)
    {
        if (isxdigit((unsigned char)mac_in[i]))
        {
            if (hex_chars < 12)
                hex_chars++;
            else
                return false;
        }
        else if (mac_in[i] != ':' && mac_in[i] != '-')
            return false;
    }
    if (hex_chars != 12)
        return false;
    return true;
}

static void *discovery_responder_thread(void *arg)
{
    discovery_responder_spec_t spec;
    memcpy(&spec, arg, sizeof(spec));
    free(arg);

    int disc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (disc_fd < 0)
        return NULL;

    int reuse = 1;
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in disc_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(discover_port),
        .sin_addr.s_addr = INADDR_ANY};

    if (bind(disc_fd, (struct sockaddr *)&disc_addr, sizeof(disc_addr)) < 0)
    {
        log_msg(1, "Discovery port %d is already in use by another application! "
                   "Background service discovery responder is disabled.",
                discover_port);
        close(disc_fd);
        return NULL;
    }

    disc_req_packet_t rx_packet;
    struct sockaddr_in client_addr;

    // continue if global keep_running or if context specific keep running pointer
    // is set and its contents are "true"
    while (keep_running && (spec.keep_running == NULL || *spec.keep_running))
    {
        fd_set rfds;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        FD_ZERO(&rfds);
        FD_SET(disc_fd, &rfds);

        int retval = select(disc_fd + 1, &rfds, NULL, NULL, &tv);
        if (retval > 0 && FD_ISSET(disc_fd, &rfds))
        {
            socklen_t addr_len = sizeof(client_addr);
            ssize_t len = recvfrom(disc_fd, &rx_packet, sizeof(disc_req_packet_t), 0,
                                   (struct sockaddr *)&client_addr, &addr_len);

            if (len == sizeof(disc_req_packet_t) && rx_packet.type == PACKET_REQ)
            {
                if (strncmp(rx_packet.magic, DISCOVER_MAGIC, sizeof(DISCOVER_MAGIC)) == 0)
                {
                    disc_resp_packet_t tx_packet;
                    memset(&tx_packet, 0, sizeof(tx_packet));

                    memcpy(tx_packet.magic, DISCOVER_MAGIC, sizeof(DISCOVER_MAGIC));
                    tx_packet.type = PACKET_ACK;
                    tx_packet.role = (uint8_t)spec.role_id;
                    tx_packet.port = htonl((uint32_t)spec.port);
                    snprintf(tx_packet.version, sizeof(tx_packet.version), "%s-%s", VENDOR_STR, APP_VERSION);
                    snprintf(tx_packet.mac, sizeof(tx_packet.mac), "%s", spec.mac);

                    char target_ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, target_ip_str, INET_ADDRSTRLEN);

                    // Debug log output tracing the dynamic network reply dispatch
                    log_msg(4, "Dispatching discovery response packet back to prober host: %s:%d",
                            target_ip_str, ntohs(client_addr.sin_port));

                    sendto(disc_fd, &tx_packet, sizeof(disc_resp_packet_t), 0,
                           (struct sockaddr *)&client_addr, sizeof(client_addr));
                }
            }
        }
    }
    close(disc_fd);
    return NULL;
}

pthread_t* run_discovery_responder(int role_id, const char* mac, int port, volatile bool *keep_running) {
    discovery_responder_spec_t* responder_spec = calloc(1, sizeof(*responder_spec));
    responder_spec->role_id = role_id;
    responder_spec->port = port;
    responder_spec->keep_running = keep_running;
    strcpy(responder_spec->mac, mac);
    return create_thread(NULL, discovery_responder_thread, responder_spec);
}

int is_ipaddr_local(const uint32_t s_addr) {
    struct ifaddrs* ifaddr;
    // default to not local IP address
    bool rv = 1;

    if (getifaddrs(&ifaddr) == -1) {
        log_msg(-1, "getifaddrs() failed: %s", strerror(errno));
        return -1;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) { continue; }
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (s_addr == ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr) {
                // IP address is local
                rv = 0;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);

    return rv;
}
