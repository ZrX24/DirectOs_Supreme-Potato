#include "keyboard.h"
#include "screen.h"
#include "timer.h"

#define HISTORY_SIZE 8
#define LINE_MAX 128
#define KEY_UP 0x100
#define KEY_DOWN 0x101

static char history[HISTORY_SIZE][LINE_MAX];
static int history_count;
static int history_next;
static int shift_down;
static int caps_lock;

static unsigned char port_in(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int read_key_event(void)
{
    static const char normal[58] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    static const char shifted[58] = {
        0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, '\t',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };
    unsigned char code;
    int extended = 0;
    for (;;) {
        while (!(port_in(0x64) & 1)) timer_poll();
        code = port_in(0x60);
        if (code == 0xE0) { extended = 1; continue; }
        if (extended) {
            extended = 0;
            if (code & 0x80) continue;
            if (code == 0x48) return KEY_UP;
            if (code == 0x50) return KEY_DOWN;
            if (code == 0x1C) return '\n';
            if (code == 0x53) return 127;
            continue;
        }
        if (code == 0x2A || code == 0x36) { shift_down = 1; continue; }
        if (code == 0xAA || code == 0xB6) { shift_down = 0; continue; }
        if (code == 0x3A) { caps_lock = !caps_lock; continue; }
        if (code & 0x80) continue;
        if (code >= sizeof(normal)) continue;
        if (code == 0x1C) return '\n';
        if (!normal[code]) continue;
        if (normal[code] >= 'a' && normal[code] <= 'z') {
            return (shift_down != caps_lock) ? shifted[code] : normal[code];
        }
        return shift_down ? shifted[code] : normal[code];
    }
}

static void erase_line(char *buffer, int *length)
{
    while (*length > 0) {
        screen_putchar('\b');
        (*length)--;
    }
    buffer[0] = 0;
}

static int keyboard_read_line_internal(char *buffer, int capacity, int echo, int remember)
{
    int length = 0;
    int browse = -1;
    int key;
    if (!buffer || capacity < 2) return 0;
    if (capacity > LINE_MAX) capacity = LINE_MAX;
    buffer[0] = 0;
    for (;;) {
        key = read_key_event();
        if (key == '\n') {
            screen_putchar('\n');
            break;
        }
        if (key == 8 || key == 127) {
            if (length) {
                length--;
                buffer[length] = 0;
                if (echo) screen_putchar('\b');
            }
            browse = -1;
            continue;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            int count = keyboard_history_count();
            if (!echo || !remember || !count) continue;
            if (key == KEY_UP && browse < count - 1) browse++;
            if (key == KEY_DOWN && browse >= 0) browse--;
            erase_line(buffer, &length);
            if (browse >= 0) {
                const char *saved = keyboard_history_get(count - 1 - browse);
                while (*saved && length + 1 < capacity) {
                    buffer[length++] = *saved++;
                    screen_putchar(buffer[length - 1]);
                }
                buffer[length] = 0;
            }
            continue;
        }
        if (key >= 32 && key <= 126 && length + 1 < capacity) {
            buffer[length++] = (char)key;
            buffer[length] = 0;
            if (echo) screen_putchar((char)key);
        }
    }
    if (remember && length) {
        int i;
        int duplicate = 0;
        for (i = 0; i < length && history[history_next][i] == buffer[i]; i++) { }
        if (i == length && history[history_next][i] == 0) duplicate = 1;
        if (!duplicate) {
            int j;
            for (j = 0; j < capacity && j < LINE_MAX; j++) {
                history[history_next][j] = buffer[j];
                if (!buffer[j]) break;
            }
            history_next = (history_next + 1) % HISTORY_SIZE;
            if (history_count < HISTORY_SIZE) history_count++;
        }
    }
    return length;
}

int keyboard_read_line(char *buffer, int capacity)
{
    return keyboard_read_line_internal(buffer, capacity, 1, 1);
}

int keyboard_read_line_hidden(char *buffer, int capacity)
{
    return keyboard_read_line_internal(buffer, capacity, 0, 0);
}

int keyboard_history_count(void)
{
    return history_count;
}

const char *keyboard_history_get(int index)
{
    int oldest;
    if (index < 0 || index >= history_count) return "";
    oldest = (history_next - history_count + HISTORY_SIZE) % HISTORY_SIZE;
    return history[(oldest + index) % HISTORY_SIZE];
}

int keyboard_wait_key(void)
{
    return read_key_event();
}
