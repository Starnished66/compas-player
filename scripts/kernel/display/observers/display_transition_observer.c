/* Bounded, read-only observer for R1 display-controller state transitions.
 * Captures samples in memory and emits CSV only after the capture completes. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DPU_PHYS_BASE UINT32_C(0x13050000)
#define DPU_WINDOW_SIZE UINT32_C(0x00010000)
#define REG_ST UINT32_C(0x2004)
#define REG_FLAG UINT32_C(0x2010)
#define REG_TFT_ST UINT32_C(0x9014)
#define REG_CHAIN_SITE UINT32_C(0x2204)
#define REG_RDMA_SITE UINT32_C(0x3110)
#define REG_CHAIN_ADDR UINT32_C(0x1000)
#define DEFAULT_DURATION_MS 3000U
#define DEFAULT_INTERVAL_US 250U
#define MIN_DURATION_MS 1U
#define MAX_DURATION_MS 3000U
#define MIN_INTERVAL_US 250U
#define MAX_INTERVAL_US 1000U
#define MAX_SAMPLES 12000U

struct sample {
	uint64_t monotonic_ns;
	uint32_t st_before;
	uint32_t flag_before;
	uint32_t tft_st_before;
	uint32_t chain_site_before;
	uint32_t rdma_site;
	uint32_t chain_addr;
	uint32_t st_after;
	uint32_t flag_after;
	uint32_t tft_st_after;
	uint32_t chain_site_after;
};

static int parse_uint(const char *text, unsigned long min, unsigned long max,
		      unsigned long *out)
{
	char *end = NULL;
	unsigned long value;
	if (!text || !*text || *text == '-' || *text == '+')
		return -1;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !end || *end || value < min || value > max)
		return -1;
	*out = value;
	return 0;
}

static int monotonic_ns(uint64_t *out)
{
	struct timespec ts;
	uint64_t seconds;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
		errno = EOVERFLOW;
		return -1;
	}
	seconds = (uint64_t)ts.tv_sec;
	if (seconds > (UINT64_MAX - (uint64_t)ts.tv_nsec) / UINT64_C(1000000000)) {
		errno = EOVERFLOW;
		return -1;
	}
	*out = seconds * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
	return 0;
}

static int sleep_until_ns(uint64_t deadline_ns)
{
	struct timespec deadline;
	int rc;
	deadline.tv_sec = (time_t)(deadline_ns / UINT64_C(1000000000));
	deadline.tv_nsec = (long)(deadline_ns % UINT64_C(1000000000));
	do {
		rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
	} while (rc == EINTR);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	return 0;
}

static uint32_t read_register(const volatile unsigned char *window,
			      uint32_t offset)
{
	const volatile uint32_t *reg =
		(const volatile uint32_t *)(window + offset);
	return *reg;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [--duration-ms 1..%u] [--interval-us %u..%u]\n",
		program, MAX_DURATION_MS, MIN_INTERVAL_US, MAX_INTERVAL_US);
}

int main(int argc, char **argv)
{
	unsigned long duration_ms = DEFAULT_DURATION_MS;
	unsigned long interval_us = DEFAULT_INTERVAL_US;
	unsigned long value;
	int duration_seen = 0, interval_seen = 0;
	int fd = -1, rc = 1, i;
	void *mapping = MAP_FAILED;
	struct sample *samples = NULL;
	uint64_t start_ns = 0, end_ns, interval_ns, duration_ns;
	size_t sample_count = 0, capacity;
	const volatile unsigned char *window;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--help")) {
			usage(stdout, argv[0]);
			return 0;
		} else if (!strcmp(argv[i], "--duration-ms") && i + 1 < argc) {
			if (duration_seen++ || parse_uint(argv[++i], MIN_DURATION_MS,
							  MAX_DURATION_MS, &value) != 0) {
				usage(stderr, argv[0]);
				return 2;
			}
			duration_ms = value;
		} else if (!strcmp(argv[i], "--interval-us") && i + 1 < argc) {
			if (interval_seen++ || parse_uint(argv[++i], MIN_INTERVAL_US,
							  MAX_INTERVAL_US, &value) != 0) {
				usage(stderr, argv[0]);
				return 2;
			}
			interval_us = value;
		} else {
			usage(stderr, argv[0]);
			return 2;
		}
	}

	duration_ns = (uint64_t)duration_ms * UINT64_C(1000000);
	interval_ns = (uint64_t)interval_us * UINT64_C(1000);
	capacity = (size_t)((duration_ns + interval_ns - 1U) / interval_ns);
	if (!capacity || capacity > MAX_SAMPLES) {
		fprintf(stderr, "display_transition_observer: sample bound exceeded\n");
		return 2;
	}
	samples = calloc(capacity, sizeof(*samples));
	if (!samples) {
		fprintf(stderr, "display_transition_observer: sample allocation failed\n");
		return 1;
	}

	fd = open("/dev/mem", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "display_transition_observer: open /dev/mem: %s\n",
			strerror(errno));
		goto cleanup;
	}
	mapping = mmap(NULL, DPU_WINDOW_SIZE, PROT_READ, MAP_SHARED, fd,
		       (off_t)DPU_PHYS_BASE);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "display_transition_observer: mmap DPU window: %s\n",
			strerror(errno));
		goto cleanup;
	}
	window = (const volatile unsigned char *)mapping;
	if (monotonic_ns(&start_ns) != 0) {
		fprintf(stderr, "display_transition_observer: monotonic clock: %s\n",
			strerror(errno));
		goto cleanup;
	}
	if (UINT64_MAX - start_ns < duration_ns) {
		errno = EOVERFLOW;
		fprintf(stderr, "display_transition_observer: duration overflow\n");
		goto cleanup;
	}
	end_ns = start_ns + duration_ns;

	while (sample_count < capacity) {
		struct sample *sample = &samples[sample_count];
		uint64_t timestamp_ns;
		uint64_t next_ns;
		if (monotonic_ns(&timestamp_ns) != 0) {
			fprintf(stderr, "display_transition_observer: monotonic clock: %s\n",
				strerror(errno));
			goto cleanup;
		}
		if (timestamp_ns >= end_ns)
			break;
		sample->monotonic_ns = timestamp_ns;
		sample->st_before = read_register(window, REG_ST);
		sample->flag_before = read_register(window, REG_FLAG);
		sample->tft_st_before = read_register(window, REG_TFT_ST);
		sample->chain_site_before = read_register(window, REG_CHAIN_SITE);
		sample->rdma_site = read_register(window, REG_RDMA_SITE);
		sample->chain_addr = read_register(window, REG_CHAIN_ADDR);
		sample->st_after = read_register(window, REG_ST);
		sample->flag_after = read_register(window, REG_FLAG);
		sample->tft_st_after = read_register(window, REG_TFT_ST);
		sample->chain_site_after = read_register(window, REG_CHAIN_SITE);
		++sample_count;

		if ((uint64_t)sample_count > (UINT64_MAX - start_ns) / interval_ns)
			break;
		next_ns = start_ns + (uint64_t)sample_count * interval_ns;
		if (next_ns >= end_ns)
			break;
		if (sleep_until_ns(next_ns) != 0) {
			fprintf(stderr, "display_transition_observer: interval sleep: %s\n",
				strerror(errno));
			goto cleanup;
		}
	}

	if (printf("monotonic_ns,st_before,flag_before,tft_st_before,chain_site_before,"
		   "rdma_site,chain_addr,st_after,flag_after,tft_st_after,chain_site_after\n") < 0)
		goto output_error;
	for (i = 0; (size_t)i < sample_count; ++i) {
		const struct sample *sample = &samples[i];
		if (printf("%" PRIu64 ",0x%08" PRIx32 ",0x%08" PRIx32
			   ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32
			   ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32
			   ",0x%08" PRIx32 ",0x%08" PRIx32 "\n",
			   sample->monotonic_ns, sample->st_before, sample->flag_before,
			   sample->tft_st_before, sample->chain_site_before,
			   sample->rdma_site, sample->chain_addr, sample->st_after,
			   sample->flag_after, sample->tft_st_after,
			   sample->chain_site_after) < 0)
			goto output_error;
	}
	if (fflush(stdout) != 0)
		goto output_error;
	rc = 0;
	goto cleanup;

output_error:
	fprintf(stderr, "display_transition_observer: CSV output failed: %s\n",
		strerror(errno ? errno : EIO));
cleanup:
	if (mapping != MAP_FAILED && munmap(mapping, DPU_WINDOW_SIZE) != 0) {
		fprintf(stderr, "display_transition_observer: munmap: %s\n",
			strerror(errno));
		rc = 1;
	}
	if (fd >= 0 && close(fd) != 0) {
		fprintf(stderr, "display_transition_observer: close /dev/mem: %s\n",
			strerror(errno));
		rc = 1;
	}
	free(samples);
	return rc;
}
