#ifndef WC_GUEST_SERVICES_H
#define WC_GUEST_SERVICES_H

#include <stdint.h>

/* Offsets are relative to the engine's 32-bit memory. */
uint32_t wc_wasi_empty_sizes(uint32_t count, uint32_t size);
uint32_t wc_wasi_random_get(uint32_t data, uint32_t size);
uint32_t wc_wasi_clock_time(uint32_t id, uint32_t result);
uint32_t wc_wasi_clock_res(uint32_t id, uint32_t result);
uint32_t wc_wasi_fd_write(uint32_t fd, uint32_t vectors, uint32_t count, uint32_t result);
uint32_t wc_wasi_fd_read(uint32_t fd, uint32_t result);
uint32_t wc_wasi_fdstat(uint32_t fd, uint32_t result);
uint32_t wc_wasi_close(uint32_t fd);
uint32_t wc_wasi_seek(uint32_t fd);
uint32_t wc_wasi_yield(void);
int32_t wc_host_save(uint32_t name, uint32_t name_size, uint32_t data, uint32_t data_size,
                     uint32_t save_as, uint32_t output, uint32_t output_size);
void wc_guest_services_shutdown(void);

#endif
