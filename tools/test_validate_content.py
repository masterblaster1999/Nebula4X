import json
import tempfile
import unittest
from pathlib import Path

# Import as a module (tools/ is not a package, so adjust sys.path).
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import validate_content  # type: ignore  # noqa: E402


class ValidateContentTests(unittest.TestCase):
    def test_repo_content_passes(self) -> None:
        issues = validate_content.validate_all(REPO_ROOT)
        if issues:
            formatted = "\n".join(i.format(root=REPO_ROOT) for i in issues)
            self.fail(f"Expected no issues, got {len(issues)}:\n{formatted}")

    def test_catches_missing_component_reference(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "data" / "blueprints").mkdir(parents=True)
            (root / "data" / "tech").mkdir(parents=True)

            # Copy baseline content files.
            for rel in [
                Path("data/blueprints/resources.json"),
                Path("data/blueprints/starting_blueprints.json"),
                Path("data/tech/tech_tree.json"),
                Path("data/settings.json"),
            ]:
                src = REPO_ROOT / rel
                dst = root / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(src.read_bytes())

            bp_path = root / "data" / "blueprints" / "starting_blueprints.json"
            bp = json.loads(bp_path.read_text(encoding="utf-8"))
            self.assertIn("designs", bp)
            self.assertGreater(len(bp["designs"]), 0)

            # Inject an invalid component id into the first design.
            bp["designs"][0]["components"].append("definitely_missing_component")
            bp_path.write_text(json.dumps(bp, indent=2, sort_keys=True) + "\n", encoding="utf-8")

            issues = validate_content.validate_all(root)
            self.assertTrue(issues, "Expected validation to fail")
            combined = "\n".join(i.message for i in issues)
            self.assertIn("definitely_missing_component", combined)


    def test_catches_invalid_ship_role(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "data" / "blueprints").mkdir(parents=True)
            (root / "data" / "tech").mkdir(parents=True)

            # Copy baseline content files.
            for rel in [
                Path("data/blueprints/resources.json"),
                Path("data/blueprints/starting_blueprints.json"),
                Path("data/tech/tech_tree.json"),
                Path("data/settings.json"),
            ]:
                src = REPO_ROOT / rel
                dst = root / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(src.read_bytes())

            bp_path = root / "data" / "blueprints" / "starting_blueprints.json"
            bp = json.loads(bp_path.read_text(encoding="utf-8"))
            self.assertIn("designs", bp)
            self.assertGreater(len(bp["designs"]), 0)

            # Inject an invalid role into the first design.
            bp["designs"][0]["role"] = "not_a_real_role"
            bp_path.write_text(json.dumps(bp, indent=2, sort_keys=True) + "\n", encoding="utf-8")

            issues = validate_content.validate_all(root)
            self.assertTrue(issues, "Expected validation to fail")
            combined = "\n".join(i.message for i in issues)
            self.assertIn("unknown ship role", combined)
            self.assertIn("not_a_real_role", combined)
if __name__ == "__main__":
    unittest.main()
