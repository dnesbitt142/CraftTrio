#ifndef WC_NETWORK_H
#define WC_NETWORK_H

#include <stdint.h>

int wc_network_ready(void);
void wc_network_shutdown(void);

int32_t wc_net_socket(uint32_t operation, uint32_t address, uint32_t length);
void wc_net_open_url(uint32_t address, uint32_t length);
int32_t wc_net_release(uint32_t operation, uint32_t address, uint32_t length);

#endif
