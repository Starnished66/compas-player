/* Host stubs and behavior checks for code extracted from the kernel patch. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef unsigned long gfp_t;
typedef unsigned long spinlock_t;
typedef unsigned long work_struct;
typedef unsigned long delayed_work;
struct class { const char *name; };
struct device {
	const char *name;
	struct device *parent;
	struct class *class;
	void *driver;
	bool registered;
	bool locked;
	bool runtime_pm;
};
struct mmc_host;
struct mmc_card;
struct sdio_func {
	struct device dev;
	struct mmc_card *card;
	unsigned int num;
	u16 vendor, device;
};
struct sdio_func_tuple {
	u8 code, size;
	u8 *data;
	struct sdio_func_tuple *next;
};
struct mmc_card {
	struct device dev;
	unsigned int sdio_funcs;
	struct sdio_func *sdio_func[7];
	struct sdio_func_tuple *tuples;
	bool is_sdio;
};
struct mmc_host {
	struct device *parent;
	struct device class_dev;
	unsigned int caps;
	spinlock_t lock;
	int rescan_disable;
	int rescan_entered;
	delayed_work detect;
	struct mmc_card *card;
	const char *hostname;
	unsigned int pm_flags;
};

#define SDIO_MAX_FUNCS 7
#define MMC_CAP_NONREMOVABLE 1
#define MMC_CAP_POWER_OFF_CARD 2
#define MMC_PM_KEEP_POWER 1
#define GFP_KERNEL 0
#define DEFINE_SPINLOCK(name) spinlock_t name
#define EXPORT_SYMBOL_GPL(x)
#define mmc_dev(h) ((h)->parent)
#define mmc_card_sdio(c) ((c)->is_sdio)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)

struct mmc_compas_sdio_guard;
struct mmc_compas_sdio_identity {
	u16 function2_vendor, function2_device;
	bool valid, maker_tuple_present, azurewave;
	u8 maker_tuple_value;
};
enum mmc_compas_sdio_end_policy {
	MMC_COMPAS_SDIO_PRESERVE,
	MMC_COMPAS_SDIO_FORCE,
	MMC_COMPAS_SDIO_DISCARD,
};

static char events[4096];
static size_t events_len;
static bool sleep_locked;
static bool pending_detect;
static bool inject_work_during_guard;
static unsigned int detect_cancels;
static unsigned int scans_queued;
static unsigned int parent_unlocks;
static unsigned int function_unlocks;
static unsigned int power_saves, power_restores;
static unsigned int hardware_power_offs, clock_stops;
static unsigned int ordinary_rescan_work, resume_reinitializations;
static unsigned int cmd0_count, cmd5_count, card_init_count, width_change_count;
static int power_result;
static struct class mmc_host_class = { "mmc_host" };

static void event(const char *name)
{
	size_t n = strlen(name);
	if (events_len && events_len < sizeof(events) - 1)
		events[events_len++] = ',';
	if (n > sizeof(events) - events_len - 1)
		n = sizeof(events) - events_len - 1;
	memcpy(events + events_len, name, n);
	events_len += n;
	events[events_len] = '\0';
}

static void reset_observation(void)
{
	memset(events, 0, sizeof(events));
	events_len = 0;
	sleep_locked = false;
	pending_detect = false;
	inject_work_during_guard = false;
	detect_cancels = scans_queued = parent_unlocks = function_unlocks = 0;
	power_saves = power_restores = 0;
	hardware_power_offs = clock_stops = 0;
	ordinary_rescan_work = resume_reinitializations = 0;
	cmd0_count = cmd5_count = card_init_count = width_change_count = 0;
	power_result = 0;
}

static void *kzalloc(size_t size, gfp_t flags)
{
	(void)flags;
	return calloc(1, size);
}
static void kfree(void *p) { free(p); }
static bool in_interrupt(void) { return false; }
static bool irqs_disabled(void) { return false; }
static bool in_atomic(void) { return false; }
static void lock_system_sleep(void) { sleep_locked = true; event("sleep_lock"); }
static void unlock_system_sleep(void) { sleep_locked = false; event("sleep_unlock"); }
static bool device_trylock(struct device *d)
{
	if (d->locked)
		return false;
	d->locked = true;
	if (d->name && !strncmp(d->name, "func", 4))
		event(d->name);
	else
		event("parent_lock");
	return true;
}
static void device_unlock(struct device *d)
{
	d->locked = false;
	if (d->name && !strncmp(d->name, "func", 4)) {
		function_unlocks++;
		event(d->name);
	} else {
		parent_unlocks++;
		event("parent_unlock");
	}
}
static bool device_is_registered(struct device *d) { return d->registered; }
static const char *dev_name(struct device *d) { return d->name; }
static const char *mmc_hostname(struct mmc_host *h) { return h->hostname; }
static bool pm_runtime_enabled(struct device *d) { return d->runtime_pm; }
static void cancel_delayed_work_sync(delayed_work *work)
{
	(void)work;
	detect_cancels++;
	event("cancel_detect");
	pending_detect = false;
}
static void _mmc_detect_change(struct mmc_host *host, unsigned int delay, bool cd_irq)
{
	(void)host; (void)delay; (void)cd_irq;
	scans_queued++;
	pending_detect = true;
	event("queue_detect");
}
static int mmc_claim_host(struct mmc_host *host) { (void)host; event("claim_host"); return 0; }
static void mmc_release_host(struct mmc_host *host) { (void)host; event("release_host"); }
static int mmc_power_restore_host(struct mmc_host *host)
{
	(void)host; power_restores++; event("power_restore"); return power_result;
}
static int mmc_power_save_host(struct mmc_host *host)
{
	(void)host; power_saves++; event("power_save"); return power_result;
}
static void get_device(struct device *d) { (void)d; }
static void mmc_power_off(struct mmc_host *host)
{
	(void)host; hardware_power_offs++; event("power_off");
}
static void mmc_set_clock(struct mmc_host *host, unsigned int hz)
{
	(void)host;
	if (!hz)
		clock_stops++;
	event("clock");
}

/* ACTUAL_HELPER_SOURCE */

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL line %d: %s\nevents=%s\n", __LINE__, #c, events); \
	return 1; } } while (0)

