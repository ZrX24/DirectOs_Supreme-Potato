#include "timer.h"

#define PIT_RELOAD 11932U

static volatile unsigned int ticks;
static unsigned short previous_count;

static void port_out(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char port_in(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static unsigned short read_counter(void)
{
    unsigned char low;
    unsigned char high;
    port_out(0x43, 0);
    low = port_in(0x40);
    high = port_in(0x40);
    return (unsigned short)(low | ((unsigned short)high << 8));
}

void timer_init(void)
{
    port_out(0x43, 0x34);
    port_out(0x40, (unsigned char)(PIT_RELOAD & 0xFF));
    port_out(0x40, (unsigned char)(PIT_RELOAD >> 8));
    ticks = 0;
    previous_count = read_counter();
}

void timer_poll(void)
{
    unsigned short count = read_counter();
    if (count > previous_count) ticks++;
    previous_count = count;
}

unsigned int timer_ticks(void)
{
    timer_poll();
    return ticks;
}

unsigned int timer_seconds(void)
{
    return timer_ticks() / 100;
}
