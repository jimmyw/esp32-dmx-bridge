#include "sacn.h"

#include <string.h>
#include "config.h"
#include "dmx_buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "sacn";

static const uint8_t ACN_ID[12] = { 'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0 };

#define VECTOR_ROOT_E131_DATA     0x00000004
#define VECTOR_E131_DATA_PACKET   0x00000002
#define VECTOR_DMP_SET_PROPERTY   0x02
#define OPT_PREVIEW               0x80
#define OPT_TERMINATED            0x40
#define SACN_MIN_LEN              126

#define MAX_SEQ_SOURCES 4

typedef struct {
    bool    used;
    uint8_t cid[16];
    uint8_t last_seq;
} seq_entry_t;

static int s_sock = -1;
static volatile bool s_rejoin = true;
static uint16_t s_joined_universe;   // 0 = not joined
static seq_entry_t s_seq[MAX_SEQ_SOURCES];
static uint8_t s_seq_next;

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

void sacn_rejoin(void)
{
    s_rejoin = true;
}

static struct ip_mreq make_mreq(uint16_t universe)
{
    struct ip_mreq m = { 0 };
    // 239.255.<hi>.<lo>
    m.imr_multiaddr.s_addr = htonl(0xEFFF0000u | universe);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    return m;
}

static void update_membership(void)
{
    s_rejoin = false;
    if (s_joined_universe) {
        struct ip_mreq m = make_mreq(s_joined_universe);
        setsockopt(s_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m));
        s_joined_universe = 0;
    }
    if (!(g_config.protocol & PROTO_SACN)) {
        return;
    }
    uint16_t uni = g_config.sacn_universe;
    struct ip_mreq m = make_mreq(uni);
    if (setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0) {
        s_joined_universe = uni;
        ESP_LOGI(TAG, "joined 239.255.%u.%u (universe %u)", uni >> 8, uni & 0xFF, uni);
    } else {
        // Usually means no interface is up yet; retried on next got-IP event.
        ESP_LOGW(TAG, "multicast join for universe %u failed: %d", uni, errno);
    }
}

// Returns false if the packet is out of order (E1.31 section 6.7.2).
static bool sequence_ok(const uint8_t cid[16], uint8_t seq)
{
    for (int i = 0; i < MAX_SEQ_SOURCES; i++) {
        if (s_seq[i].used && memcmp(s_seq[i].cid, cid, 16) == 0) {
            int8_t diff = (int8_t)(seq - s_seq[i].last_seq);
            if (diff <= 0 && diff > -20) {
                return false;
            }
            s_seq[i].last_seq = seq;
            return true;
        }
    }
    seq_entry_t *e = &s_seq[s_seq_next];
    s_seq_next = (s_seq_next + 1) % MAX_SEQ_SOURCES;
    e->used = true;
    memcpy(e->cid, cid, 16);
    e->last_seq = seq;
    return true;
}

static void handle_packet(const uint8_t *p, int len, const struct sockaddr_in *from)
{
    if (!(g_config.protocol & PROTO_SACN) || len < SACN_MIN_LEN) {
        return;
    }
    // Root layer
    if (be16(&p[0]) != 0x0010 || memcmp(&p[4], ACN_ID, sizeof(ACN_ID)) != 0 ||
        be32(&p[18]) != VECTOR_ROOT_E131_DATA) {
        return;   // also filters universe-discovery and sync packets
    }
    const uint8_t *cid = &p[22];
    // Framing layer
    if (be32(&p[40]) != VECTOR_E131_DATA_PACKET) {
        return;
    }
    uint8_t priority = p[108];
    uint8_t seq      = p[111];
    uint8_t options  = p[112];
    uint16_t universe = be16(&p[113]);
    if (universe != g_config.sacn_universe) {
        return;
    }
    if (options & OPT_TERMINATED) {
        dmx_buffer_terminate(cid);
        return;
    }
    if (options & OPT_PREVIEW) {
        return;
    }
    // DMP layer
    if (p[117] != VECTOR_DMP_SET_PROPERTY || p[118] != 0xA1) {
        return;
    }
    uint16_t count = be16(&p[123]);   // includes the start code
    if (count < 1 || p[125] != 0x00) {
        return;                       // only NULL start code (dimmer data)
    }
    if (!sequence_ok(cid, seq)) {
        return;
    }
    uint16_t slots = count - 1;
    if (slots > DMX_SLOTS) {
        slots = DMX_SLOTS;
    }
    if (126 + slots > len) {
        slots = len - 126;
    }
    if (priority > 200) {
        priority = 200;
    }
    dmx_buffer_submit(SRC_SACN, cid, from->sin_addr.s_addr, priority, &p[126], slots);
}

static void sacn_task(void *arg)
{
    static uint8_t buf[700];
    uint16_t last_universe = g_config.sacn_universe;
    uint8_t last_proto = g_config.protocol;

    while (1) {
        if (g_config.sacn_universe != last_universe || g_config.protocol != last_proto) {
            last_universe = g_config.sacn_universe;
            last_proto = g_config.protocol;
            memset(s_seq, 0, sizeof(s_seq));
            s_rejoin = true;
        }
        if (s_rejoin) {
            update_membership();
        }
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            handle_packet(buf, n, &from);
        }
        // n < 0 with EAGAIN is the 500 ms receive timeout: loop to re-check config.
    }
}

esp_err_t sacn_start(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SACN_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(s_sock);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "listening on UDP %d, universe %u", SACN_PORT, g_config.sacn_universe);
    xTaskCreatePinnedToCore(sacn_task, "sacn", 4096, NULL, 12, NULL, 0);
    return ESP_OK;
}
