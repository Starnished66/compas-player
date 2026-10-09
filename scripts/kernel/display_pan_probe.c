/* Read-only-mode framebuffer pan benchmark for the R1 RGB565 scanout path.
 * It never maps or writes framebuffer pixels. It only changes yoffset through
 * FBIOPAN_DISPLAY and restores the original yoffset after a successful run. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fb.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FLIPS 200U
#define MAX_FLIPS 1000U
#define MAX_RUN_US UINT64_C(30000000)
#define R1_FRAME_INTERVAL_US (1000000U / 62U)
#define LINUX_FB_MAJOR 29U

static int monotonic_us(uint64_t *out)
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
	if (seconds > (UINT64_MAX - (uint64_t)ts.tv_nsec / 1000U) /
		      UINT64_C(1000000)) {
		errno = EOVERFLOW;
		return -1;
	}
	*out = seconds * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000U;
	return 0;
}

static int parse_flips(const char *text, unsigned int *out)
{
	char *end = NULL;
	unsigned long value;
	if (!text || !*text || *text == '-')
		return -1;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !end || *end || !value || value > MAX_FLIPS)
		return -1;
	*out = (unsigned int)value;
	return 0;
}

static int rgb565_fb(const struct fb_fix_screeninfo *fix,
		     const struct fb_var_screeninfo *var)
{
	return fix->type == FB_TYPE_PACKED_PIXELS &&
	       fix->visual == FB_VISUAL_TRUECOLOR &&
	       var->xres == 480U && var->yres == 800U &&
	       var->xres_virtual == 480U && var->yres_virtual == 1600U &&
	       var->bits_per_pixel == 16U && var->grayscale == 0U &&
	       var->nonstd == 0U && var->xoffset == 0U &&
	       (var->yoffset == 0U || var->yoffset == 800U) &&
	       var->rotate == 0U && fix->line_length == 960U &&
	       fix->smem_len >= 2U * 960U * 800U && fix->ypanstep > 0U &&
	       800U % fix->ypanstep == 0U &&
	       (var->vmode & (FB_VMODE_YWRAP | FB_VMODE_INTERLACED)) == 0U &&
	       var->red.offset == 11U && var->red.length == 5U &&
	       var->red.msb_right == 0U && var->green.offset == 5U &&
	       var->green.length == 6U && var->green.msb_right == 0U &&
	       var->blue.offset == 0U && var->blue.length == 5U &&
	       var->blue.msb_right == 0U && var->transp.offset == 0U &&
	       var->transp.length == 0U && var->transp.msb_right == 0U;
}

static int timed_pan(int fd, struct fb_var_screeninfo *var,
		     uint64_t *elapsed, int *saved_error,
		     int *ioctl_succeeded)
{
	uint64_t start, end;
	*ioctl_succeeded = 0;
	if (monotonic_us(&start) != 0) {
		*saved_error = errno ? errno : EIO;
		return -2;
	}
	if (ioctl(fd, FBIOPAN_DISPLAY, var) != 0) {
		*saved_error = errno ? errno : EIO;
		return -1;
	}
	*ioctl_succeeded = 1;
	if (monotonic_us(&end) != 0) {
		*saved_error = errno ? errno : EIO;
		return -2;
	}
	if (end < start) {
		*saved_error = EIO;
		return -2;
	}
	*elapsed = end - start;
	return 0;
}

static int compare_u64(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}

static void emit_json_string(const char *value)
{
	const unsigned char *p = (const unsigned char *)value;
	putchar('"');
	for (; *p; ++p) {
		if (*p == '"' || *p == '\\') {
			putchar('\\');
			putchar(*p);
		} else if (*p < 0x20U) {
			printf("\\u%04x", (unsigned int)*p);
		} else {
			putchar(*p);
		}
	}
	putchar('"');
}

static void emit_report(const char *device, unsigned int requested,
			unsigned int succeeded, unsigned int pan_failures,
			unsigned int sample_count, unsigned int timing_failures,
			unsigned int original_yoffset,
			unsigned int last_known_yoffset,
			unsigned int requested_yoffset,
			int restore_attempted,
			int restore_failed, int restore_skipped_uncertain,
			int state_uncertain, int budget_exhausted,
			int metadata_valid, int clock_fatal, int close_failed,
			int failure_errno, uint64_t clock_resolution_ns,
			uint64_t *samples)
{
	uint64_t min = 0, max = 0, total = 0, p50 = 0, p95 = 0;
	unsigned int over_deadline = 0, i;
	if (sample_count) {
		qsort(samples, sample_count, sizeof(*samples), compare_u64);
		min = samples[0];
		max = samples[sample_count - 1];
		for (i = 0; i < sample_count; ++i) {
			if (UINT64_MAX - total < samples[i]) {
				total = UINT64_MAX;
				break;
			}
			total += samples[i];
			if (samples[i] > R1_FRAME_INTERVAL_US)
				++over_deadline;
		}
		p50 = samples[(sample_count - 1U) / 2U];
		p95 = samples[((uint64_t)(sample_count - 1U) * 95U) / 100U];
	}
	printf("{\"device\":");
	emit_json_string(device);
	printf(",\"frame_interval_us_62hz\":%u,\"clock_resolution_ns\":%" PRIu64
	       ",\"flips_requested\":%u,\"successful_pan_ioctls\":%u"
	       ",\"original_yoffset\":%u,\"last_known_yoffset\":%u"
	       ",\"requested_yoffset\":%u,\"state_uncertain\":%s"
	       ",\"budget_exhausted\":%s"
	       ",\"pan_failures\":%u,\"latency_samples\":%u"
	       ",\"timing_failures\":%u"
	       ",\"restore_attempted\":%s,\"restore_failed\":%s"
	       ",\"restore_skipped_uncertain\":%s,\"metadata_valid\":%s"
	       ",\"clock_fatal\":%s,\"close_failed\":%s"
	       ",\"failure_errno\":%d,\"latency_min_us\":%" PRIu64
	       ",\"latency_max_us\":%" PRIu64 ",\"latency_mean_us\":%" PRIu64
	       ",\"latency_p50_us\":%" PRIu64 ",\"latency_p95_us\":%" PRIu64
	       ",\"over_62hz_deadline_count\":%u}\n",
	       R1_FRAME_INTERVAL_US, clock_resolution_ns, requested, succeeded,
	       original_yoffset, last_known_yoffset, requested_yoffset,
	       state_uncertain ? "true" : "false",
	       budget_exhausted ? "true" : "false", pan_failures,
	       sample_count, timing_failures, restore_attempted ? "true" : "false",
	       restore_failed ? "true" : "false",
	       restore_skipped_uncertain ? "true" : "false",
	       metadata_valid ? "true" : "false", clock_fatal ? "true" : "false",
	       close_failed ? "true" : "false", failure_errno, min, max,
	       sample_count ? total / sample_count : 0, p50, p95, over_deadline);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/fb0";
	unsigned int requested = DEFAULT_FLIPS;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo original, pan;
	struct stat device_stat;
	struct timespec resolution;
	uint64_t *samples = NULL;
	uint64_t run_start_us = 0, clock_resolution_ns = 0;
	unsigned int succeeded = 0, sample_count = 0;
	unsigned int pan_failures = 0, timing_failures = 0;
	unsigned int original_yoffset = 0, last_known_yoffset = 0;
	unsigned int requested_yoffset = 0;
	int restore_attempted = 0, restore_failed = 0;
	int restore_skipped_uncertain = 0, metadata_valid = 0;
	int state_uncertain = 0, budget_exhausted = 0;
	int clock_fatal = 0, close_failed = 0, failure_errno = 0;
	int fd = -1, rc = 0;
	int i;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			device = argv[++i];
			if (strcmp(device, "/dev/fb0") != 0) {
				fprintf(stderr, "display_pan_probe: only /dev/fb0 is supported\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--flips") && i + 1 < argc) {
			if (parse_flips(argv[++i], &requested) != 0) {
				fprintf(stderr, "display_pan_probe: --flips must be 1..%u\n", MAX_FLIPS);
				return 2;
			}
		} else if (!strcmp(argv[i], "--help")) {
			printf("Usage: %s [--device /dev/fb0] [--flips 1..%u]\n",
			       argv[0], MAX_FLIPS);
			return 0;
		} else {
			fprintf(stderr, "Usage: %s [--device /dev/fb0] [--flips 1..%u]\n",
				argv[0], MAX_FLIPS);
			return 2;
		}
	}

	samples = calloc(requested, sizeof(*samples));
	if (!samples) {
		fprintf(stderr, "display_pan_probe: latency allocation failed\n");
		return 1;
	}
	fd = open(device, O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		failure_errno = errno;
		rc = 1;
		goto report;
	}
	if (fstat(fd, &device_stat) != 0) {
		failure_errno = errno ? errno : EIO;
		rc = 1;
		goto close_report;
	}
	if (!S_ISCHR(device_stat.st_mode) ||
	    major(device_stat.st_rdev) != LINUX_FB_MAJOR) {
		failure_errno = ENODEV;
		rc = 1;
		goto close_report;
	}
	memset(&fix, 0, sizeof(fix));
	memset(&original, 0, sizeof(original));
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &original) != 0) {
		failure_errno = errno ? errno : EIO;
		rc = 1;
		goto close_report;
	}
	original_yoffset = original.yoffset;
	last_known_yoffset = original.yoffset;
	metadata_valid = rgb565_fb(&fix, &original);
	if (!metadata_valid) {
		failure_errno = EINVAL;
		rc = 3;
		goto close_report;
	}

	if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0) {
		failure_errno = errno ? errno : EIO;
		clock_fatal = 1;
		rc = 1;
		goto restore_known;
	}
	if (resolution.tv_sec < 0 || resolution.tv_nsec < 0 ||
	    resolution.tv_nsec >= 1000000000L ||
	    (uint64_t)resolution.tv_sec >
	    (UINT64_MAX - (uint64_t)resolution.tv_nsec) / UINT64_C(1000000000)) {
		failure_errno = EOVERFLOW;
		clock_fatal = 1;
		rc = 1;
		goto restore_known;
	}
	clock_resolution_ns = (uint64_t)resolution.tv_sec * UINT64_C(1000000000) +
		(uint64_t)resolution.tv_nsec;
	if (monotonic_us(&run_start_us) != 0) {
		failure_errno = errno ? errno : EIO;
		clock_fatal = 1;
		rc = 1;
		goto restore_known;
	}
	pan = original;
	while (succeeded < requested) {
		uint64_t elapsed = 0;
		uint64_t now_us = 0;
		unsigned int target_yoffset;
		int pan_errno = 0;
		int ioctl_succeeded = 0;
		if (monotonic_us(&now_us) != 0) {
			failure_errno = errno ? errno : EIO;
			clock_fatal = 1;
			++timing_failures;
			rc = 1;
			goto restore_known;
		}
		if (now_us < run_start_us) {
			failure_errno = EIO;
			clock_fatal = 1;
			++timing_failures;
			rc = 1;
			goto restore_known;
		}
		if (now_us - run_start_us >= MAX_RUN_US) {
			budget_exhausted = 1;
			failure_errno = ETIMEDOUT;
			rc = 1;
			goto restore_known;
		}
		target_yoffset = (last_known_yoffset == 0U) ? 800U : 0U;
		requested_yoffset = target_yoffset;
		pan = original;
		pan.yoffset = target_yoffset;
		i = timed_pan(fd, &pan, &elapsed, &pan_errno, &ioctl_succeeded);
		if (i != 0) {
			if (i == -1)
				++pan_failures;
			else {
				++timing_failures;
				clock_fatal = 1;
			}
			if (ioctl_succeeded) {
				++succeeded;
				if (pan.yoffset == target_yoffset)
					last_known_yoffset = target_yoffset;
				else
					state_uncertain = 1;
			}
			failure_errno = pan_errno;
			rc = 1;
			if (i == -1 || state_uncertain) {
				state_uncertain = 1;
				restore_skipped_uncertain = 1;
				goto close_report;
			}
			goto restore_known;
		}
		if (pan.yoffset != target_yoffset) {
			++succeeded;
			samples[succeeded - 1U] = elapsed;
			++sample_count;
			failure_errno = EIO;
			state_uncertain = 1;
			restore_skipped_uncertain = 1;
			rc = 1;
			goto close_report;
		}
		last_known_yoffset = target_yoffset;
		samples[succeeded++] = elapsed;
		++sample_count;
	}
	if (last_known_yoffset != original_yoffset) {
		goto restore_known;
	}
	goto close_report;

restore_known:
	if (!state_uncertain && fd >= 0 && last_known_yoffset != original_yoffset) {
		restore_attempted = 1;
		pan = original;
		if (ioctl(fd, FBIOPAN_DISPLAY, &pan) != 0) {
			restore_failed = 1;
			state_uncertain = 1;
			restore_skipped_uncertain = 1;
			failure_errno = errno ? errno : EIO;
			rc = 1;
			goto close_report;
		}
		if (pan.yoffset != original_yoffset) {
			state_uncertain = 1;
			restore_skipped_uncertain = 1;
			restore_failed = 1;
			failure_errno = EIO;
			rc = 1;
			goto close_report;
		}
		last_known_yoffset = original_yoffset;
	}

close_report:
	if (fd >= 0) {
		if (close(fd) != 0) {
			close_failed = 1;
			if (!failure_errno)
				failure_errno = errno ? errno : EIO;
			rc = 1;
		}
	}
report:
	emit_report(device, requested, succeeded, pan_failures, sample_count,
		    timing_failures, original_yoffset, last_known_yoffset,
		    requested_yoffset,
		    restore_attempted, restore_failed, restore_skipped_uncertain,
		    state_uncertain, budget_exhausted, metadata_valid, clock_fatal,
		    close_failed, failure_errno, clock_resolution_ns, samples);
	if (fflush(stdout) != 0 || ferror(stdout))
		rc = 1;
	free(samples);
	return rc;
}
