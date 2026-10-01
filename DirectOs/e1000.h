#ifndef E1000_H
#define E1000_H

int e1000_init(void);
int e1000_send(const unsigned char *frame, unsigned short length);
int e1000_receive(unsigned char *frame, unsigned short capacity, unsigned short *length);
void e1000_get_mac(unsigned char mac[6]);

#endif