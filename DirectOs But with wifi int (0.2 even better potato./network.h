#ifndef NETWORK_H
#define NETWORK_H

int network_init(void);
int network_ping(const unsigned char address[4], unsigned int *elapsed_ms);

#endif