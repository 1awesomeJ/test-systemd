#include <systemd/sd-journal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <errno.h>

#define ANSI_HOME_CLEAR "\x1B[H\x1B[2J"
#define STRLEN(x) (sizeof(x) - 1)
#define ANSI_BRIGHT_BLUE_BACKGROUND "\x1B[44m"

#define IS_SIGNED_INTEGER_TYPE(type) \
        (__builtin_types_compatible_p(typeof(type), signed char) ||   \
         __builtin_types_compatible_p(typeof(type), signed short) ||  \
         __builtin_types_compatible_p(typeof(type), signed) ||        \
         __builtin_types_compatible_p(typeof(type), signed long) ||   \
         __builtin_types_compatible_p(typeof(type), signed long long))


#define DECIMAL_STR_MAX(type)                                           \
        ((size_t) IS_SIGNED_INTEGER_TYPE(type) + 1U +                   \
            (sizeof(type) <= 1 ? 3U :                                   \
             sizeof(type) <= 2 ? 5U :                                   \
             sizeof(type) <= 4 ? 10U :                                  \
             sizeof(type) <= 8 ? (IS_SIGNED_INTEGER_TYPE(type) ? 19U : 20U) : sizeof(int[-2*(sizeof(type) > 8)])))

static inline void *memcpy_safe(void *dst, const void *src, size_t n) {
        if (n == 0)
                return dst;
        return memcpy(dst, src, n);
}

void* memdup_suffix0(const void *p, size_t l) {
        void *ret;

        ret = malloc(l + 1);
        if (!ret)
                return NULL;

        ((uint8_t*) ret)[l] = 0;
        return memcpy_safe(ret, p, l);
}

