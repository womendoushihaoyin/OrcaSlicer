#!/usr/bin/env python3
"""Fixture/verifier contracts only; these are not native host execution evidence."""
import tempfile
import unittest
import zipfile
from pathlib import Path
import test_plugin_wheel_cache_recovery as driver


class WheelRecoveryContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture = self.root / "fixtures"
        driver.create_fixtures(self.fixture)

    def wheel(self, scenario, state="bad"):
        return next((self.fixture / scenario / state).glob("*.whl"))

    def success(self, legacy=False, restart=False):
        scenario = "entry_restart" if restart else "entry_legacy" if legacy else "entry_crc"
        value = "ORIGINAL" if legacy else "COMPLETE"
        good = {"cross_process_complete_cache_reuse": "UPDATED"} if restart else {
            "legacy_unmarked_cache_recovery" if legacy else "good_archive_repair": value,
            "complete_cache_reuse": value, "truncated_cached_resource_recovery": value,
            "invalid_completion_record_recovery": value, "missing_cached_resource_recovery": value,
            "changed_archive_recovery": "UPDATED"}
        bad = [] if legacy or restart else ["first_corrupt_load", "same_corrupt_retry", "corrupt_replacement_rejected"]
        if bad:
            good["restored_archive_reuse"] = value
        observations = [dict(phase=phase, loaded=True, actual_callback_data=payload, error="",
                             cache_exists=True, cache_files={".orca-wheel-cache.json": 50})
                        for phase, payload in good.items()]
        observations += [dict(phase=phase, loaded=False, error="real error", cache_exists=phase == "corrupt_replacement_rejected")
                         for phase in bad]
        return dict(scenario=scenario, passed=True, interpreter_stopped=True, failures=[], observations=observations)

    def test_fixture_bytes_are_deterministic(self):
        other = self.root / "second"
        driver.create_fixtures(other)
        self.assertEqual((self.fixture / "manifest.json").read_bytes(), (other / "manifest.json").read_bytes())

    def test_crc_fault_is_late_and_metadata_is_readable(self):
        for kind, module in (("entry", "cacheprobe"), ("dependency", "cachedep")):
            with zipfile.ZipFile(self.wheel(kind + "_crc")) as archive:
                self.assertEqual(archive.read(module + "/resource.txt"), b"ORIGINAL")
                self.assertTrue(archive.read(module + "-1.0.0.dist-info/RECORD"))
                with self.assertRaises(zipfile.BadZipFile):
                    archive.read(module + "/tail.bin")

    def test_path_duplicate_and_conflict_are_real_zip_entries(self):
        with zipfile.ZipFile(self.wheel("entry_unsafe")) as archive:
            self.assertIn("../escaped.txt", archive.namelist())
        with zipfile.ZipFile(self.wheel("entry_duplicate")) as archive:
            self.assertEqual(archive.namelist().count("cacheprobe/resource.txt"), 2)
        with zipfile.ZipFile(self.wheel("entry_conflict")) as archive:
            self.assertIn("cacheprobe/block", archive.namelist())
            self.assertIn("cacheprobe/block/child", archive.namelist())

    def test_complete_fixtures_and_all_cases(self):
        self.assertEqual(len(driver.CASES), 11)
        for case in driver.CASES:
            for state in ("good", "updated"):
                with zipfile.ZipFile(self.wheel(case, state)) as archive:
                    self.assertIsNone(archive.testzip())

    def test_accepts_recovery_legacy_and_restart_shapes(self):
        for options in ({}, {"legacy": True}, {"restart": True}):
            data = self.success(**options)
            driver.verify_observation(data["scenario"], data)

    def test_rejects_fallback_import_and_missing_phase(self):
        data = self.success()
        bad = next(item for item in data["observations"] if item["phase"] == "same_corrupt_retry")
        bad["loaded"] = True
        bad["actual_callback_data"] = "ORIGINAL"
        with self.assertRaises(ValueError):
            driver.verify_observation("entry_crc", data)
        data = self.success()
        data["observations"].pop()
        with self.assertRaises(ValueError):
            driver.verify_observation("entry_crc", data)

    def test_rejects_bad_callback_record_or_shutdown(self):
        for field, value in (("actual_callback_data", "MISSING"), ("cache_files", {})):
            data = self.success()
            data["observations"][0][field] = value
            with self.assertRaises(ValueError):
                driver.verify_observation("entry_crc", data)
        data = self.success()
        data["interpreter_stopped"] = False
        with self.assertRaises(ValueError):
            driver.verify_observation("entry_crc", data)

    def test_case_alias_uses_actual_volume_probe(self):
        data = self.success()
        data["scenario"] = "entry_case_alias"
        with self.assertRaises(ValueError):
            driver.verify_observation("entry_case_alias", data)
        data["filesystem_case_insensitive"] = True
        driver.verify_observation("entry_case_alias", data)
        data["filesystem_case_insensitive"] = False
        data["observations"] = [item for item in data["observations"] if item["phase"] not in
                                ("first_corrupt_load", "same_corrupt_retry", "corrupt_replacement_rejected", "restored_archive_reuse")]
        data["observations"].append(dict(phase="distinct_case_sensitive_entries", loaded=True, error="",
                                         actual_callback_data="ORIGINAL", cache_exists=True,
                                         cache_files={".orca-wheel-cache.json": 50}))
        driver.verify_observation("entry_case_alias", data)

    def test_refuses_existing_fixture_directory(self):
        with self.assertRaises(FileExistsError):
            driver.create_fixtures(self.fixture)
        self.assertEqual(len(list(self.fixture.glob("*/*.py"))), 11)


if __name__ == "__main__":
    unittest.main()
