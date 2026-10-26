#include "remote_control_mdns.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT 5353
#define RC_PORT 8899
#define SERVICE_NAME "_compas-remote._tcp.local"
#define INSTANCE_NAME "compás." SERVICE_NAME
/* Use the OS brand on every board; probing adds a suffix on collision. */
#define DEVICE_LABEL "compas"
#define DEVICE_DOMAIN "local"
#define MAX_NAME_SUFFIX 9
#define PROBE_COUNT 3
#define PROBE_INTERVAL_MS 250
#define LEGACY_UNICAST_TTL 10

static atomic_bool active;
static pthread_t worker;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t name_mutex = PTHREAD_MUTEX_INITIALIZER;
static char published_name[64];

static uint16_t put16(uint8_t *p, uint16_t v) {
    v = htons(v); memcpy(p, &v, 2); return 2;
}

static uint16_t put32(uint8_t *p, uint32_t v) {
    v = htonl(v); memcpy(p, &v, 4); return 4;
}

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static size_t put_name(uint8_t *out, size_t capacity, const char *name) {
    size_t n = 0;
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (!len || len > 63 || n + len + 2 > 255 || n + len + 2 > capacity) return 0;
        out[n++] = (uint8_t)len;
        memcpy(out + n, label, len); n += len;
        if (!dot) break;
        label = dot + 1;
    }
    out[n++] = 0;
    return n;
}

static bool decode_name(const uint8_t *packet, size_t packet_len, size_t *offset,
                        char *out, size_t out_size) {
    size_t pos = *offset, written = 0, resume = 0;
    bool jumped = false;
    unsigned labels = 0;
    for (;;) {
        if (pos >= packet_len || ++labels > 128) return false;
        uint8_t len = packet[pos++];
        if ((len & 0xc0) == 0xc0) {
            if (pos >= packet_len) return false;
            size_t pointer = ((size_t)(len & 0x3f) << 8) | packet[pos++];
            if (pointer >= packet_len) return false;
            if (!jumped) resume = pos;
            jumped = true; pos = pointer;
            continue;
        }
        if (len & 0xc0 || pos + len > packet_len) return false;
        if (!len) break;
        if (written && written + 1 >= out_size) return false;
        if (written) out[written++] = '.';
        if (written + len >= out_size) return false;
        for (unsigned i = 0; i < len; i++) {
            unsigned char c = packet[pos++];
            out[written++] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        }
    }
    out[written] = '\0';
    *offset = jumped ? resume : pos;
    return true;
}

static bool append_record(uint8_t *packet, size_t *offset, const char *owner,
                          uint16_t type, uint32_t ttl, bool unique,
                          const uint8_t *data, size_t len) {
    size_t n = *offset;
    if (n + 255 + 10 + len > 1400) return false;
    size_t name_len = put_name(packet + n, 1400 - n, owner);
    if (!name_len || n + name_len + 10 + len > 1400) return false;
    n += name_len;
    n += put16(packet + n, type);
    n += put16(packet + n, (uint16_t)(1 | (unique ? 0x8000 : 0))); /* IN; cache flush on unique records */
    n += put32(packet + n, ttl);
    n += put16(packet + n, (uint16_t)len);
    memcpy(packet + n, data, len); n += len;
    *offset = n;
    return true;
}

