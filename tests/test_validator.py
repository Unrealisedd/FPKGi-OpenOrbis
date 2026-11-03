from __future__ import annotations

import importlib.util
import json
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
        VALIDATOR.validate_document({"DATA": {"http://lan.example/test.pkg": entry()}})

    def test_malformed_metadata_is_rejected(self) -> None:
        invalid_documents = [
            {},
            {"DATA": [], "extra": True},
            {"DATA": {"https://example.test/a.pkg": entry(title_id="bad")}},
            {"DATA": {"https://example.test/a.pkg": entry(size=32)}},
            {"DATA": {"https://example.test/a.pkg": entry(size="0")}},
            {"DATA": {"https://example.test/a.pkg": entry(size=str(1 << 64))}},
            {"DATA": {"https://user@example.test/a.pkg": entry()}},
            {"DATA": {"https://example.test/a.pkg#fragment": entry()}},
            {"DATA": {"https://example.test:/a.pkg": entry()}},
            {"DATA": {"https://example..test/a.pkg": entry()}},
            {"DATA": {"https://example.test/bad%2.pkg": entry()}},
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

    def test_uppercase_sha_is_accepted(self) -> None:
        VALIDATOR.validate_document({"DATA": {
            "https://example.test/a.pkg": entry(sha256="A" * 64),
        }})


if __name__ == "__main__":
    unittest.main()