struct fixture {
	struct device parent;
	struct mmc_host host;
	struct mmc_card card;
	struct sdio_func funcs[2];
	struct sdio_func_tuple tuples[2];
	u8 tuple_data[2];
};

static void fixture_init(struct fixture *f, bool with_card)
{
	memset(f, 0, sizeof(*f));
	f->parent.name = "md_ingenic,mmc.0";
	f->parent.registered = true;
	f->parent.driver = (void *)1;
	f->host.parent = &f->parent;
	f->host.caps = MMC_CAP_NONREMOVABLE;
	f->host.hostname = "mmc0";
	f->host.class_dev.name = "mmc0";
	f->host.class_dev.registered = true;
	f->host.class_dev.parent = &f->parent;
	f->host.class_dev.class = &mmc_host_class;
	if (!with_card)
		return;
	f->card.dev.registered = true;
	f->card.is_sdio = true;
	f->card.sdio_funcs = 2;
	f->card.sdio_func[0] = &f->funcs[0]; /* SDK convention: function 1 */
	f->card.sdio_func[1] = &f->funcs[1]; /* SDK convention: function 2 */
	f->host.card = &f->card;
	f->funcs[0].num = 1;
	f->funcs[1].num = 2;
	f->funcs[0].card = f->funcs[1].card = &f->card;
	f->funcs[0].dev.name = "func1";
	f->funcs[1].dev.name = "func2";
	f->funcs[0].dev.registered = f->funcs[1].dev.registered = true;
	f->funcs[1].vendor = 0x02d0;
	f->funcs[1].device = 0xa9a6;
}