/* The SRV record and address replies use the same collision-checked name. */
static size_t make_answer(uint8_t *packet, const char *host, struct in_addr ip,
                          uint32_t ttl, const char *qname, uint16_t qtype,
                          unsigned *answers, unsigned *additional) {
    /* Match the probed hostname suffix so multiple players remain discoverable. */
    char instance[sizeof(INSTANCE_NAME) + 8];
    unsigned suffix = 1;
    if (sscanf(host, DEVICE_LABEL "-%u." DEVICE_DOMAIN, &suffix) == 1 &&
        suffix > 1 && suffix <= MAX_NAME_SUFFIX)
        snprintf(instance, sizeof(instance), "compás (%u)." SERVICE_NAME, suffix);
    else
        snprintf(instance, sizeof(instance), "%s", INSTANCE_NAME);
    memset(packet, 0, 1400);
    put16(packet, 0); put16(packet + 2, 0x8400); /* response, authoritative */
    *answers = *additional = 0;
    size_t n = 12;
    uint8_t data[300];
    bool announce = !qname;
    bool service = announce || strcasecmp(qname, SERVICE_NAME) == 0;
    bool instance_query = announce || strcasecmp(qname, instance) == 0;
    bool host_query = !announce && strcasecmp(qname, host) == 0;
    bool ptr = service && (announce || qtype == 12 || qtype == 255);
    bool srv = instance_query && (announce || qtype == 33 || qtype == 255);
    bool txt = instance_query && (announce || qtype == 16 || qtype == 255);
    bool address = host_query && (qtype == 1 || qtype == 255);
    size_t len = put_name(data, sizeof(data), instance);
    if (ptr && append_record(packet, &n, SERVICE_NAME, 12, ttl, false, data, len)) (*answers)++;
    len = put16(data, 0); len += put16(data + len, 0); len += put16(data + len, RC_PORT);
    len += put_name(data + len, sizeof(data) - len, host);
    if (srv && append_record(packet, &n, instance, 33, ttl, true, data, len)) (*answers)++;
    data[0] = 5; memcpy(data + 1, "api=1", 5);
    if (txt && append_record(packet, &n, instance, 16, ttl, true, data, 6)) (*answers)++;
    memcpy(data, &ip.s_addr, 4);
    if (announce) {
        if (append_record(packet, &n, host, 1, ttl, true, data, 4)) (*answers)++;
    } else if (address && append_record(packet, &n, host, 1, ttl, true, data, 4)) {
        (*answers)++;
    }
    /* Browse answers carry the SRV/TXT/A records as additional data. */
    if (!announce && ptr) {
        len = put16(data, 0); len += put16(data + len, 0); len += put16(data + len, RC_PORT);
        len += put_name(data + len, sizeof(data) - len, host);
        if (append_record(packet, &n, instance, 33, ttl, true, data, len)) (*additional)++;
        data[0] = 5; memcpy(data + 1, "api=1", 5);
        if (append_record(packet, &n, instance, 16, ttl, true, data, 6)) (*additional)++;
        memcpy(data, &ip.s_addr, 4);
        if (append_record(packet, &n, host, 1, ttl, true, data, 4)) (*additional)++;
    } else if (!announce && srv) {
        memcpy(data, &ip.s_addr, 4);
        if (append_record(packet, &n, host, 1, ttl, true, data, 4)) (*additional)++;
    }
    put16(packet + 6, (uint16_t)*answers);
    put16(packet + 10, (uint16_t)*additional);
    return n;
}

static void device_host_name(char *out, size_t size, unsigned suffix) {
    if (suffix <= 1) snprintf(out, size, "%s.%s", DEVICE_LABEL, DEVICE_DOMAIN);
    else snprintf(out, size, "%s-%u.%s", DEVICE_LABEL, suffix, DEVICE_DOMAIN);
}

/* RFC 6762 8.1 probe: an ANY question for host, with the proposed A record
 * in the authority section so simultaneous probers can tie-break. */
static size_t make_probe(uint8_t *packet, const char *host, struct in_addr ip) {
    memset(packet, 0, 1400);
    put16(packet + 4, 1);
    put16(packet + 8, 1);
    size_t n = 12;
    size_t name_len = put_name(packet + n, 1400 - n, host);
    if (!name_len) return 0;
    n += name_len;
    n += put16(packet + n, 255);
    n += put16(packet + n, 0x8001); /* IN, unicast response requested */
    if (!append_record(packet, &n, host, 1, 120, false, (const uint8_t *)&ip.s_addr, 4)) return 0;
    return n;
}

/* True when another device owns host: any response giving it a different
 * address, or a simultaneous probe whose address wins the RFC 6762 8.2
 * tie-break (lexicographically greater rdata). */
static bool packet_claims_host(const uint8_t *packet, size_t len, const char *host, struct in_addr ip) {
    if (len < 12) return false;
    bool response = (packet[2] & 0x80) != 0;
    unsigned questions = read16(packet + 4);
    unsigned answer_count = read16(packet + 6);
    unsigned authority_count = read16(packet + 8);
    unsigned records = answer_count + authority_count + read16(packet + 10);
    size_t offset = 12;
    char owner[256];
    for (unsigned i = 0; i < questions; i++) {
        if (!decode_name(packet, len, &offset, owner, sizeof(owner)) || offset + 4 > len) return false;
        offset += 4;
    }
    for (unsigned i = 0; i < records; i++) {
        if (!decode_name(packet, len, &offset, owner, sizeof(owner)) || offset + 10 > len) return false;
        uint16_t type = read16(packet + offset);
        uint16_t rrclass = read16(packet + offset + 2);
        uint16_t rdlength = read16(packet + offset + 8);
        offset += 10;
        if (offset + rdlength > len) return false;
        const uint8_t *rdata = packet + offset;
        bool authority = i >= answer_count && i < answer_count + authority_count;
        if (type == 1 && (rrclass & 0x7fff) == 1 && rdlength == 4 && strcasecmp(owner, host) == 0) {
            int order = memcmp(rdata, &ip.s_addr, 4);
            if (order != 0 && (response || (authority && order > 0))) return true;
        }
        offset += rdlength;
    }
    return false;
}

