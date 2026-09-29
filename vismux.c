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

#define APP_VERSION "0.0.9m"
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

// Discovery Layer Configuration Constants
#define DISCOVER_PORT (DEFAULT_PORT + 1)
#define DISCOVER_MAGIC "VISMUXv0"
#define DISCOVER_VERSION_LEN 16
#define DEFAULT_MAC_TIMEOUT 2
#define SQUEEZELITE_SHM_PREFIX "squeezelite-"
#define SQUEEZELITE_SHM_PREFIX_LEN (sizeof(SQUEEZELITE_SHM_PREFIX) - 1)

#ifndef NOELF	// ELF support not included
#include <elf.h>

// Define a structured layout matching an official ELF note entry
struct elf_version_note {
    Elf64_Word namesz;   // Size of the vendor name string
    Elf64_Word descsz;   // Size of the description version string
    Elf64_Word type;     // Note type specifier
    char name[((sizeof(VENDOR_STR) + 3) & ~3)]; // Dynamically padded to 4-byte boundary
    char desc[((sizeof(APP_VERSION) + 3) & ~3)]; // Dynamically padded to 4-byte boundary
};

// Embed the version string explicitly inside the .note section allocation
__attribute__((used, section(".note.vismux.version"), aligned(4)))
static const struct elf_version_note app_version = {
    .namesz = sizeof(((struct elf_version_note *)0)->name),
    .descsz = sizeof(((struct elf_version_note *)0)->desc),
    .type = NT_VERSION,
    .name = "VISMUX",
    .desc = APP_VERSION
};
#endif	// NOELF

volatile sig_atomic_t keep_running = 1;
atomic_int log_level = 2;	// Atomic because it can be written to from console_listener_thread and read from other threads
atomic_int force_stats_log = 0;	// Atomic because it can be written to from console_listener_thread and read/reset from other threads
#define STATS_LOG_LEVEL 3
#define LOG_BUF_SIZE 1024	// Maximum length of single log message
#define TRUNC_TAG " ... [TRUNCATED]"
int port = DEFAULT_PORT;
int target_fps = DEFAULT_FPS;
int stats_int = DEFAULT_STATS_INTERVAL;
int timeout_secs = 2;
int mac_timeout_secs = DEFAULT_MAC_TIMEOUT;
int forced_proto_version = 2;
bool keep_shm = true;
char shm_path[128] = {0};
bool wait_for_shm = false;
bool has_interactive_tty = false;	// set in main

// Discovery Layer Runtime Flags
bool disable_discovery_listener = false;
int discover_timeout_secs = 2;
int discover_port = DISCOVER_PORT; // Overridable runtime discovery port descriptor

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

int global_shm_fd = -1;
vis_t *global_shm_ptr = MAP_FAILED;
int global_sock_fd = -1;
bool is_source_mode = false;
struct termios console_old_opts;
int console_old_flags = -1;
bool console_state_saved = false;

void restore_console_state(void)
{
    if (!console_state_saved)
        return;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &console_old_opts);
    fcntl(STDIN_FILENO, F_SETFL, console_old_flags);
    console_state_saved = false;
}

// Allocate pthread_t struct, and creates a thread
// Returns  pointer to pthread_t or  NULL if memory allocation or thread creation failed
static pthread_t* create_thread(const pthread_attr_t* attr,
                        void* (*start_routine)(void*),
                        void *arg) {
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
static void join_thread(pthread_t** ppt) {
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

void release_system_resources()
{
    if (global_shm_ptr != MAP_FAILED)
        munmap(global_shm_ptr, sizeof(vis_t));
    if (global_shm_fd != -1)
        close(global_shm_fd);
    if (!is_source_mode && !keep_shm && shm_path[0] != '\0')
    {
        shm_unlink(shm_path);
    }
    if (global_sock_fd != -1)
        close(global_sock_fd);
    restore_console_state();
    log_msg(1, "Engine offline.");
}

void handle_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "Got signal %d, terminating\n", sig);
    keep_running = 0;
}