static int check_begin_failure(struct fixture *f, int expected, bool disabled_before_error)
{
	struct mmc_compas_sdio_guard *g = NULL;
	int original_rescan_disabled = f->host.rescan_disable;
	bool original_parent_lock = f->parent.locked;
	bool original_func1_lock = f->funcs[0].dev.locked;
	bool original_func2_lock = f->funcs[1].dev.locked;
	int ret = mmc_compas_sdio_begin(&f->host, &f->parent, &g);
	CHECK(ret == expected && g == NULL);
	CHECK(!sleep_locked && f->host.rescan_disable == original_rescan_disabled);
	CHECK(f->parent.locked == original_parent_lock);
	CHECK(f->funcs[0].dev.locked == original_func1_lock &&
	      f->funcs[1].dev.locked == original_func2_lock);
	CHECK(detect_cancels == (disabled_before_error ? 2u : 0u));
	CHECK(scans_queued == (disabled_before_error ? 1u : 0u));
	return 0;
}

static int test_begin_failure_paths_restore_owned_state(void)
{
	struct fixture f;

	reset_observation(); fixture_init(&f, false); f.parent.locked = true;
	CHECK(check_begin_failure(&f, -EBUSY, false) == 0);
	f.parent.locked = false;

	reset_observation(); fixture_init(&f, false); f.parent.registered = false;
	CHECK(check_begin_failure(&f, -ENODEV, false) == 0);
	reset_observation(); fixture_init(&f, false); f.host.caps = 0;
	CHECK(check_begin_failure(&f, -EOPNOTSUPP, false) == 0);
	reset_observation(); fixture_init(&f, false); f.host.rescan_disable = 1;
	CHECK(check_begin_failure(&f, -EBUSY, false) == 0);

	reset_observation(); fixture_init(&f, true); f.card.is_sdio = false;
	CHECK(check_begin_failure(&f, -EOPNOTSUPP, true) == 0);
	reset_observation(); fixture_init(&f, true); f.card.dev.runtime_pm = true;
	CHECK(check_begin_failure(&f, -EBUSY, true) == 0);
	reset_observation(); fixture_init(&f, true); f.funcs[0].num = 2;
	CHECK(check_begin_failure(&f, -ENODEV, true) == 0);
	reset_observation(); fixture_init(&f, true); f.funcs[0].dev.locked = true;
	CHECK(check_begin_failure(&f, -EBUSY, true) == 0);
	reset_observation(); fixture_init(&f, true); f.funcs[0].dev.runtime_pm = true;
	CHECK(check_begin_failure(&f, -EBUSY, true) == 0);
	return 0;
}

static int test_bound_func1_fails_and_unwinds(void)
{
	struct fixture f; struct mmc_compas_sdio_guard *g = NULL;
	reset_observation(); fixture_init(&f, true); f.funcs[0].dev.driver = (void *)1;
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == -EBUSY);
	CHECK(g == NULL && !sleep_locked && !f.parent.locked && !f.host.rescan_disable);
	CHECK(!f.funcs[0].dev.locked && !f.funcs[1].dev.locked);
	CHECK(detect_cancels == 2 && scans_queued == 1 && parent_unlocks == 1);
	CHECK(strstr(events, "func1,func1") != NULL);
	return 0;
}

static int test_valid_identity_uses_slot_one_for_function_two(void)
{
	struct fixture f; struct mmc_compas_sdio_guard *g = NULL;
	struct mmc_compas_sdio_identity id;
	reset_observation(); fixture_init(&f, true);
	f.tuple_data[0] = 1;
	f.tuples[0].code = 0x81; f.tuples[0].size = 1;
	f.tuples[0].data = &f.tuple_data[0]; f.card.tuples = &f.tuples[0];
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == 0);
	CHECK(g && f.funcs[0].dev.locked && f.funcs[1].dev.locked);
	CHECK(mmc_compas_sdio_identity(g, &id) == 0);
	CHECK(id.valid && id.function2_vendor == 0x02d0 && id.function2_device == 0xa9a6);
	CHECK(id.maker_tuple_present && id.azurewave && id.maker_tuple_value == 1);
	CHECK(f.tuples[0].data == &f.tuple_data[0] && f.tuple_data[0] == 1);
	CHECK(mmc_compas_sdio_power(g, true) == 0 && power_restores == 1);
	CHECK(mmc_compas_sdio_power(g, false) == 0 && power_saves == 1);
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_DISCARD);
	CHECK(!sleep_locked && !f.parent.locked && !f.host.rescan_disable);
	CHECK(!f.funcs[0].dev.locked && !f.funcs[1].dev.locked);
	CHECK(detect_cancels == 2 && scans_queued == 0 && function_unlocks == 2);
	return 0;
}