/* Sends the probes and listens between them; true when host is taken. */
static bool name_in_use(int fd, const struct sockaddr_in *group, const char *host, struct in_addr ip) {
    uint8_t packet[1500];
    for (int probe = 0; probe < PROBE_COUNT && atomic_load(&active); probe++) {
        size_t len = make_probe(packet, host, ip);
        if (len) (void)sendto(fd, packet, len, 0, (const struct sockaddr *)group, sizeof(*group));
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= PROBE_INTERVAL_MS) break;
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            if (poll(&pfd, 1, (int)(PROBE_INTERVAL_MS - elapsed)) <= 0 || !(pfd.revents & POLLIN)) continue;
            ssize_t got = recv(fd, packet, sizeof(packet), 0);
            if (got > 0 && packet_claims_host(packet, (size_t)got, host, ip)) return true;
        }
    }
    return false;
}

static void set_published_name(const char *name) {
    pthread_mutex_lock(&name_mutex);
    snprintf(published_name, sizeof(published_name), "%s", name ? name : "");
    pthread_mutex_unlock(&name_mutex);
}

bool remote_control_mdns_hostname(char *out, size_t size) {
    pthread_mutex_lock(&name_mutex);
    bool published = published_name[0] != '\0';
    if (published && out && size) snprintf(out, size, "%s", published_name);
    pthread_mutex_unlock(&name_mutex);
    return published;
}

static bool wifi_address(struct in_addr *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.ifr_name, sizeof(req.ifr_name), "wlan0");
    bool ok = ioctl(fd, SIOCGIFFLAGS, &req) == 0 &&
              (req.ifr_flags & IFF_UP) && (req.ifr_flags & IFF_RUNNING);
    if (ok) {
        memset(&req, 0, sizeof(req));
        snprintf(req.ifr_name, sizeof(req.ifr_name), "wlan0");
        ok = ioctl(fd, SIOCGIFADDR, &req) == 0;
    }
    if (ok) {
        struct sockaddr_in *addr = (struct sockaddr_in *)&req.ifr_addr;
        *out = addr->sin_addr;
        ok = out->s_addr != htonl(INADDR_ANY) && (ntohl(out->s_addr) >> 24) != 169;
    }
    close(fd);
    return ok;
}

/* RFC 6762 6.7: a query from a port other than 5353 comes from a one-shot
 * resolver (Android's, dig, ...), which only reads a direct reply carrying
 * its query ID and question. Such replies use a short TTL and never set the
 * cache-flush bit. Rewrites the multicast-style answer in place. */
static size_t make_legacy_answer(uint8_t *packet, size_t len, const uint8_t *query, size_t query_len) {
    size_t question_end = 12;
    char name[256];
    if (query_len < 12 || !decode_name(query, query_len, &question_end, name, sizeof(name)) ||
        question_end + 4 > query_len) return 0;
    question_end += 4;
    size_t question_len = question_end - 12;
    if (len + question_len > 1400) return 0;
    unsigned records = (unsigned)read16(packet + 6) + read16(packet + 8) + read16(packet + 10);
    size_t offset = 12;
    for (unsigned i = 0; i < records; i++) {
        if (!decode_name(packet, len, &offset, name, sizeof(name)) || offset + 10 > len) return 0;
        put16(packet + offset + 2, (uint16_t)(read16(packet + offset + 2) & 0x7fff));
        uint32_t ttl = ((uint32_t)read16(packet + offset + 4) << 16) | read16(packet + offset + 6);
        if (ttl > LEGACY_UNICAST_TTL) put32(packet + offset + 4, LEGACY_UNICAST_TTL);
        offset += 10 + read16(packet + offset + 8);
    }
    memmove(packet + 12 + question_len, packet + 12, len - 12);
    memcpy(packet + 12, query + 12, question_len);
    memcpy(packet, query, 2); /* query ID */
    put16(packet + 4, 1);
    return len + question_len;
}