void *console_listener_thread(void *arg)
{
    (void)arg;
    struct termios new_opts;
    if (tcgetattr(STDIN_FILENO, &console_old_opts) < 0)
        return NULL;
    console_old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (console_old_flags < 0)
        return NULL;
    console_state_saved = true;

    new_opts = console_old_opts;
    new_opts.c_lflag &= ~(ICANON | ECHO);
    new_opts.c_cc[VMIN] = 1;
    new_opts.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_opts);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (keep_running)
    {
        fd_set rfds;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int retval = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (retval > 0 && FD_ISSET(STDIN_FILENO, &rfds))
        {
            char ch = '\0';
            if (read(STDIN_FILENO, &ch, 1) > 0)
            {
                if (ch == 'q' || ch == 'Q')
                {
                    log_msg(1, "Instant terminal shutdown key captured ('%c'). Stop requested...", ch);
                    keep_running = 0;
                    break;
                }
                else if (ch == 'l' || ch == 'L')
                {
					int local_log_level = atomic_load(&log_level);
					local_log_level++;

					if (local_log_level > 3 || local_log_level < 0) {
						local_log_level = 0;
					}
					atomic_store(&log_level, local_log_level);
                    const char *level_names[] = {"0 (ERROR)", "1 (WARN)", "2 (INFO)", "3 (DEBUG)"};
                    log_msg(-1, "Console log level cycled dynamically to: %s", level_names[local_log_level]);
                }
                else if (ch == 's' || ch == 'S')
                {	// Request stats now
					atomic_store(&force_stats_log, 1);
                }
                else if (ch == 'v' || ch == 'V')
                {
                    log_msg(-1, "Application Version: %s", APP_VERSION);
                }
            }
        }
    }
    restore_console_state();
    return NULL;
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
    struct dirent *entry;
    while ((entry = readdir(shm_dir)) != NULL)
    {
        if (strncmp(entry->d_name, SQUEEZELITE_SHM_PREFIX, SQUEEZELITE_SHM_PREFIX_LEN) != 0)
            continue;

        char formatted_path[sizeof(shm_path)];
        if (!validate_and_format_mac(entry->d_name + SQUEEZELITE_SHM_PREFIX_LEN, formatted_path, sizeof(formatted_path)) ||
            strcmp(entry->d_name, formatted_path + 1) != 0)
            continue;

        if (match_count < MAX_DESTINATIONS)
            snprintf(matches[match_count], sizeof(matches[match_count]), "%s", formatted_path);
        match_count++;
    }
    closedir(shm_dir);

    if (match_count == 1)
    {
        snprintf(shm_path, sizeof(shm_path), "%s", matches[0]);
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

void init_destination_shm(vis_t *shm_ptr)
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&shm_ptr->rwlock, &attr);
    pthread_rwlockattr_destroy(&attr);

    shm_ptr->buf_size = VIS_BUF_SIZE;
    shm_ptr->buf_index = 0;
    shm_ptr->running = false;
    shm_ptr->rate = 44100;
    shm_ptr->updated = (uint64_t)time(NULL);
    memset(shm_ptr->buffer, 0, sizeof(shm_ptr->buffer));
}

