#ifndef RTL8139_H
#define RTL8139_H

int rtl8139_init(void);
int rtl8139_is_ready(void);
int rtl8139_send(const unsigned char *frame, unsigned short length);
int rtl8139_receive(unsigned char *frame, unsigned short capacity, unsigned short *length);
void rtl8139_get_mac(unsigned char mac[6]);

#endif