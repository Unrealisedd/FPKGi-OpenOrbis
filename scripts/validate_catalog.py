#!/usr/bin/env python3
"""Validate the bounded FPKGi Native DATA-map catalog contract."""
from __future__ import annotations

import ipaddress
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

TITLE_ID_RE = re.compile(r"^[A-Z]{4}[0-9]{5}$")
VERSION_RE = re.compile(r"^(?!\.$)(?!.*\.\.)[A-Za-z0-9._-]+$")
SHA_RE = re.compile(r"^[a-fA-F0-9]{64}$")
DECIMAL_RE = re.compile(r"^[0-9]+$")
MAX_UINT64 = (1 << 64) - 1
MAX_CATALOG_BYTES = 2 * 1024 * 1024
MAX_URL = 2048
MAX_ITEMS = 4096
MAX_JSON_DEPTH = 16
MAX_JSON_NODES = 65536
ALLOWED_FIELDS = {
    "title_id",
    "region",
    "name",
    "version",
    "release",
    "size",
    "min_fw",
    "cover_url",
    "sha256",
}
OPTIONAL_TEXT_LIMITS = {"region": 16, "release": 32, "min_fw": 16}


def fail(message: str) -> None:
    raise ValueError(message)


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON object key: {key}")
        result[key] = value
    return result

def enforce_json_limits(document: Any) -> None:
    nodes = 0
    stack: list[tuple[Any, int]] = [(document, 0)]
    while stack:
        value, parent_depth = stack.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES:
            fail(f"JSON contains more than {MAX_JSON_NODES} values")
        if isinstance(value, (dict, list)):
            depth = parent_depth + 1
            if depth > MAX_JSON_DEPTH:
                fail(f"JSON nesting exceeds {MAX_JSON_DEPTH} container levels")
            children = value.values() if isinstance(value, dict) else value
            stack.extend((child, depth) for child in children)


def bounded_text(value: Any, label: str, maximum: int, *, nullable: bool = False) -> str | None:
    if value is None and nullable:
        return None
    if not isinstance(value, str):
        fail(f"{label} must be a string" + (" or null" if nullable else ""))
    if not value or len(value.encode("utf-8")) > maximum or all(character in " \t" for character in value):
        fail(f"{label} must be 1-{maximum} UTF-8 bytes")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        fail(f"{label} contains a control character")
    return value


