#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DMX_SLOTS 512

typedef enum {
    SRC_NONE   = 0,
    SRC_ARTNET = 1,
    SRC_SACN   = 2,
} dmx_src_type_t;

typedef struct {
    uint32_t artnet_packets;
    uint32_t sacn_packets;
    uint32_t frames_sent;
    uint8_t  active_type;    // dmx_src_type_t
    uint32_t active_ip;      // network byte order
    uint8_t  active_priority;
    uint8_t  num_sources;
    bool     signal;         // at least one live source
} dmx_stats_t;

void dmx_buffer_init(void);

// Feed received data. `id` identifies the source (sACN CID, or Art-Net IP padded to 16 bytes).
void dmx_buffer_submit(dmx_src_type_t type, const uint8_t id[16], uint32_t ip, uint8_t priority,
                       const uint8_t *data, uint16_t len);

// sACN stream_terminated: drop the source immediately.
void dmx_buffer_terminate(const uint8_t id[16]);

// Forget all sources (e.g. after a universe change).
void dmx_buffer_reset_sources(void);

// Called by the DMX transmitter: runs merge/expiry and copies the 512 output slots.
void dmx_buffer_get_output(uint8_t out[DMX_SLOTS]);

void dmx_buffer_count_frame(void);
void dmx_buffer_get_stats(dmx_stats_t *stats);
void dmx_buffer_peek(uint8_t *out, uint16_t n);   // first n output slots, for the web UI
