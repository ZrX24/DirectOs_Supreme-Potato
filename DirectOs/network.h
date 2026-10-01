#ifndef NETWORK_H
#define NETWORK_H

int network_init(void);
int network_ping(const unsigned char address[4], unsigned short payload_size, unsigned int *elapsed_ms);
const char *network_driver_name(void);
void network_get_mac(unsigned char mac[6]);
void network_set_verbose(int enabled);

#endif