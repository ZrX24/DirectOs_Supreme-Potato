#include "screen.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile unsigned short *const video = (unsigned short *)0xB8000;
static unsigned int cursor_x;
static unsigned int cursor_y;
static unsigned char current_color = 0x0F;

static unsigned short cell(char value, unsigned char color)
{
	return (unsigned short)value | ((unsigned short)color << 8);
}

static void update_cursor(void)
{
	unsigned short position = (unsigned short)(cursor_y * VGA_WIDTH + cursor_x);
	__asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x0F), "Nd"((unsigned short)0x3D4));
	__asm__ volatile ("outb %0, %1" : : "a"((unsigned char)(position & 0xFF)), "Nd"((unsigned short)0x3D5));
	__asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x0E), "Nd"((unsigned short)0x3D4));
	__asm__ volatile ("outb %0, %1" : : "a"((unsigned char)(position >> 8)), "Nd"((unsigned short)0x3D5));
}

static void scroll(void)
{
	unsigned int i;
	if (cursor_y < VGA_HEIGHT) return;
	for (i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) video[i] = video[i + VGA_WIDTH];
	for (i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) video[i] = cell(' ', current_color);
	cursor_y = VGA_HEIGHT - 1;
}

void screen_init(void)
{
	cursor_x = 0;
	cursor_y = 0;
	current_color = 0x0F;
	screen_clear();
}

void screen_clear(void)
{
	unsigned int i;
	for (i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) video[i] = cell(' ', current_color);
	cursor_x = 0;
	cursor_y = 0;
	update_cursor();
}

void screen_set_color(unsigned char color)
{
	current_color = color;
}

void screen_putchar(char value)
{
	if (value == '\n') {
		cursor_x = 0;
		cursor_y++;
	} else if (value == '\r') {
		cursor_x = 0;
	} else if (value == '\b') {
		if (cursor_x) cursor_x--;
		else if (cursor_y) { cursor_y--; cursor_x = VGA_WIDTH - 1; }
		video[cursor_y * VGA_WIDTH + cursor_x] = cell(' ', current_color);
	} else {
		video[cursor_y * VGA_WIDTH + cursor_x] = cell(value, current_color);
		cursor_x++;
		if (cursor_x == VGA_WIDTH) { cursor_x = 0; cursor_y++; }
	}
	scroll();
	update_cursor();
}

void screen_write(const char *text)
{
	if (!text) return;
	while (*text) screen_putchar(*text++);
}

void screen_write_color(const char *text, unsigned char color)
{
	unsigned char previous = current_color;
	screen_set_color(color);
	screen_write(text);
	screen_set_color(previous);
}

void screen_move_cursor(unsigned int x, unsigned int y)
{
	if (x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
	cursor_x = x;
	cursor_y = y;
	update_cursor();
}

unsigned int screen_row(void)
{
	return cursor_y;
}
