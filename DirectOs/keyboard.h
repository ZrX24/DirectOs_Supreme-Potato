#ifndef KEYBOARD_H
#define KEYBOARD_H

int keyboard_read_line(char *buffer, int capacity);
int keyboard_read_line_hidden(char *buffer, int capacity);
int keyboard_history_count(void);
const char *keyboard_history_get(int index);
int keyboard_wait_key(void);

#endif