def safe_http_url(value: Any, label: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= MAX_URL:
        fail(f"{label} must be a 1-{MAX_URL} character URL")
    if "\\" in value or any(ord(character) <= 0x20 or ord(character) > 0x7E for character in value):
        fail(f"{label} contains whitespace, a non-ASCII character, or a backslash")
    for match in re.finditer("%", value):
        if not re.fullmatch(r"[0-9A-Fa-f]{2}", value[match.start() + 1:match.start() + 3]):
            fail(f"{label} contains an invalid percent escape")
    try:
        parsed = urlsplit(value)
        port = parsed.port
        hostname = parsed.hostname
    except ValueError as exc:
        fail(f"{label} is malformed: {exc}")
    if parsed.scheme.lower() not in {"http", "https"} or not parsed.netloc or hostname is None:
        fail(f"{label} must be an absolute HTTP or HTTPS URL with an authority")
    if parsed.username is not None or parsed.password is not None or "@" in parsed.netloc:
        fail(f"{label} must not contain credentials")
    if "#" in value:
        fail(f"{label} must not contain a fragment")
    if parsed.netloc.endswith(":"):
        fail(f"{label} has an empty port")
    try:
        if ":" in hostname:
            if ipaddress.ip_address(hostname).version != 6:
                fail(f"{label} has an invalid IPv6 host")
        elif hostname.replace(".", "").isdigit():
            if ipaddress.ip_address(hostname).version != 4:
                fail(f"{label} has an invalid IPv4 host")
        else:
            labels = hostname.split(".")
            if len(hostname) > 253 or any(
                not 1 <= len(part) <= 63
                or not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?", part)
                for part in labels
            ):
                fail(f"{label} host is malformed")
    except ValueError as exc:
        fail(f"{label} host is malformed: {exc}")
    if port is not None and not 1 <= port <= 65535:
        fail(f"{label} has an invalid port")
    return value


def parse_size(value: Any, label: str) -> int:
    if not isinstance(value, str) or len(value) > 20 or not DECIMAL_RE.fullmatch(value):
        fail(f"{label} must be a nonzero uint64 decimal string")
    parsed = int(value, 10)
    if parsed == 0 or parsed > MAX_UINT64:
        fail(f"{label} must be a nonzero uint64 decimal string")
    return parsed


def validate_document(document: Any) -> None:
    enforce_json_limits(document)
    if not isinstance(document, dict) or set(document) != {"DATA"}:
        fail("catalog root must contain only DATA")
    data = document["DATA"]
    if not isinstance(data, dict):
        fail("DATA must be an object keyed by package URL")
    if len(data) > MAX_ITEMS:
        fail(f"DATA contains more than {MAX_ITEMS} packages")

    identities: set[tuple[str, str]] = set()
    for package_url, metadata in data.items():
        prefix = f"DATA[{package_url!r}]"
        safe_http_url(package_url, f"{prefix} package URL")
        if not isinstance(metadata, dict):
            fail(f"{prefix} must be an object")
        unknown = set(metadata) - ALLOWED_FIELDS
        if unknown:
            fail(f"{prefix} has unknown fields: {sorted(unknown)}")
        for required in ("title_id", "name", "version", "size"):
            if required not in metadata:
                fail(f"{prefix}.{required} is required")

        title_id = bounded_text(metadata["title_id"], f"{prefix}.title_id", 9)
        if not TITLE_ID_RE.fullmatch(title_id):
            fail(f"{prefix}.title_id must match [A-Z]{{4}}[0-9]{{5}}")
        name = bounded_text(metadata["name"], f"{prefix}.name", 96)
        version = bounded_text(metadata["version"], f"{prefix}.version", 16)
        if not VERSION_RE.fullmatch(version):
            fail(f"{prefix}.version may not be '.' and may contain only letters, digits, '.', '-', and '_' without '..'")
        parse_size(metadata["size"], f"{prefix}.size")

        for field, limit in OPTIONAL_TEXT_LIMITS.items():
            if field in metadata:
                bounded_text(metadata[field], f"{prefix}.{field}", limit, nullable=True)
        if "cover_url" in metadata and metadata["cover_url"] is not None:
            safe_http_url(metadata["cover_url"], f"{prefix}.cover_url")
        if "sha256" in metadata:
            sha256 = metadata["sha256"]
            if not isinstance(sha256, str) or not SHA_RE.fullmatch(sha256):
                fail(f"{prefix}.sha256 must be 64 hexadecimal characters")
        if package_url.lower().startswith("http://") and "sha256" not in metadata:
            fail(f"{prefix}.sha256 is required for an HTTP package URL")

        identity = (title_id, version)
        if identity in identities:
            fail(f"duplicate or conflicting title_id/version identity: {title_id} {version}")
        identities.add(identity)


def validate(path: Path) -> None:
    with path.open("rb") as file:
        payload = file.read(MAX_CATALOG_BYTES + 1)
    if len(payload) > MAX_CATALOG_BYTES:
        fail(f"catalog exceeds {MAX_CATALOG_BYTES} bytes")
    document = json.loads(payload.decode("utf-8"), object_pairs_hook=reject_duplicate_keys)
    validate_document(document)


if __name__ == "__main__":
    target = Path(sys.argv[1] if len(sys.argv) > 1 else "catalog/catalog.json")
    try:
        validate(target)
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError, ValueError) as exc:
        print(f"catalog validation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
    print(f"catalog OK: {target}")