static int test_bad_tuple_and_function_count_unwind(void)
{
	struct fixture f; struct mmc_compas_sdio_guard *g = NULL;
	struct mmc_compas_sdio_identity id;
	reset_observation(); fixture_init(&f, true);
	f.tuple_data[0] = 1; f.tuple_data[1] = 0;
	f.tuples[0].code = f.tuples[1].code = 0x81;
	f.tuples[0].size = f.tuples[1].size = 1;
	f.tuples[0].data = &f.tuple_data[0]; f.tuples[0].next = &f.tuples[1];
	f.tuples[1].data = &f.tuple_data[1]; f.card.tuples = &f.tuples[0];
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == 0);
	CHECK(mmc_compas_sdio_identity(g, &id) == -EOPNOTSUPP);
	CHECK(id.maker_tuple_present && !id.azurewave);
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_PRESERVE);
	CHECK(scans_queued == 1 && !sleep_locked && !f.parent.locked);

	reset_observation(); fixture_init(&f, true); g = NULL;
	f.tuple_data[0] = 1; f.tuples[0].code = 0x81;
	f.tuples[0].size = 2; f.tuples[0].data = &f.tuple_data[0];
	f.card.tuples = &f.tuples[0];
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == 0);
	CHECK(mmc_compas_sdio_identity(g, &id) == -EOPNOTSUPP);
	CHECK(id.maker_tuple_present);
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_DISCARD);
	CHECK(!sleep_locked && !f.parent.locked && scans_queued == 0);

	reset_observation(); fixture_init(&f, true); f.card.sdio_funcs = 8; g = NULL;
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == -EOVERFLOW);
	CHECK(!sleep_locked && !f.parent.locked && !f.host.rescan_disable);
	CHECK(scans_queued == 1 && detect_cancels == 2);
	return 0;
}

static int test_no_card_rescan_policy_and_discard_drain(void)
{
	struct fixture f; struct mmc_compas_sdio_guard *g = NULL;
	reset_observation(); fixture_init(&f, false);
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == 0);
	CHECK(g && f.host.rescan_disable && !f.host.card);
	/* Work queued while scan is disabled is drained by DISCARD's second cancel. */
	pending_detect = true;
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_DISCARD);
	CHECK(!pending_detect && detect_cancels == 2 && scans_queued == 0);
	CHECK(!f.host.rescan_disable && !sleep_locked && !f.parent.locked);

	reset_observation(); fixture_init(&f, false); g = NULL;
	f.host.rescan_entered = 1;
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == -EOPNOTSUPP);
	CHECK(f.host.rescan_entered == 1 && !sleep_locked && !f.parent.locked);
	CHECK(!f.host.rescan_disable && scans_queued == 1);
	return 0;
}

/* The host stubs model one deterministic interleaving; they do not prove
 * kernel scheduling or locking behavior. */
static struct fixture retained_fixture, late_fixture, other_fixture, unrelated_fixture;
static void simulated_mmc_rescan(struct mmc_host *host)
{
/* ACTUAL_RESCAN_GUARD */
	ordinary_rescan_work++;
}

static int simulated_sdio_resume(struct mmc_host *host)
{
/* ACTUAL_PM_RESUME_HOOK */
	cmd0_count++;
	cmd5_count++;
	card_init_count++;
	width_change_count++;
	resume_reinitializations++;
	return 1;
}

