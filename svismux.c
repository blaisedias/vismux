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

char shm_path[128] = {0};
char global_mac[18] = {0};

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


int main(int argc, char *argv[])
{
    char *mac_input = NULL;
    bool disable_discovery_listener = false;
    bool daemonise = false;
    const char* logfile = NULL;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--no-discover") == 0) {
            disable_discovery_listener = true;
        } else if (strcmp(argv[i], "--wait-for-shm") == 0) {
            wait_for_shm = true;
        } else if (strcmp(argv[i], "--mac") == 0 && i + 1 < argc) {
            mac_input = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            target_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mac-timeout") == 0 && i + 1 < argc) {
            mac_timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            log_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--proto-version") == 0 && i + 1 < argc) {
            forced_proto_version = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stats-int") == 0 && i + 1 < argc) {
            stats_int = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--discover-port") == 0 && i + 1 < argc) {
            discover_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--daemonise") == 0) {
            daemonise = true;
        } else if ( (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--logfile") == 0)
                && i + 1 < argc) {
            ++i;
            logfile = argv[i];
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("vismux version %s\n", APP_VERSION);
            return 0; // Clean exit immediately
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf("Squeezelite Replicator v%s\nUsage Options:\n", APP_VERSION);
            printf(" %s  [--mac <mac_address>] [--wait-for-shm] [--port <p>] [--fps <f>] [--timeout <sec>] [--no-discover] [--discover-port <p>]\n", argv[0]);
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

    if (mac_input && !validate_and_format_mac(mac_input, shm_path, sizeof(shm_path)))
    {
        fprintf(stderr, "Invalid MAC parameter. %s\n", HELP_HINT);
        return 1;
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

    if (!mac_input && !resolve_source_shm())
    {
        return 1;
    }

    if (!mac_input) {
        mac_input = global_mac;
    }

    pthread_t* ui_thread = NULL;

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

    run_source(shm_path, mac_input, !disable_discovery_listener);

    join_thread(&ui_thread);

    return 0;
}