bool setup_destination_shm(const char *path)
{
    global_shm_fd = shm_open(path, O_CREAT | O_RDWR, 0666);
    if (global_shm_fd == -1)
    {
        perror("SHM open error");
        return false;
    }

    // Check if the memory size has already been configured
    struct stat shm_stat;
    if (fstat(global_shm_fd, &shm_stat) == -1)
    {
        perror("SHM fstat error");
        close(global_shm_fd);
        global_shm_fd = -1;
        return false;
    }

    // Only ftruncate if the segment is brand new (size is 0) - because macOS does not support setting size on pre-existing SHM
	bool is_creator = (shm_stat.st_size == 0);
    if (is_creator)
    {
        if (ftruncate(global_shm_fd, sizeof(vis_t)) == -1)
        {
            perror("SHM truncate error");
            close(global_shm_fd);
            global_shm_fd = -1;
            return false;
        }
    }

    global_shm_ptr = (vis_t *)mmap(0, sizeof(vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, global_shm_fd, 0);
    if (global_shm_ptr == MAP_FAILED)
    {
        perror("SHM mapping error");
        close(global_shm_fd);
        global_shm_fd = -1;
        return false;
    }

    // Initialise if we created it - with risk of corrupted data from earlier run being present
	// or fighting with some other application (Squezelite is obvious candidate) - expect issues if local Squeezelite using same memory
	if (is_creator)
    {
        init_destination_shm(global_shm_ptr);
    }
    
    return true;
}

void run_source()
{
    is_source_mode = true;
    global_shm_fd = shm_open(shm_path, O_RDWR, 0666);
    if (global_shm_fd == -1)
    {
        log_msg(0, "Failed to open SHM %s. Is Squeezelite running with visualiser enabled (-v)?", shm_path);
        exit(EXIT_FAILURE);
    }
    global_shm_ptr = (vis_t *)mmap(0, sizeof(vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, global_shm_fd, 0);
    global_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(global_sock_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (bind(global_sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)))
    {
        log_msg(1, "port %d is already in use! ", (int)server_addr.sin_port);
        release_system_resources();
        exit(EXIT_FAILURE);
    }

    destination_t clients[MAX_DESTINATIONS];
    memset(clients, 0, sizeof(clients));
    log_msg(2, "Source Engine Online (%s) tracking SHM: %s", APP_VERSION, shm_path);

    char source_mac[18] = {0};
    char *source_mac_ptr = strchr(shm_path, '-');
    if (source_mac_ptr)
        snprintf(source_mac, sizeof(source_mac), "%s", source_mac_ptr + 1);

    uint32_t seq_counter = 0;
    useconds_t sleep_interval = 1000000 / target_fps;
    char net_buf[sizeof(msg_hdr_t) + sizeof(vis_t)];

    while (keep_running)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        msg_hdr_t incoming_hdr;

        while (recvfrom(global_sock_fd, &incoming_hdr, sizeof(msg_hdr_t), 0, (struct sockaddr *)&client_addr, &addr_len) > 0)
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
                    sendto(global_sock_fd, response_buf, sizeof(response_buf), 0,
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
            pthread_rwlock_rdlock(&global_shm_ptr->rwlock);
            current_hdr.buf_size = htonl(global_shm_ptr->buf_size);
            current_hdr.buf_index = htonl(global_shm_ptr->buf_index);
            uint32_t current_idx = global_shm_ptr->buf_index;
            current_hdr.running = htonl((int)global_shm_ptr->running);
            bool current_running = (int)global_shm_ptr->running;
            current_hdr.rate = htonl(global_shm_ptr->rate);
            memcpy(current_buffer, global_shm_ptr->buffer, sizeof(current_buffer));
            time_t current_update_time = global_shm_ptr->updated;
            pthread_rwlock_unlock(&global_shm_ptr->rwlock);

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
                        payload_audio_bytes = sizeof(global_shm_ptr->buffer);
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
                            payload_audio_bytes = sizeof(global_shm_ptr->buffer);
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
                    sendto(global_sock_fd, net_buf, sizeof(msg_hdr_t) + total_payload_len, 0, (struct sockaddr *)&clients[i].addr, sizeof(struct sockaddr_in));
                }
            }
        }
        usleep(sleep_interval);
    }
}

void *heartbeat_loop(void *arg)
{
    hb_ctx_t *ctx = (hb_ctx_t *)arg;
    msg_hdr_t req_hdr = {.protocol_version = forced_proto_version, .type = PACKET_REQ, .sequence = 0, .payload_len = 0};

    while (keep_running)
    {
        // Dynamically read the active socket pointer directly out of the context wrapper
        int current_fd = *(ctx->sock_fd_ptr);
        if (current_fd >= 0)
        {
            sendto(current_fd, &req_hdr, sizeof(msg_hdr_t), 0, (struct sockaddr *)&ctx->server_addr, sizeof(ctx->server_addr));
        }
        for (int i = 0; i < 10 && keep_running; i++)
            usleep(100000);
    }
    free(ctx);
    return NULL;
}

