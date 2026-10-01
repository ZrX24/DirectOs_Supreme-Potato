#ifndef TIMER_H
#define TIMER_H

void timer_init(void);
void timer_poll(void);
unsigned int timer_ticks(void);
unsigned int timer_seconds(void);

#endif
