from __future__ import annotations

import importlib.util
import json
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("validate_catalog", ROOT / "scripts" / "validate_catalog.py")
assert SPEC and SPEC.loader
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def entry(**overrides: object) -> dict[str, object]:
    value: dict[str, object] = {
        "title_id": "FPKG00001",
        "name": "Owner Supplied Homebrew",
        "version": "01.00",
        "size": "32",
        "cover_url": None,
    }
    value.update(overrides)
    return value


class CatalogValidatorTests(unittest.TestCase):
    def test_empty_and_valid_catalogs(self) -> None:
        VALIDATOR.validate_document({"DATA": {}})
        VALIDATOR.validate_document({"DATA": {
            "http://lan.example/test.pkg": entry(sha256="0" * 64),
        }})
        VALIDATOR.validate_document({"DATA": {
            "https://example.test/unicode.pkg": entry(name="A" * 92 + "\U0001F600"),
        }})

    def test_malformed_metadata_is_rejected(self) -> None:
        invalid_documents = [
            {},
            {"DATA": [], "extra": True},
            {"DATA": {"https://example.test/a.pkg": entry(title_id="bad")}},
            {"DATA": {"https://example.test/a.pkg": entry(size=32)}},
            {"DATA": {"https://example.test/a.pkg": entry(size="0")}},
            {"DATA": {"https://example.test/a.pkg": entry(size=str(1 << 64))}},
            {"DATA": {"https://example.test/a.pkg": entry(name="A" * 93 + "\U0001F600")}},
            {"DATA": {"https://example.test/a.pkg": entry(version=".")}},
            {"DATA": {"http://lan.example/a.pkg": entry()}},
            {"DATA": {"https://user@example.test/a.pkg": entry()}},
            {"DATA": {"https://example.test/a.pkg#fragment": entry()}},
            {"DATA": {"https://example.test:/a.pkg": entry()}},
            {"DATA": {"https://example..test/a.pkg": entry()}},
            {"DATA": {"https://example.test/bad%2.pkg": entry()}},
            {"DATA": {"https://010.000.000.001/a.pkg": entry()}},
            {"DATA": {"https://127.0.0.1./a.pkg": entry()}},
            {"DATA": {"https://example.test/a.pkg": entry(cover_url="//example.test/cover.png")}},
            {"DATA": {"https://example.test/a.pkg": entry(unknown=True)}},
            {"DATA": {"https://example.test/a.pkg": entry(sha256="1234")}},
            {"DATA": {
                "https://example.test/a.pkg": entry(),
                "https://example.test/b.pkg": entry(name="Conflicting identity"),
            }},
        ]
        for document in invalid_documents:
            with self.subTest(document=document):
                with self.assertRaises(ValueError):
                    VALIDATOR.validate_document(document)

    def test_duplicate_json_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.json"
            path.write_text('{"DATA":{},"DATA":{}}', encoding="utf-8")
            with self.assertRaises(ValueError):
                VALIDATOR.validate(path)

    def test_file_size_and_json_whitespace_limits(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.json"
            path.write_bytes(b" " * (VALIDATOR.MAX_CATALOG_BYTES + 1))
            with self.assertRaisesRegex(ValueError, "exceeds"):
                VALIDATOR.validate(path)
            for whitespace in (b"\x0c", b"\x0b"):
                with self.subTest(whitespace=whitespace):
                    path.write_bytes(b"{" + whitespace + b'"DATA":{}}')
                    with self.assertRaises(json.JSONDecodeError):
                        VALIDATOR.validate(path)

    def test_version_schema_matches_runtime(self) -> None:
        schema = json.loads((ROOT / "catalog" / "schema.json").read_text(encoding="utf-8"))
        pattern = schema["$defs"]["package"]["properties"]["version"]["pattern"]
        for value in ("1", ".1", "1_", "1-"):
            with self.subTest(valid=value):
                self.assertIsNotNone(re.fullmatch(pattern, value))
        for value in (".", "..", "1..0"):
            with self.subTest(invalid=value):
                self.assertIsNone(re.fullmatch(pattern, value))

    def test_uint64_schema_pattern_matches_runtime_bounds(self) -> None:
        schema = json.loads((ROOT / "catalog" / "schema.json").read_text(encoding="utf-8"))
        pattern = schema["$defs"]["package"]["properties"]["size"]["pattern"]
        for value in ("1", "0001", "9999999999999999999", str((1 << 64) - 1)):
            with self.subTest(valid=value):
                self.assertIsNotNone(re.fullmatch(pattern, value))
        for value in ("0", str(1 << 64), "99999999999999999999"):
            with self.subTest(invalid=value):
                self.assertIsNone(re.fullmatch(pattern, value))

    def test_catalog_item_limit_is_enforced(self) -> None:
        oversized = {
            f"https://example.test/{index}.pkg": {}
            for index in range(VALIDATOR.MAX_ITEMS + 1)
        }
        with self.assertRaises(ValueError):
            VALIDATOR.validate_document({"DATA": oversized})

    def test_json_resource_limits_are_enforced(self) -> None:
        nested: object = 0
        for _ in range(VALIDATOR.MAX_JSON_DEPTH + 1):
            nested = [nested]
        with self.assertRaises(ValueError):
            VALIDATOR.validate_document({"DATA": nested})
        with self.assertRaises(ValueError):
            VALIDATOR.validate_document({
                "DATA": [None] * VALIDATOR.MAX_JSON_NODES,
            })

    def test_uppercase_sha_is_accepted(self) -> None:
        VALIDATOR.validate_document({"DATA": {
            "https://example.test/a.pkg": entry(sha256="A" * 64),
        }})


if __name__ == "__main__":
    unittest.main()
