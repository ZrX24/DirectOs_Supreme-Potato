#ifndef SCREEN_H
#define SCREEN_H

void screen_init(void);
void screen_clear(void);
void screen_set_color(unsigned char color);
void screen_putchar(char value);
void screen_write(const char *text);
void screen_write_color(const char *text, unsigned char color);
void screen_move_cursor(unsigned int x, unsigned int y);
unsigned int screen_row(void);

#endif