int set_cursor_row(int fd, int row) {
    char cursor_position[STRLEN("\x1B[") + DECIMAL_STR_MAX(int) * 2 + STRLEN("H") + 1];
    sprintf(cursor_position, "\x1B[%dH", row);

    int r = write(fd, cursor_position, strlen(cursor_position));
    if (r < 0) {
        printf("Failed to set cursor position: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int acquire_first_emergency_log_message(char **ret_message, char **ret_message_id, bool is_wait) {
    sd_journal *j = NULL;
    size_t l;
    const void *d;
    sd_id128_t boot_id;
    char boot_id_filter[44];
    int r;

    r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
    if (r < 0) {
        printf("Failed to open journal\n");
        return r;
    }

    r = sd_id128_get_boot(&boot_id);
    if (r < 0) {
        printf("Failed to get boot ID\n");
        sd_journal_close(j);
        return r;
    }

    snprintf(boot_id_filter, sizeof(boot_id_filter), "_BOOT_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(boot_id));

    r = sd_journal_add_match(j, boot_id_filter, 0);
    if (r < 0) {
        printf("Failed to add boot ID filter\n");
        sd_journal_close(j);
        return r;
    }

    r = sd_journal_add_match(j, "_UID=0", 0);
    if (r < 0) {
        printf("Failed to add User ID filter\n");
        sd_journal_close(j);
        return r;
    }

    r = sd_journal_add_match(j, "PRIORITY=0", 0);
    if (r < 0) {
        printf("Failed to add Emergency filter\n");
        sd_journal_close(j);
        return r;
    }

    r = sd_journal_seek_head(j);
    if (r < 0) {
        printf("Failed to seek to start of journal\n");
        sd_journal_close(j);
        return r;
    }
    
    for(;;) {
        r = sd_journal_next(j);
            if (r < 0) {
                printf("Failed to read next journal entry\n");
                sd_journal_close(j);
                return r;
            } 

            if (r == 0) {
                if (is_wait) {
                    r = sd_journal_wait(j, (uint64_t) -1);
                    if (r < 0) {
                        printf("Failed to read next journal entry\n");
                        sd_journal_close(j);
                        return r;
                    }
                    continue;
                }
                
                printf("No entries in the journal\n");
                sd_journal_close(j);
                return 0;
            }
        break;
    }

    r = sd_journal_get_data(j, "MESSAGE", &d, &l);
    if (r < 0) {
        printf("Failed to read journal message\n");
        sd_journal_close(j);
        return r;
    }

    *ret_message = memdup_suffix0((const char*)d + strlen("MESSAGE="), l - strlen("MESSAGE="));
    if (!*ret_message) {
        printf("Out of memory\n");
        sd_journal_close(j);
        return -1;
    }

    r = sd_journal_get_data(j, "MESSAGE_ID", &d, &l);
    if (r < 0)
        printf("Failed to read journal message id\n");

    if (r > 0) {
        *ret_message_id = memdup_suffix0((const char*)d + strlen("MESSAGE_ID="), l - strlen("MESSAGE_ID="));
        if (!*ret_message_id) {
                printf("Out of memory\n");
                sd_journal_close(j);
                return -1;
         }
     }   

    sd_journal_close(j);
    return 0;
}

static int find_next_free_vt(int fd, int *free_vt, int *original_vt) {
        /*assert(free_vt);
        assert(original_vt);*/

        size_t i;
        struct vt_stat terminal_status;

        if (ioctl(fd, VT_GETSTATE, &terminal_status) < 0)
                return -errno;

        for (i = 0; i < sizeof(terminal_status.v_state) * 8; i++) {
                if ((terminal_status.v_state & (1 << i)) == 0)
                        break;
        }

        *free_vt = i;
        *original_vt = terminal_status.v_active;

        return 0;
}

static int display_emergency_message_fullscreen(char *message, char *message_id) {
        int r, free_vt = -1, original_vt = -1;
        char tty[STRLEN("/dev/tty") + DECIMAL_STR_MAX(int) + 1];
        int fd = -1;
        FILE *stream = NULL;

        fd = open("/dev/tty1", O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0) {
                printf("Failed to open tty1: %s\n", strerror(errno));
                return -1;
        }

        r = find_next_free_vt(fd, &free_vt, &original_vt);
        if (r < 0) {
                printf("Failed to find a free VT: %s\n", strerror(-r));
                close(fd);
                return r;
        }
      
        close(fd);

        sprintf(tty, "/dev/tty%d", free_vt + 1);

        fd = open(tty, O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0) {
                printf("Failed to open tty: %s\n", strerror(errno));
                return -1;
        }

        if (ioctl(fd, VT_ACTIVATE, free_vt + 1) < 0) {
                printf("Failed to activate tty: %s\n", strerror(errno));
                close(fd);
                return -1;
        }
        struct winsize w;
        ioctl(fd, TIOCGWINSZ, &w);

        int mid_row = w.ws_row / 2;

        r = write(fd, ANSI_BRIGHT_BLUE_BACKGROUND, strlen(ANSI_BRIGHT_BLUE_BACKGROUND));
        if (r < 0)
                printf("Failed to set terminal background colour t blue, ignoring: %s\n", strerror(errno));

        r = write(fd, ANSI_HOME_CLEAR, strlen(ANSI_HOME_CLEAR));
        if (r < 0)
                printf("Failed to clear terminal, ignoring: %s\n", strerror(errno));

        r = set_cursor_row(fd, mid_row);
        if (r < 0) {
                close(fd);
                return -1;
        }

        r = write(fd, message, strlen(message));
        if (r < 0) {
                printf("Failed to write emergency message to terminal: %s\n", strerror(errno));
                close(fd);
                return -1;
        }

        r = write(fd, message_id, strlen(message_id));
        if (r < 0) {
                printf("Failed to write emergency message_id to terminal: %s\n", strerror(errno));
                close(fd);
                return -1;
        }

        stream = fdopen(fd, "r");
        if (stream == NULL) {
                printf("Failed to open stream: %s\n", strerror(errno));
                close(fd);
                return -1;
        }

        r = fgetc(stream);
        if (r == EOF)
                printf("Failled to read character\n");

        if (ioctl(fd, VT_ACTIVATE, original_vt) < 0) {
                printf("Failed to switch back to original VT: %s\n", strerror(errno));
                close(fd);
                fclose(stream);
                return -1;
        }
        fclose(stream);
        return 0;
}

int main(void) {
        char *message = NULL, *message_id = NULL;
        int r;

        r = acquire_first_emergency_log_message(&message, &message_id, true);
        if (r < 0) {
                printf("Failed to acquire emergency log message\n");
                return -1;
        }
        printf("THIS IS THE LOG MESSAGE: %s\n", message);
        if(!message_id)
                message_id = (char*) "0000";
        printf("THIS IS THE MESSAGE_ID: %s\n", message_id);
        r = display_emergency_message_fullscreen(message, message_id);
        if (r < 0)
                printf("Failed to display emergency message on terminal:");

    free(message);
    free(message_id);
    return r;
}
