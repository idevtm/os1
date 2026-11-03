from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.check_architecture import (
    RULE_BOOT,
    RULE_COMMON,
    RULE_KERNEL,
    RULE_USER,
    Violation,
    compare_with_baseline,
    load_baseline,
    scan_violations,
)


class ArchitectureCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for relative in (
            "src/common/elf",
            "src/user/programs",
            "src/kernel/core",
            "src/kernel/handoff",
            "src/kernel/arch",
            "src/kernel/util",
            "src/boot/limine",
            "src/uapi/os1",
        ):
            (self.root / relative).mkdir(parents=True, exist_ok=True)
        (self.root / "src/kernel/core/internal.hpp").write_text("#pragma once\n", encoding="utf-8")
        (self.root / "src/kernel/handoff/boot_info.hpp").write_text("#pragma once\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, relative: str, contents: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def violations(self) -> set[Violation]:
        violations, _, _ = scan_violations(self.root)
        return set(violations)

    def test_detects_common_to_kernel_dependency(self) -> None:
        self.write("src/common/elf/file.hpp", '#include "core/internal.hpp"\n')
        self.assertIn(
            Violation(RULE_COMMON, "src/common/elf/file.hpp", "core/internal.hpp"),
            self.violations(),
        )

    def test_detects_user_to_kernel_dependency(self) -> None:
        self.write("src/user/programs/app.cpp", '#include "core/internal.hpp"\n')
        self.assertIn(
            Violation(RULE_USER, "src/user/programs/app.cpp", "core/internal.hpp"),
            self.violations(),
        )

    def test_detects_kernel_to_boot_vendor_dependency(self) -> None:
        self.write("src/kernel/core/start.cpp", '#include "limine.h"\n')
        self.assertIn(
            Violation(RULE_KERNEL, "src/kernel/core/start.cpp", "limine.h"),
            self.violations(),
        )

    def test_detects_kernel_to_bios_dependency_from_boot_include_root(self) -> None:
        self.write("src/boot/bios/private.hpp", "#pragma once\n")
        self.write("src/kernel/core/start.cpp", '#include "bios/private.hpp"\n')
        self.assertIn(
            Violation(RULE_KERNEL, "src/kernel/core/start.cpp", "bios/private.hpp"),
            self.violations(),
        )

    def test_allows_boot_handoff_dependency(self) -> None:
        self.write("src/boot/limine/entry.cpp", '#include "handoff/boot_info.hpp"\n')
        self.assertEqual(set(), self.violations())

    def test_detects_boot_to_kernel_core_dependency(self) -> None:
        self.write("src/boot/limine/entry.cpp", '#include "core/internal.hpp"\n')
        self.assertIn(
            Violation(RULE_BOOT, "src/boot/limine/entry.cpp", "core/internal.hpp"),
            self.violations(),
        )

    def test_baseline_parser_rejects_parent_traversal(self) -> None:
        baseline = self.root / "baseline.txt"
        baseline.write_text(f"{RULE_USER}|../outside.cpp|core/internal.hpp\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "normalized repository path"):
            load_baseline(baseline)

    def test_ratchet_reports_new_and_stale_entries(self) -> None:
        current = {Violation(RULE_USER, "src/user/new.cpp", "core/internal.hpp")}
        baseline = {Violation(RULE_USER, "src/user/old.cpp", "core/internal.hpp")}
        new, stale = compare_with_baseline(current, baseline)
        self.assertEqual(current, new)
        self.assertEqual(baseline, stale)


if __name__ == "__main__":
    unittest.main()