void run_destination(const char *server_ip)
{
    is_source_mode = false;
    bool shm_ready = shm_path[0] != '\0';
    if (shm_ready && !setup_destination_shm(shm_path))
        exit(EXIT_FAILURE);

    log_msg(2, "Destination Engine Online (%s) expecting data from: %s:%d", APP_VERSION, server_ip, port);

    global_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    struct sockaddr_in local_bound_addr;
    socklen_t local_bound_len = sizeof(local_bound_addr);
    struct sockaddr_in any_addr = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY};
    if (bind(global_sock_fd, (struct sockaddr *)&any_addr, sizeof(any_addr)))
    {
        log_msg(0, "bind to ephemeral port failed (1)");
        release_system_resources();
        exit(EXIT_FAILURE);
    }
    if (getsockname(global_sock_fd, (struct sockaddr *)&local_bound_addr, &local_bound_len) == 0)
        log_msg(3, "Outbound ephemeral port: %d", ntohs(local_bound_addr.sin_port));

    hb_ctx_t *hb_ctx = (hb_ctx_t *)malloc(sizeof(hb_ctx_t));
    hb_ctx->sock_fd_ptr = &global_sock_fd;
    hb_ctx->server_addr = server_addr;
    pthread_t* hb_thread = create_thread(NULL, heartbeat_loop, hb_ctx);

    char rx_window[sizeof(msg_hdr_t) + sizeof(vis_t)];
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(global_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

    while (keep_running)
    {
        time_t now_check = time(NULL);
        ssize_t bytes_in = recvfrom(global_sock_fd, rx_window, sizeof(rx_window), 0, NULL, NULL);
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
                            log_msg(3, "No valid MAC received from source. Rotating ports...");
                        else
                            log_msg(1, "Source pipeline stopped. Rotating ports...");
                        was_connection_logged_down = true;
                    }
                    last_rotation_time = now_check;
                    close(global_sock_fd);
                    global_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
                    if (global_sock_fd >= 0)
                    {
                        if (bind(global_sock_fd, (struct sockaddr *)&any_addr, sizeof(any_addr)))
                        {
                            log_msg(0, "bind to ephemeral port failed (2)");
                            release_system_resources();
                            exit(EXIT_FAILURE);
                        }
                        setsockopt(global_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        local_bound_len = sizeof(local_bound_addr);
                        if (getsockname(global_sock_fd, (struct sockaddr *)&local_bound_addr, &local_bound_len) == 0)
                            log_msg(3, "Recovery port switch: %d", ntohs(local_bound_addr.sin_port));

                        first_frame = true;
                        last_network_seq = 0;

                        sendto(global_sock_fd, &instant_req, sizeof(msg_hdr_t), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
                    }
                    last_mac_response_time = now_check;
                    last_success_packet_time = now_check;
                }
                continue;
            }
            if (errno == ECONNREFUSED)
            {
                close(global_sock_fd);
                global_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (global_sock_fd >= 0)
                {
                    if (bind(global_sock_fd, (struct sockaddr *)&any_addr, sizeof(any_addr)))
                    {
                        log_msg(0, "bind to ephemeral port failed (3)");
                        release_system_resources();
                        exit(EXIT_FAILURE);
                    }
                    setsockopt(global_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                    // Re-inject instant request frame on connection refusal recovery triggers
                    sendto(global_sock_fd, &instant_req, sizeof(msg_hdr_t), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
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
                    char negotiated_shm_path[sizeof(shm_path)];
                    if (validate_and_format_mac(response->mac, negotiated_shm_path, sizeof(negotiated_shm_path)))
                    {
                        snprintf(shm_path, sizeof(shm_path), "%s", negotiated_shm_path);
                        if (setup_destination_shm(shm_path))
                        {
                            shm_ready = true;
                            first_frame = true;
                            last_network_seq = 0;
                            log_msg(2, "Destination SHM initialized from source MAC: %s", response->mac);
                        }
                        else
                        {
                            shm_path[0] = '\0';
                        }
                    }
                    else
                    {
                        log_msg(1, "Source subscription response contained an invalid MAC address.");
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
                log_msg(1, "Connection re-established.");
                was_connection_logged_down = false;
            }
            total_received_frames++;
            total_received_bytes += msg->payload_len;
            bool is_full_frame = msg->payload_len >= wire_headers_sz &&
                                 msg->payload_len - wire_headers_sz == sizeof(global_shm_ptr->buffer);
            if (is_full_frame)
                total_full_frames++;

			int current_level = STATS_LOG_LEVEL;

			if ( has_interactive_tty && atomic_load(&force_stats_log) != 0 )
			{
				current_level = -1;	// NOTIFY
			}

            time_t current_time = time(NULL);
            if (first_frame || current_time - last_stats_log_time >= stats_int || current_level != STATS_LOG_LEVEL)
            {
				
				
                log_msg(current_level, "Processing updates: [Total frames: captured: %" PRIu64 ", dropped: %" PRIu64 ", full: %" PRIu64 "] [Total Data: %.2f MB]",
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

            if (msg->payload_len <= wire_headers_sz || msg->payload_len > (wire_headers_sz + sizeof(global_shm_ptr->buffer)) || (size_t)bytes_in < (sizeof(msg_hdr_t) + msg->payload_len))
                continue;

            pthread_rwlock_wrlock(&global_shm_ptr->rwlock);
            char *incoming_audio_payload = rx_window + sizeof(msg_hdr_t) + wire_headers_sz;
            size_t received_audio_bytes = msg->payload_len - wire_headers_sz;
            if (received_audio_bytes == sizeof(global_shm_ptr->buffer))
            {
                memcpy(global_shm_ptr->buffer, incoming_audio_payload, received_audio_bytes);
            }
            else
            {
                uint32_t incoming_samples = received_audio_bytes / sizeof(int16_t);
                for (uint32_t i = 0; i < incoming_samples; i++)
                {
                    uint32_t target_idx = (global_shm_ptr->buf_index - incoming_samples + i + VIS_BUF_SIZE) % VIS_BUF_SIZE;
                    global_shm_ptr->buffer[target_idx] = ((int16_t *)incoming_audio_payload)[i];
                }
            }

            vis_wire_hdr_t* wirehdr = (vis_wire_hdr_t*)(rx_window + sizeof(msg_hdr_t));
            global_shm_ptr->buf_size = ntohl(wirehdr->buf_size);
            global_shm_ptr->buf_index = ntohl(wirehdr->buf_index);
            global_shm_ptr->running = (bool)ntohl(wirehdr->running);
            global_shm_ptr->rate = ntohl(wirehdr->rate);
            global_shm_ptr->updated = (uint64_t)time(NULL);

            first_frame = false;
            pthread_rwlock_unlock(&global_shm_ptr->rwlock);
            msg_hdr_t ack_hdr = {.protocol_version = msg->protocol_version, .type = PACKET_ACK, .sequence = msg->sequence, .payload_len = 0};
            sendto(global_sock_fd, &ack_hdr, sizeof(msg_hdr_t), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
        }
    }
    join_thread(&hb_thread);
}

void *discovery_responder_thread(void *arg)
{
    int role_id = *(int *)arg;
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

    while (keep_running)
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
                    tx_packet.role = (uint8_t)role_id;
                    tx_packet.port = htonl((uint32_t)port);
                    snprintf(tx_packet.version, sizeof(tx_packet.version), "%s", APP_VERSION);

                    // Extract clean mac address from global path allocations
                    char *mac_ptr = strchr(shm_path, '-');
                    if (mac_ptr)
                        snprintf(tx_packet.mac, sizeof(tx_packet.mac), "%s", mac_ptr + 1);
                    else
                        snprintf(tx_packet.mac, sizeof(tx_packet.mac), "00:00:00:00:00:00");

                    char target_ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, target_ip_str, INET_ADDRSTRLEN);

                    // Debug log output tracing the dynamic network reply dispatch
                    log_msg(3, "Dispatching discovery response packet back to prober host: %s:%d",
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

void run_discovery_prober()
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

    // Define a tracking record layout to deduplicate the entire dataset combined
    typedef struct
    {
        uint32_t ip;
        uint32_t port;
        char mac[18]; // Storage footprint to match clean "XX:XX:XX:XX:XX:XX\0" length bounds
    } peer_record_t;

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
                    snprintf(seen_records[seen_count].mac, sizeof(seen_records[seen_count].mac), "%s", resp.mac);
                    seen_count++;
                }

                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, INET_ADDRSTRLEN);

                  printf("%s,%s,%u,%s,%s\n",
                       (resp.role == 1) ? "SOURCE" : "DESTINATION",
                      ip_str, response_port, resp.mac, response_version);
                fflush(stdout);
            }
        }
    }
    close(probe_fd);
}

