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

#ifndef __vismux_h_
#define __vismux_h_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/select.h>
#include <termios.h>
#include <dirent.h>
#include <stdatomic.h>

#define APP_VERSION "0.0.9l"
#define VENDOR_STR "VISMUX"

#define VIS_BUF_SIZE 16384
#define MAX_DESTINATIONS 16
#define DEFAULT_PORT 23483
#define DEFAULT_STATS_INTERVAL 10 // Stats log reporting interval in seconds at Destination when active
#define DEFAULT_FPS 60
#define HELP_HINT "Pass -h for help."
#define PACKET_DATA 0x01
#define PACKET_REQ 0x02
#define PACKET_ACK 0x03
#define PACKET_SUB_ACK 0x04

#define STATS_LOG_LEVEL 3
#define LOG_BUF_SIZE 1024	// Maximum length of single log message
#define TRUNC_TAG " ... [TRUNCATED]"

// Discovery Layer Configuration Constants
#define DISCOVER_PORT (DEFAULT_PORT + 1)
#define DISCOVER_MAGIC "VISMUXv0"
#define DISCOVER_VERSION_LEN 16
#define DEFAULT_MAC_TIMEOUT 2
#define SQUEEZELITE_SHM_PREFIX "squeezelite-"
#define SQUEEZELITE_SHM_PREFIX_LEN (sizeof(SQUEEZELITE_SHM_PREFIX) - 1)

typedef struct
{
    uint32_t buf_size;
    uint32_t buf_index;
    uint32_t running;
    uint32_t rate;
} __attribute__((packed)) vis_wire_hdr_t;

typedef struct
{
    pthread_rwlock_t rwlock;
	uint32_t buf_size;
	uint32_t buf_index;
	bool running;
	uint32_t rate;
	time_t updated;
    int16_t buffer[VIS_BUF_SIZE];
} vis_t;

typedef struct
{
    struct sockaddr_in addr;
    time_t last_seen;
    uint32_t last_sent_index;
    time_t last_update_time;
    uint8_t proto_version;
    bool was_running_cache;
    bool force_initial_sync;
    bool active;
} destination_t;

typedef struct
{
    uint8_t protocol_version;
    uint8_t type;
    uint32_t sequence;
    uint32_t payload_len;
} __attribute__((packed)) msg_hdr_t;

typedef struct
{
    int *sock_fd_ptr;
    struct sockaddr_in server_addr;
    volatile bool* keep_running;
} hb_ctx_t;

typedef struct
{
    char mac[18];
} __attribute__((packed)) subscription_response_t;

// Version-Locked Discovery Wire Layouts
typedef struct
{
    char magic[sizeof(DISCOVER_MAGIC)]; // Holds: "VISMUXv0\0"
    uint8_t type;                       // Holds: PACKET_REQ
} __attribute__((packed)) disc_req_packet_t;

typedef struct
{
    char magic[sizeof(DISCOVER_MAGIC)]; // Holds: "VISMUXv0\0"
    uint8_t type;                       // Holds: PACKET_ACK
    uint8_t role;                       // 1 = SOURCE, 2 = DESTINATION
    uint32_t port;                      // Operational data streaming port (e.g. 23483)
    char mac[18];                       // Alphanumeric buffer: "2c:cf:67:82:cc:29\0"
    char version[DISCOVER_VERSION_LEN]; // Application version; absent in legacy responses
} __attribute__((packed)) disc_resp_packet_t;

typedef struct
{
    uint32_t ip;
    uint32_t port;
    char mac[18]; // Storage footprint to match clean "XX:XX:XX:XX:XX:XX\0" length bounds
    uint8_t role;
    char version[16];
} peer_record_t;

#define MAX_SEEN_PEERS 64
typedef struct {
    int count;
    peer_record_t records[MAX_SEEN_PEERS];
} discover_records_t;

typedef struct {
    int role_id;
    char mac[18];
} discovery_responder_spec_t;

typedef struct {
    int shm_fd;
    int sock_fd;
    vis_t *shm_ptr;
    char shm_path[128];
    char *mac;
}destination_context_t;

typedef struct {
    const char* server_ip;
    const char* mac;
    volatile bool keep_running;
    bool  discoverable;
} destination_spec_t;

bool validate_and_format_mac(const char *mac_in, char *shm_out, size_t out_len);
bool validate_mac_spec(const char *mac_in);
void log_msg(int level, const char *fmt, ...);
void handle_signal(int sig);

pthread_t* create_thread(const pthread_attr_t*  attr,
                        void* (*start_routine)(void*),
                        void* arg);
void join_thread(pthread_t** ppt);

void run_destination(destination_spec_t* spec);
void* run_destination_thread(void*);

void run_source(const char* _shm_path, const char* mac, bool discoverable);

void *console_listener_thread(void *arg);
// returns allocated memory, to be freed by the caller
discover_records_t* run_discovery_prober(uint8_t role_filter);
pthread_t* run_discovery_responder(int role_id, const char* mac);

// global variables defined in vismux_common.c
extern volatile sig_atomic_t keep_running;
extern atomic_int log_level;	// Atomic because it can be written to from console_listener_thread and read from other threads
extern atomic_int force_stats_log;	// Atomic because it can be written to from console_listener_thread and read/reset from other threads

extern int port;
extern int target_fps;
extern int stats_int;
extern int timeout_secs;
extern int mac_timeout_secs;
extern int forced_proto_version;
extern bool keep_shm;
extern bool wait_for_shm;
extern bool has_interactive_tty;
extern int discover_timeout_secs;
extern int discover_port;
#endif // __vismux_h_
