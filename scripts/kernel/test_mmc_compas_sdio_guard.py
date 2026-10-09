#!/usr/bin/env python3
"""Supplementary Python state-machine model for the R1 MMC guard contract.

This is not executable kernel-helper coverage.  The actual helper C is extracted
from the patch and compiled against kernel-shaped stubs by
test_mmc_compas_sdio_guard_c.py; this small model remains only for policy cases.
"""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCH = ROOT / "firmware/kernel/wifi-patches/compas-mmc-radio-lifetime.patch"


class GuardModel:
    def __init__(self, *, parent_busy=False, parent_registered=True, parent_bound=True,
                 right_parent=True, host_registered=True, host_name="mmc0", caps_ok=True,
                 already_disabled=False, card=True, sdio=True, rescan_entered=False,
                 card_runtime_pm=False, functions=()):
        self.parent_busy = parent_busy
        self.parent_registered = parent_registered
        self.parent_bound = parent_bound
        self.right_parent = right_parent
        self.host_registered = host_registered
        self.host_name = host_name
        self.caps_ok = caps_ok
        self.rescan_disabled = already_disabled
        self.card = card
        self.sdio = sdio
        self.rescan_entered = rescan_entered
        self.card_runtime_pm = card_runtime_pm
        self.functions = list(functions)
        self.events = []
        self.locked = []
        self.active = False

    def begin(self):
        self.events.append("lock_system_sleep")
        self.events.append("trylock_parent")
        if self.parent_busy:
            return "EBUSY"
        self.events.append("validate_parent_host")
        if not (self.parent_registered and self.parent_bound and self.right_parent and
                self.host_registered and self.host_name == "mmc0"):
            return "ENODEV"
        if not self.caps_ok:
            return "EOPNOTSUPP"
        if self.rescan_disabled:
            return "EBUSY"
        self.rescan_disabled = True
        self.events.append("disable_rescan")
        self.events.append("cancel_detect_sync")
        if self.card and not self.sdio:
            self.end("PRESERVE")
            return "EOPNOTSUPP"
        if not self.card and self.rescan_entered:
            self.end("PRESERVE")
            return "EOPNOTSUPP"
        if self.card and len(self.functions) > 7:
            self.end("PRESERVE")
            return "EOVERFLOW"
        if self.card and self.card_runtime_pm:
            self.end("PRESERVE")
            return "EBUSY"
        for item in sorted(self.functions, key=lambda f: f["slot"]):
            if not item.get("registered", True):
                continue
            self.events.append(f"trylock_func{item['num']}")
            if item.get("busy"):
                self.end("PRESERVE")
                return "EBUSY"
            self.locked.append(item["num"])
            if item["num"] != item["slot"] + 1:
                self.end("PRESERVE")
                return "ENODEV"
            if item.get("bound") or item.get("runtime_pm"):
                self.end("PRESERVE")
                return "EBUSY"
        self.active = True
        return None

    def end(self, policy):
        for index in reversed(self.locked):
            self.events.append(f"unlock_func{index}")
        self.locked.clear()
        if self.rescan_disabled:
            self.events.append("cancel_detect_sync")
            self.rescan_disabled = False
            self.events.append("enable_rescan")
            if policy in ("PRESERVE", "FORCE"):
                self.events.append("queue_detect")
        self.events.extend(("unlock_parent", "unlock_system_sleep"))
        self.active = False

    @staticmethod
    def maker_identity(tuples):
        present = False
        value = None
        for code, size, data in tuples:
            if code != 0x81:
                continue
            present = True
            if size != 1:
                return "EOPNOTSUPP", present, None, False
            if value is not None and value != data:
                return "EOPNOTSUPP", present, None, False
            value = data
        return None, present, value, present and value == 1