int main(int argc, char *argv[])
{
    bool is_source = false, is_dest = false, is_discover = false;
    char *server_ip = NULL, *mac_input = NULL;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--source") == 0)
            is_source = true;
        else if (strcmp(argv[i], "--destination") == 0)
            is_dest = true;
        else if (strcmp(argv[i], "--discover") == 0)
            is_discover = true;
        else if (strcmp(argv[i], "--no-discover") == 0)
            disable_discovery_listener = true;
        else if (strcmp(argv[i], "--wait-for-shm") == 0)
            wait_for_shm = true;
        else if (strcmp(argv[i], "--discover-timeout") == 0 && i + 1 < argc)
            discover_timeout_secs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--discover-port") == 0 && i + 1 < argc)
            discover_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server_ip = argv[++i];
        else if (strcmp(argv[i], "--mac") == 0 && i + 1 < argc)
            mac_input = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            target_fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_secs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--mac-timeout") == 0 && i + 1 < argc)
            mac_timeout_secs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            log_level = atoi(argv[++i]);
        else if (strcmp(argv[i], "--proto-version") == 0 && i + 1 < argc)
            forced_proto_version = atoi(argv[++i]);
        else if (strcmp(argv[i], "--remove-shm") == 0)
            keep_shm = false;
        else if (strcmp(argv[i], "--stats-int") == 0 && i + 1 < argc)
            stats_int = atoi(argv[++i]);
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
        {
            printf("vismux version %s\n", APP_VERSION);
            return 0; // Clean exit immediately
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf("Squeezelite Replicator v%s\nUsage Options:\n", APP_VERSION);
            printf("  Source Mode:      %s --source [--mac <mac_address>] [--wait-for-shm] [--port <p>] [--fps <f>] [--timeout <sec>] [--no-discover] [--discover-port <p>]\n", argv[0]);
            printf("  Destination Mode: %s --destination --server <source_ip> [--mac <mac_address>] [--mac-timeout <sec>] [--port <p>] [--proto-version <1|2>] [--remove-shm] [--stats-int <interval_secs>]\n\n", argv[0]);
            printf("  Discovery Mode:   %s --discover [--discover-timeout <sec>] [--discover-port <p>\n\n", argv[0]);
            printf("Global Flags:\n");
			printf("  -h, --help        Display this help message\n");
            printf("  -v, --version     Display application version details\n");
            printf("  --log-level <0-3> Filter verbosity (0=ERR, 1=WARN, 2=INFO, 3=DBG)\n\n");
            printf("Interactive Controls (does not require Enter):\n");
            printf("  Press 'v'         Version - Display application version details\n");
            printf("  Press 'q'         Quit - Request shutdown\n");
            printf("  Press 'l'         Log Level - Cycle log levels dynamically (0=ERROR -> 1=WARN -> 2=INFO -> 3=DEBUG)\n");
			printf("  Press 's'         Stats - Request stats summary on next data reception\n");
            return 0;
        }
        else
        {
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

    if (is_discover)
    {
        run_discovery_prober();
        return 0; // Turnkey exit immediately when the prober pass wraps up
    }

    if (is_source && mac_input && !validate_and_format_mac(mac_input, shm_path, sizeof(shm_path)))
    {
        fprintf(stderr, "Invalid MAC parameter. %s\n", HELP_HINT);
        return 1;
    }
    if (is_dest && mac_input && !validate_and_format_mac(mac_input, shm_path, sizeof(shm_path)))
    {
        fprintf(stderr, "Invalid MAC parameter. %s\n", HELP_HINT);
        return 1;
    }

    if (is_dest && !server_ip)
    {
        fprintf(stderr, "Destination mode requires --server <source_ip>. %s\n", HELP_HINT);
        return 1;
    }
    if (is_dest)
    {
        struct in_addr server_addr;
        if (inet_pton(AF_INET, server_ip, &server_addr) != 1)
        {
            fprintf(stderr, "Invalid server IP address: %s. %s\n", server_ip, HELP_HINT);
            return 1;
        }
    }

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

    pthread_t* ui_thread=NULL;
    pthread_t* disc_thread=NULL;
    has_interactive_tty = isatty(STDIN_FILENO);
    if (has_interactive_tty)
        ui_thread = create_thread(NULL, console_listener_thread, NULL);

    if (is_source && !mac_input && !resolve_source_shm())
    {
        //if (has_interactive_tty)
        //    pthread_join(ui_thread, NULL);
        release_system_resources();
        return 1;
    }

    // Dynamic birth pass configuration for background responders
    if (!disable_discovery_listener)
    {
        int *role_payload = (int *)malloc(sizeof(int));
        *role_payload = is_source ? 1 : 2;
        disc_thread = create_thread(NULL, discovery_responder_thread, role_payload);
    }

    if (is_source)
    {
        run_source();
    }
    else if (is_dest && server_ip)
    {
        run_destination(server_ip);
    }

    join_thread(&disc_thread);
    join_thread(&ui_thread);

    release_system_resources();
    return 0;
}
