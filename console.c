#include "vismux.h"

static struct termios console_old_opts;
static int console_old_flags = -1;
static bool console_state_saved = false;

static void restore_console_state(void)
{
    if (!console_state_saved)
        return;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &console_old_opts);
    fcntl(STDIN_FILENO, F_SETFL, console_old_flags);
    console_state_saved = false;
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
    atexit(restore_console_state);
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