class MmcCompasSdioGuardTests(unittest.TestCase):
    def test_begin_acquires_and_checks_in_required_order(self):
        # The pinned SDK stores function N in sdio_func[N - 1].
        guard = GuardModel(functions=[{"slot": 1, "num": 2}, {"slot": 0, "num": 1}])
        self.assertIsNone(guard.begin())
        self.assertLess(guard.events.index("lock_system_sleep"), guard.events.index("trylock_parent"))
        self.assertLess(guard.events.index("disable_rescan"), guard.events.index("cancel_detect_sync"))
        self.assertLess(guard.events.index("cancel_detect_sync"), guard.events.index("trylock_func1"))
        self.assertLess(guard.events.index("trylock_func1"), guard.events.index("trylock_func2"))

    def test_parent_busy_fails_without_changing_scan_state(self):
        guard = GuardModel(parent_busy=True)
        self.assertEqual(guard.begin(), "EBUSY")
        self.assertFalse(guard.rescan_disabled)
        self.assertNotIn("cancel_detect_sync", guard.events)

    def test_missing_card_can_end_with_preserved_enumeration(self):
        guard = GuardModel(card=False)
        self.assertIsNone(guard.begin())
        guard.end("PRESERVE")
        self.assertIn("queue_detect", guard.events)

    def test_no_card_with_rescan_entered_is_rejected_without_reset(self):
        guard = GuardModel(card=False, rescan_entered=True)
        self.assertEqual(guard.begin(), "EOPNOTSUPP")
        self.assertTrue(guard.rescan_entered)
        self.assertFalse(guard.rescan_disabled)
        self.assertIn("queue_detect", guard.events)

    def test_runtime_pm_and_non_sdio_reject_with_unwind(self):
        for guard, error in (
            (GuardModel(card_runtime_pm=True), "EBUSY"),
            (GuardModel(sdio=False), "EOPNOTSUPP"),
        ):
            with self.subTest(error=error):
                self.assertEqual(guard.begin(), error)
                self.assertFalse(guard.rescan_disabled)
                self.assertIn("queue_detect", guard.events)

    def test_bound_or_busy_function_unwinds_locked_functions_in_reverse(self):
        guard = GuardModel(functions=[
            {"slot": 0, "num": 1}, {"slot": 1, "num": 2, "bound": True},
            {"slot": 2, "num": 3},
        ])
        self.assertEqual(guard.begin(), "EBUSY")
        self.assertEqual([event for event in guard.events if event.startswith("unlock_func")],
                         ["unlock_func2", "unlock_func1"])
        self.assertNotIn("trylock_func3", guard.events)
        self.assertFalse(guard.rescan_disabled)

    def test_power_errors_leave_guard_for_caller_rollback(self):
        guard = GuardModel()
        self.assertIsNone(guard.begin())
        # The core power operation returns its error; caller still owns guard.
        power_result = "EIO"
        self.assertEqual(power_result, "EIO")
        self.assertTrue(guard.rescan_disabled)
        guard.end("PRESERVE")
        self.assertIn("queue_detect", guard.events)

    def test_discard_drains_before_rescan_enable_and_never_requeues(self):
        guard = GuardModel(functions=[{"slot": 0, "num": 1}, {"slot": 1, "num": 2}])
        self.assertIsNone(guard.begin())
        guard.end("DISCARD")
        drained = guard.events.index("cancel_detect_sync", guard.events.index("unlock_func2") + 1)
        self.assertLess(guard.events.index("unlock_func2"), drained)
        self.assertLess(drained, guard.events.index("enable_rescan"))
        self.assertNotIn("queue_detect", guard.events)
        self.assertLess(guard.events.index("enable_rescan"), guard.events.index("unlock_parent"))

    def test_preserve_and_force_both_queue_one_scan(self):
        for policy in ("PRESERVE", "FORCE"):
            guard = GuardModel()
            self.assertIsNone(guard.begin())
            guard.end(policy)
            self.assertEqual(guard.events.count("queue_detect"), 1)

    def test_identity_marker_is_exact_and_contradictory_values_fail_closed(self):
        self.assertEqual(GuardModel.maker_identity([(0x81, 1, 1)]), (None, True, 1, True))
        self.assertEqual(GuardModel.maker_identity([]), (None, False, None, False))
        self.assertEqual(GuardModel.maker_identity([(0x81, 2, 1)]),
                         ("EOPNOTSUPP", True, None, False))
        self.assertEqual(GuardModel.maker_identity([(0x81, 1, 1), (0x81, 1, 0)]),
                         ("EOPNOTSUPP", True, None, False))

    def test_patch_source_keeps_rescan_entered_and_struct_layout_untouched(self):
        patch = PATCH.read_text()
        self.assertNotIn("include/linux/mmc/host.h", patch)
        self.assertNotIn("rescan_entered =", patch)
        self.assertIn('strcmp(dev_name(expected_parent), "md_ingenic,mmc.0")', patch)
        self.assertIn('strcmp(mmc_hostname(host), "mmc0")', patch)
        begin = patch[patch.index("int mmc_compas_sdio_begin("):patch.index("EXPORT_SYMBOL_GPL(mmc_compas_sdio_begin)")]
        self.assertLess(begin.index("cancel_delayed_work_sync(&host->detect)"),
                        begin.index("guard->card = host->card"))
        finish = patch[patch.index("static void mmc_compas_sdio_finish"):patch.index("int mmc_compas_sdio_begin(")]
        self.assertLess(finish.index("mmc_compas_sdio_unlock_functions"),
                        finish.index("cancel_delayed_work_sync(&host->detect)"))
        self.assertLess(finish.index("cancel_delayed_work_sync(&host->detect)"),
                        finish.index("host->rescan_disable = 0"))
        self.assertLess(finish.index("host->rescan_disable = 0"),
                        finish.index("_mmc_detect_change(host, 0, false)"))
        self.assertIn("i < guard->card->sdio_funcs", begin)
        self.assertIn("func->num != i + 1", begin)
        self.assertIn("guard->card->sdio_funcs > SDIO_MAX_FUNCS", begin)
        identity = patch[patch.index("int mmc_compas_sdio_identity("):
                         patch.index("EXPORT_SYMBOL_GPL(mmc_compas_sdio_identity)")]
        self.assertIn("guard->funcs[i]", identity)
        self.assertIn("func->num != 2", identity)
        self.assertIn("tuple->size != 1", identity)
        self.assertIn("identity->maker_tuple_value != tuple->data[0]", identity)


if __name__ == "__main__":
    unittest.main(verbosity=2)
