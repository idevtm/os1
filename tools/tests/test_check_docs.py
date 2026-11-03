from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.check_docs import check_repository


class DocumentationCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "doc/exec-plans/active").mkdir(parents=True)
        (self.root / "doc/exec-plans/completed").mkdir(parents=True)
        self.write("AGENTS.md", "# Agents\n")
        self.write("README.md", "# Readme\n")
        self.write("GOALS.md", "# Goals\n")
        self.write("doc/ARCHITECTURE.md", "# Architecture\n")
        self.write("doc/REFERENCES.md", "# References\n")
        self.write("doc/latest-review.md", "# Review\n")
        metadata = (
            "> Status: active\n"
            "> Owner: repository maintainers\n"
            "> Last verified: 2026-08-19 at `1234567`\n"
        )
        self.write("doc/QUALITY.md", f"# Quality\n\n{metadata}")
        self.write("doc/DEPENDENCIES.md", f"# Dependencies\n\n{metadata}")
        self.write("doc/AUTONOMY.md", f"# Autonomy\n\n{metadata}")
        self.write(
            "doc/TECH_DEBT.md",
            f"# Debt\n\n{metadata}\n"
            "## TD-001 — Example\n\n"
            "- **Status:** open\n"
            "- **Owner:** kernel/mm\n"
            "- **Impact:** Example impact.\n"
            "- **Evidence:** Example evidence.\n"
            "- **Prerequisite:** Example prerequisite.\n"
            "- **Next action:** Example action.\n",
        )
        self.write("doc/history.md", "# History\n")
        self.write_index()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, relative: str, contents: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def write_index(self, extra: str = "") -> None:
        self.write(
            "doc/README.md",
            "\n".join(
                (
                    "# Index",
                    "[Architecture](ARCHITECTURE.md)",
                    "[References](REFERENCES.md)",
                    "[Review](latest-review.md)",
                    "[Quality](QUALITY.md)",
                    "[Debt](TECH_DEBT.md)",
                    "[Dependencies](DEPENDENCIES.md)",
                    "[Autonomy](AUTONOMY.md)",
                    "[History](history.md)",
                    extra,
                    "",
                )
            ),
        )

    def errors(self) -> list[str]:
        errors, _ = check_repository(self.root)
        return errors

    def test_accepts_complete_index_and_live_links(self) -> None:
        self.write("README.md", "[Architecture](doc/ARCHITECTURE.md)\n")
        self.assertEqual([], self.errors())

    def test_rejects_broken_live_link(self) -> None:
        self.write("README.md", "[Missing](doc/missing.md)\n")
        self.assertTrue(any("missing link target" in error for error in self.errors()))

    def test_rejects_unindexed_document(self) -> None:
        self.write("doc/new.md", "# New\n")
        self.assertTrue(any("unindexed document 'doc/new.md'" in error for error in self.errors()))

    def test_rejects_link_that_escapes_repository(self) -> None:
        self.write("README.md", "[Outside](../outside.md)\n")
        self.assertTrue(any("escapes the repository" in error for error in self.errors()))

    def test_rejects_malformed_url_without_crashing(self) -> None:
        self.write("README.md", "[Malformed](http://[)\n")
        self.assertTrue(any("invalid URL syntax" in error for error in self.errors()))

    def test_rejects_percent_encoded_control_character(self) -> None:
        self.write("README.md", "[Control](doc/%00.md)\n")
        self.assertTrue(any("control character" in error for error in self.errors()))

    def test_rejects_active_plan_without_concrete_metadata(self) -> None:
        self.write(
            "doc/exec-plans/active/plan.md",
            "> Status: active\n> Owner: <owner>\n> Last verified: someday\n",
        )
        self.write_index("[Plan](exec-plans/active/plan.md)")
        errors = self.errors()
        self.assertTrue(any("missing concrete owner" in error for error in errors))
        self.assertTrue(any("invalid last-verified" in error for error in errors))

    def test_accepts_well_formed_active_plan(self) -> None:
        self.write(
            "doc/exec-plans/active/plan.md",
            "> Status: active\n> Owner: kernel/mm\n> Last verified: 2026-08-19 at `1234567`\n",
        )
        self.write_index("[Plan](exec-plans/active/plan.md)")
        self.assertEqual([], self.errors())

    def test_rejects_debt_item_missing_required_field(self) -> None:
        path = self.root / "doc/TECH_DEBT.md"
        path.write_text(
            path.read_text(encoding="utf-8").replace("- **Next action:** Example action.\n", ""),
            encoding="utf-8",
        )
        self.assertTrue(any("TD-001 must contain exactly one non-empty Next action" in error
                            for error in self.errors()))

    def test_rejects_nonsequential_debt_ids(self) -> None:
        path = self.root / "doc/TECH_DEBT.md"
        path.write_text(
            path.read_text(encoding="utf-8").replace("TD-001", "TD-002"),
            encoding="utf-8",
        )
        self.assertTrue(any("debt IDs must be unique and sequential" in error
                            for error in self.errors()))

    def test_rejects_malformed_debt_heading(self) -> None:
        path = self.root / "doc/TECH_DEBT.md"
        path.write_text(
            path.read_text(encoding="utf-8").replace("TD-001", "TD-01", 1),
            encoding="utf-8",
        )
        self.assertTrue(any("canonical TD heading" in error for error in self.errors()))

    def test_rejects_missing_live_contract(self) -> None:
        (self.root / "doc/QUALITY.md").unlink()
        self.assertTrue(any("missing live document 'doc/QUALITY.md'" in error
                            for error in self.errors()))


if __name__ == "__main__":
    unittest.main()