static int simulated_sdio_suspend(struct mmc_host *host)
{
/* ACTUAL_PM_SUSPEND_HOOK */
	return 1;
}

static int test_power_errors_are_returned(void)
{
	struct fixture f; struct mmc_compas_sdio_guard *g = NULL;
	reset_observation(); fixture_init(&f, true); power_result = -EIO;
	CHECK(mmc_compas_sdio_begin(&f.host, &f.parent, &g) == 0);
	CHECK(mmc_compas_sdio_power(g, true) == -EIO);
	CHECK(mmc_compas_sdio_power(g, false) == -EIO);
	CHECK(power_restores == 1 && power_saves == 1);
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_DISCARD);
	return 0;
}

static int test_off_registry_rescan_pm_and_host_identity(void)
{
	struct mmc_compas_sdio_guard *g = NULL, *other_guard = NULL;
	int ret;

	reset_observation();
	fixture_init(&retained_fixture, true);
	retained_fixture.parent.registered = false;
	retained_fixture.host.class_dev.registered = false;
	/* A failed provider init that never calls bootstrap leaves no fence. */
	CHECK(!mmc_compas_sdio_board_off(&retained_fixture.host));
	/* Bootstrap succeeds before this MSC0 parent/host is registered. */
	CHECK(mmc_compas_sdio_bootstrap_off() == 0);
	CHECK(!sleep_locked && mmc_compas_sdio_board_off(&retained_fixture.host));
	retained_fixture.parent.locked = true;
	CHECK(mmc_compas_sdio_begin(&retained_fixture.host,
			&retained_fixture.parent, &g) == -EBUSY);
	CHECK(g == NULL && !sleep_locked);
	simulated_mmc_rescan(&retained_fixture.host);
	CHECK(ordinary_rescan_work == 0);
	CHECK(simulated_sdio_resume(&retained_fixture.host) == 0);
	CHECK(simulated_sdio_suspend(&retained_fixture.host) == 0);
	CHECK(cmd0_count == 0 && cmd5_count == 0 && card_init_count == 0 &&
	      width_change_count == 0);
	retained_fixture.parent.locked = false;

	/* A matching host created after bootstrap inherits the namespace fence. */
	fixture_init(&late_fixture, false);
	CHECK(mmc_compas_sdio_board_off(&late_fixture.host));
	fixture_init(&unrelated_fixture, false);
	unrelated_fixture.parent.name = "md_ingenic,mmc.1";
	unrelated_fixture.host.hostname = "mmc1";
	unrelated_fixture.host.class_dev.name = "mmc1";
	CHECK(!mmc_compas_sdio_board_off(&unrelated_fixture.host));
	simulated_mmc_rescan(&unrelated_fixture.host);
	CHECK(ordinary_rescan_work == 1);
	CHECK(simulated_sdio_resume(&unrelated_fixture.host) == 1);
	CHECK(simulated_sdio_suspend(&unrelated_fixture.host) == 1);
	hardware_power_offs = clock_stops = 0;

	/* First guarded publication adopts this exact host and clears the latch. */
	retained_fixture.parent.registered = true;
	retained_fixture.host.class_dev.registered = true;
	CHECK(mmc_compas_sdio_begin(&retained_fixture.host,
			&retained_fixture.parent, &g) == 0);
	CHECK(mmc_compas_sdio_set_off(g, false) == 0);
	CHECK(!mmc_compas_sdio_board_off(&retained_fixture.host));
	CHECK(!mmc_compas_sdio_board_off(&late_fixture.host));
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_FORCE);
	CHECK(scans_queued == 1 && pending_detect);
	CHECK(mmc_compas_sdio_bootstrap_off() == -EBUSY);
	CHECK(!sleep_locked);

	/* A second matching host cannot replace the adopted host in the registry. */
	fixture_init(&other_fixture, false);
	CHECK(!mmc_compas_sdio_board_off(&other_fixture.host));
	CHECK(mmc_compas_sdio_begin(&other_fixture.host,
			&other_fixture.parent, &other_guard) == 0);
	ret = mmc_compas_sdio_set_off(other_guard, true);
	CHECK(ret == -EBUSY);
	CHECK(hardware_power_offs == 0 && clock_stops == 0);
	mmc_compas_sdio_end(other_guard, MMC_COMPAS_SDIO_DISCARD);

	ordinary_rescan_work = resume_reinitializations = 0;
	cmd0_count = cmd5_count = card_init_count = width_change_count = 0;
	simulated_mmc_rescan(&retained_fixture.host);
	CHECK(ordinary_rescan_work == 1);
	CHECK(simulated_sdio_resume(&retained_fixture.host) == 1);
	CHECK(resume_reinitializations == 1);
	CHECK(cmd0_count == 1 && cmd5_count == 1 && card_init_count == 1 &&
	      width_change_count == 1);
	CHECK(simulated_sdio_suspend(&retained_fixture.host) == 1);
	CHECK(retained_fixture.host.card == &retained_fixture.card);
	CHECK(hardware_power_offs == 0 && clock_stops == 0);

	/* Mark the adopted host off; both retained and no-card paths are fenced. */
	CHECK(mmc_compas_sdio_begin(&retained_fixture.host,
			&retained_fixture.parent, &g) == 0);
	CHECK(mmc_compas_sdio_set_off(g, true) == 0);
	pending_detect = true; /* deterministic queued-work interleaving */
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_DISCARD);
	CHECK(hardware_power_offs == 1 && clock_stops == 1);
	CHECK(detect_cancels == 6 && !pending_detect);
	CHECK(mmc_compas_sdio_board_off(&retained_fixture.host));
	ordinary_rescan_work = resume_reinitializations = 0;
	cmd0_count = cmd5_count = card_init_count = width_change_count = 0;
	simulated_mmc_rescan(&retained_fixture.host);
	CHECK(ordinary_rescan_work == 0);
	CHECK(simulated_sdio_resume(&retained_fixture.host) == 0);
	CHECK(simulated_sdio_suspend(&retained_fixture.host) == 0);
	CHECK(resume_reinitializations == 0 && cmd0_count == 0 && cmd5_count == 0 &&
	      card_init_count == 0 && width_change_count == 0);
	retained_fixture.host.card = NULL;
	simulated_mmc_rescan(&retained_fixture.host);
	CHECK(ordinary_rescan_work == 0);

	/* Clearing the marker restores enumeration for the adopted host. */
	CHECK(mmc_compas_sdio_begin(&retained_fixture.host,
			&retained_fixture.parent, &g) == 0);
	CHECK(mmc_compas_sdio_set_off(g, false) == 0);
	CHECK(!mmc_compas_sdio_board_off(&retained_fixture.host));
	mmc_compas_sdio_end(g, MMC_COMPAS_SDIO_FORCE);
	CHECK(scans_queued == 2 && pending_detect);
	ordinary_rescan_work = resume_reinitializations = 0;
	cmd0_count = cmd5_count = card_init_count = width_change_count = 0;
	simulated_mmc_rescan(&retained_fixture.host);
	CHECK(ordinary_rescan_work == 1);
	CHECK(simulated_sdio_resume(&retained_fixture.host) == 1);
	CHECK(resume_reinitializations == 1);
	CHECK(cmd0_count == 1 && cmd5_count == 1 && card_init_count == 1 &&
	      width_change_count == 1);
	CHECK(simulated_sdio_suspend(&retained_fixture.host) == 1);
	return 0;
}

int main(void)
{
	if (test_begin_failure_paths_restore_owned_state() ||
	    test_bound_func1_fails_and_unwinds() ||
	    test_valid_identity_uses_slot_one_for_function_two() ||
	    test_bad_tuple_and_function_count_unwind() ||
	    test_no_card_rescan_policy_and_discard_drain() ||
	    test_power_errors_are_returned() ||
	    test_off_registry_rescan_pm_and_host_identity())
		return 1;
	puts("actual extracted MMC helper C harness: PASS (7 scenarios)");
	return 0;
}