static void send_legacy_records(int fd, const struct sockaddr_in *to, const char *host,
                                struct in_addr ip, const char *qname, uint16_t qtype,
                                const uint8_t *query, size_t query_len) {
    uint8_t packet[1400];
    unsigned answers, additional;
    size_t len = make_answer(packet, host, ip, LEGACY_UNICAST_TTL, qname, qtype, &answers, &additional);
    if (len <= 12 || !answers) return;
    len = make_legacy_answer(packet, len, query, query_len);
    if (len) (void)sendto(fd, packet, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static void send_records(int fd, const struct sockaddr_in *to, const char *host,
                         struct in_addr ip, uint32_t ttl, const char *qname, uint16_t qtype) {
    uint8_t packet[1400];
    unsigned answers, additional;
    size_t len = make_answer(packet, host, ip, ttl, qname, qtype, &answers, &additional);
    if (len > 12 && answers) (void)sendto(fd, packet, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static bool parse_question(const uint8_t *packet, size_t len, char *name, size_t name_size,
                           uint16_t *type, bool *unicast) {
    if (len < 12 || (packet[2] & 0x80) || (packet[4] == 0 && packet[5] == 0)) return false;
    size_t offset = 12;
    if (!decode_name(packet, len, &offset, name, name_size) || offset + 4 > len) return false;
    *type = ((uint16_t)packet[offset] << 8) | packet[offset + 1];
    uint16_t qclass = ((uint16_t)packet[offset + 2] << 8) | packet[offset + 3];
    *unicast = (qclass & 0x8000) != 0;
    return (qclass & 0x7fff) == 1;
}

static void *mdns_worker(void *unused) {
    (void)unused;
    unsigned suffix = 1;
    char host[64]; device_host_name(host, sizeof(host), suffix);
    struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);
    int fd = -1;
    struct in_addr last_ip = {0};
    time_t last_announce = 0;
    while (atomic_load(&active)) {
        struct in_addr ip;
        if (!wifi_address(&ip)) {
            if (fd >= 0) {
                send_records(fd, &group, host, last_ip, 0, NULL, 255);
                close(fd); fd = -1; last_ip.s_addr = 0;
                set_published_name(NULL);
            }
            poll(NULL, 0, 2000);
            continue;
        }
        if (fd < 0 || ip.s_addr != last_ip.s_addr) {
            if (fd >= 0) {
                send_records(fd, &group, host, last_ip, 0, NULL, 255);
                close(fd);
                set_published_name(NULL);
            }
            fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) { poll(NULL, 0, 2000); continue; }
            int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
            struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
            if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) { close(fd); fd = -1; poll(NULL, 0, 3000); continue; }
            struct ip_mreq membership = { .imr_multiaddr = group.sin_addr, .imr_interface = ip };
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) { close(fd); fd = -1; poll(NULL, 0, 3000); continue; }
            unsigned char ttl = 255, loop = 1;
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ip, sizeof(ip));
            last_ip = ip;
            while (atomic_load(&active) && name_in_use(fd, &group, host, ip) && suffix < MAX_NAME_SUFFIX)
                device_host_name(host, sizeof(host), ++suffix);
            if (!atomic_load(&active)) break;
            send_records(fd, &group, host, ip, 120, NULL, 255);
            set_published_name(host);
            last_announce = time(NULL);
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 2000);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            uint8_t query[1500];
            struct sockaddr_in source;
            socklen_t source_len = sizeof(source);
            ssize_t got = recvfrom(fd, query, sizeof(query), 0, (struct sockaddr *)&source, &source_len);
            char qname[256]; uint16_t qtype; bool unicast;
            if (got > 0 && (query[2] & 0x80) && suffix < MAX_NAME_SUFFIX &&
                packet_claims_host(query, (size_t)got, host, ip)) {
                /* Another device answered for our name: re-probe under the next one. */
                device_host_name(host, sizeof(host), ++suffix);
                close(fd); fd = -1;
                set_published_name(NULL);
                continue;
            }
            if (got > 0 && parse_question(query, (size_t)got, qname, sizeof(qname), &qtype, &unicast)) {
                if (ntohs(source.sin_port) != MDNS_PORT)
                    send_legacy_records(fd, &source, host, ip, qname, qtype, query, (size_t)got);
                else
                    send_records(fd, unicast ? &source : &group, host, ip, 120, qname, qtype);
            }
        }
        time_t now = time(NULL);
        if (now - last_announce >= 60) {
            send_records(fd, &group, host, ip, 120, NULL, 255);
            last_announce = now;
        }
    }
    if (fd >= 0) { send_records(fd, &group, host, last_ip, 0, NULL, 255); close(fd); }
    set_published_name(NULL);
    return NULL;
}

void remote_control_mdns_start(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    if (!atomic_load(&active)) {
        atomic_store(&active, true);
        if (pthread_create(&worker, NULL, mdns_worker, NULL) != 0) atomic_store(&active, false);
    }
    pthread_mutex_unlock(&lifecycle_mutex);
}

void remote_control_mdns_stop(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    bool was_active = atomic_exchange(&active, false);
    pthread_t thread = worker;
    pthread_mutex_unlock(&lifecycle_mutex);
    if (was_active) pthread_join(thread, NULL);
}
