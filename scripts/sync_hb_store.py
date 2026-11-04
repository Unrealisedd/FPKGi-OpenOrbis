#!/usr/bin/env python3
"""Convert the permitted official HB-Store database into FPKGi's bounded catalog."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sqlite3
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
from validate_catalog import (  # noqa: E402
    MAX_CATALOG_BYTES,
    MAX_UINT64,
    reject_duplicate_keys,
    validate_document,
)

STORE_DB_URL = "https://api.pkg-zone.com/store.db"
STORE_HASH_URL = "https://api.pkg-zone.com/api.php?db_check_hash=true"
PACKAGE_URL_PREFIX = "https://api.pkg-zone.com/download.php?tid="
COVER_URL_PREFIX = "https://api.pkg-zone.com/images/"
USER_AGENT = "StoreHAX/GL-0x14000000 FPKGi-Native-Catalog/1.03"
MAX_DATABASE_BYTES = 8 * 1024 * 1024
MAX_HASH_BYTES = 1024
MAX_STATE_BYTES = 2 * 1024 * 1024
MAX_SOURCE_ROWS = 512
PUBLISHED_STATE_SCHEMA = 1
WORK_STATE_SCHEMA = 2
WORK_STATE_KIND = "probe-work"
WORK_CACHE_MAX_AGE_SECONDS = 2 * 60 * 60
REQUEST_TIMEOUT_SECONDS = 25
MIN_LIVE_REQUEST_DELAY_SECONDS = 1.0
PKG_MAGIC = b"\x7fCNT"
TITLE_ID_RE = re.compile(r"^[A-Z]{4}[0-9]{5}$")
VERSION_RE = re.compile(r"^(?!\.$)(?!.*\.\.)[A-Za-z0-9._-]+$")
MD5_RE = re.compile(r"^[a-fA-F0-9]{32}$")
DATE_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
CONTENT_RANGE_RE = re.compile(r"^bytes 0-3/([1-9][0-9]{0,19})$")
REQUIRED_COLUMNS = {
    "pid",
    "id",
    "name",
    "package",
    "version",
    "Size",
    "Author",
    "apptype",
    "pv",
    "releaseddate",
    "md5",
}


class SyncError(ValueError):
    """A fail-closed source, policy, or output validation error."""


@dataclass(frozen=True)
class SourceRow:
    title_id: str
    name: str
    version: str
    package: str
    source_md5: str | None
    app_type: str
    platform_marker: str
    release: str | None


@dataclass(frozen=True)
class Policy:
    allowed_app_types: frozenset[str]
    allowed_title_ids: frozenset[str]
    denied_title_ids: frozenset[str]
    denied_title_prefixes: tuple[str, ...]
    denied_platform_markers: frozenset[str]


@dataclass
class CacheState:
    source_db_md5: str
    packages: dict[str, dict[str, Any]]


class RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str,
                         headers: Any, newurl: str) -> None:
        raise SyncError(f"redirects are not allowed: {req.full_url} -> {newurl}")


class RequestLimiter:
    def __init__(self, delay_seconds: float, sleeper: Callable[[float], None] = time.sleep) -> None:
        self.delay_seconds = delay_seconds
        self.sleeper = sleeper
        self.last_request: float | None = None

    def wait(self) -> None:
        now = time.monotonic()
        if self.last_request is not None:
            remaining = self.delay_seconds - (now - self.last_request)
            if remaining > 0:
                self.sleeper(remaining)
        self.last_request = time.monotonic()


def _bounded_text(value: Any, label: str, maximum: int, *, nullable: bool = False) -> str | None:
    if value is None and nullable:
        return None
    if not isinstance(value, str):
        raise SyncError(f"{label} must be a string" + (" or null" if nullable else ""))
    encoded = value.encode("utf-8")
    if not encoded or len(encoded) > maximum or not value.strip(" \t"):
        raise SyncError(f"{label} must be 1-{maximum} UTF-8 bytes")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise SyncError(f"{label} contains a control character")
    return value


def _read_bounded_file(path: Path, maximum: int) -> bytes:
    with path.open("rb") as source:
        payload = source.read(maximum + 1)
    if len(payload) > maximum:
        raise SyncError(f"{path} exceeds {maximum} bytes")
    return payload


def _json_object(path: Path, maximum: int) -> dict[str, Any]:
    try:
        document = json.loads(
            _read_bounded_file(path, maximum).decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise SyncError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise SyncError(f"{path} root must be an object")
    return document


def load_policy(path: Path) -> Policy:
    document = _json_object(path, 64 * 1024)
    expected = {
        "allowed_app_types",
        "allowed_title_ids",
        "denied_title_ids",
        "denied_title_prefixes",
        "denied_platform_markers",
    }
    if set(document) != expected:
        raise SyncError(f"{path} must contain exactly {sorted(expected)}")

    def string_set(key: str, *, title_ids: bool = False) -> frozenset[str]:
        values = document[key]
        if not isinstance(values, list) or not values:
            raise SyncError(f"{key} must be a nonempty array")
        normalized: set[str] = set()
        for index, raw in enumerate(values):
            value = _bounded_text(raw, f"{key}[{index}]", 64)
            assert value is not None
            if title_ids:
                if not TITLE_ID_RE.fullmatch(value):
                    raise SyncError(f"{key}[{index}] is not a title ID")
                normalized.add(value)
            else:
                normalized.add(value.strip().casefold())
        if len(normalized) != len(values):
            raise SyncError(f"{key} contains duplicates")
        return frozenset(normalized)

    raw_prefixes = document["denied_title_prefixes"]
    if not isinstance(raw_prefixes, list) or not raw_prefixes:
        raise SyncError("denied_title_prefixes must be a nonempty array")
    prefixes: list[str] = []
    for index, raw in enumerate(raw_prefixes):
        value = _bounded_text(raw, f"denied_title_prefixes[{index}]", 4)
        assert value is not None
        if not re.fullmatch(r"[A-Z]{1,4}", value):
            raise SyncError(f"denied_title_prefixes[{index}] must contain uppercase letters")
        prefixes.append(value)
    if len(set(prefixes)) != len(prefixes):
        raise SyncError("denied_title_prefixes contains duplicates")

    allowed_ids = string_set("allowed_title_ids", title_ids=True)
    denied_ids = string_set("denied_title_ids", title_ids=True)
    if allowed_ids & denied_ids:
        raise SyncError("a title ID may not be both explicitly allowed and denied")
    return Policy(
        allowed_app_types=string_set("allowed_app_types"),
        allowed_title_ids=allowed_ids,
        denied_title_ids=denied_ids,
        denied_title_prefixes=tuple(prefixes),
        denied_platform_markers=string_set("denied_platform_markers"),
    )


def _validate_source_package_url(value: str, title_id: str) -> None:
    try:
        parsed = urllib.parse.urlsplit(value)
        query = urllib.parse.parse_qs(parsed.query, strict_parsing=True)
        port = parsed.port
    except ValueError as exc:
        raise SyncError(f"{title_id} has a malformed source package URL: {exc}") from exc
    if parsed.scheme.lower() not in {"http", "https"}:
        raise SyncError(f"{title_id} source package URL is not HTTP(S)")
    if parsed.hostname is None or parsed.hostname.lower() != "api.pkg-zone.com":
        raise SyncError(f"{title_id} source package host is not api.pkg-zone.com")
    if parsed.username is not None or parsed.password is not None or parsed.fragment:
        raise SyncError(f"{title_id} source package URL contains credentials or a fragment")
    if port not in {None, 80, 443} or parsed.path != "/download.php":
        raise SyncError(f"{title_id} source package URL has an unexpected port or path")
    if set(query) != {"tid"} or query["tid"] != [title_id]:
        raise SyncError(f"{title_id} source package URL has an unexpected query")


def _database_uri(path: Path) -> str:
    return path.resolve().as_uri() + "?mode=ro&immutable=1"


def load_source_rows(path: Path) -> list[SourceRow]:
    try:
        connection = sqlite3.connect(_database_uri(path), uri=True)
    except sqlite3.Error as exc:
        raise SyncError(f"cannot open HB-Store database: {exc}") from exc
    try:
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA query_only = ON")
        opcode_budget = [1000]

        def limit_sqlite_work() -> int:
            opcode_budget[0] -= 1
            return 1 if opcode_budget[0] < 0 else 0

        connection.set_progress_handler(limit_sqlite_work, 1000)
        table = connection.execute(
            "SELECT type, sql FROM sqlite_master WHERE name = 'homebrews'"
        ).fetchone()
        if table is None or table["type"] != "table" or not isinstance(table["sql"], str):
            raise SyncError("homebrews must be an ordinary SQLite table")
        if "CREATE VIRTUAL TABLE" in table["sql"].upper():
            raise SyncError("homebrews must not be a virtual table")
        columns = {str(row[1]) for row in connection.execute("PRAGMA table_info(homebrews)")}
        missing = REQUIRED_COLUMNS - columns
        if missing:
            raise SyncError(f"homebrews table is missing columns: {sorted(missing)}")
        records = connection.execute(
            "SELECT id, name, package, version, md5, apptype, pv, releaseddate "
            "FROM homebrews ORDER BY id, version LIMIT ?",
            (MAX_SOURCE_ROWS + 1,),
        ).fetchall()
        if not records or len(records) > MAX_SOURCE_ROWS:
            raise SyncError(
                f"homebrews row count must be 1-{MAX_SOURCE_ROWS}, got at least {len(records)}"
            )
    except sqlite3.Error as exc:
        raise SyncError(f"cannot query HB-Store database: {exc}") from exc
    finally:
        connection.close()

    rows: list[SourceRow] = []
    title_ids: set[str] = set()
    for index, record in enumerate(records):
        label = f"homebrews row {index + 1}"
        title_id = _bounded_text(record["id"], f"{label}.id", 9)
        name = _bounded_text(record["name"], f"{label}.name", 96)
        package = _bounded_text(record["package"], f"{label}.package", 2048)
        version = _bounded_text(record["version"], f"{label}.version", 16)
        app_type_raw = _bounded_text(record["apptype"], f"{label}.apptype", 64)
        platform_raw = _bounded_text(record["pv"], f"{label}.pv", 128, nullable=True)
        release = _bounded_text(record["releaseddate"], f"{label}.releaseddate", 32, nullable=True)
        assert title_id is not None and name is not None and package is not None
        assert version is not None and app_type_raw is not None
        if not TITLE_ID_RE.fullmatch(title_id):
            raise SyncError(f"{label}.id is not a PS4 title ID")
        if not VERSION_RE.fullmatch(version):
            raise SyncError(f"{label}.version is invalid")
        _validate_source_package_url(package, title_id)
        if release is not None and not DATE_RE.fullmatch(release):
            raise SyncError(f"{label}.releaseddate must be YYYY-MM-DD or null")
        source_md5 = record["md5"]
        if source_md5 == "":
            source_md5 = None
        elif source_md5 is not None:
            source_md5 = _bounded_text(source_md5, f"{label}.md5", 32)
            assert source_md5 is not None
            if not MD5_RE.fullmatch(source_md5):
                raise SyncError(f"{label}.md5 is invalid")
            source_md5 = source_md5.lower()
        if title_id in title_ids:
            raise SyncError(f"duplicate source title ID: {title_id}")
        title_ids.add(title_id)
        app_type = app_type_raw.strip().casefold()
        platform_marker = (platform_raw or "").strip().casefold()
        _bounded_text(app_type, f"{label}.normalized apptype", 64)
        if platform_marker:
            _bounded_text(platform_marker, f"{label}.normalized pv", 128)
        rows.append(SourceRow(
            title_id=title_id,
            name=name,
            version=version,
            package=package,
            source_md5=source_md5,
            app_type=app_type,
            platform_marker=platform_marker,
            release=release,
        ))
    return rows


def policy_exclusion(row: SourceRow, policy: Policy) -> str | None:
    if row.title_id in policy.denied_title_ids:
        return "explicitly denied title ID"
    if any(row.title_id.startswith(prefix) for prefix in policy.denied_title_prefixes):
        return "retail-style title prefix"
    if row.platform_marker in policy.denied_platform_markers:
        return "PS5-only platform marker"
    if row.title_id in policy.allowed_title_ids:
        return None
    if row.app_type not in policy.allowed_app_types:
        return f"app type {row.app_type!r} is not approved"
    return None


def select_rows(rows: list[SourceRow], policy: Policy) -> tuple[list[SourceRow], dict[str, int]]:
    selected: list[SourceRow] = []
    excluded: dict[str, int] = {}
    for row in rows:
        reason = policy_exclusion(row, policy)
        if reason is None:
            selected.append(row)
        else:
            excluded[reason] = excluded.get(reason, 0) + 1
    if not selected:
        raise SyncError("HB-Store policy selected zero PS4 homebrew records")
    return selected, excluded


def _make_opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(RejectRedirects())


def _request_bytes(opener: Any, limiter: RequestLimiter, url: str, maximum: int,
                   headers: dict[str, str] | None = None) -> tuple[bytes, Any]:
    limiter.wait()
    request_headers = {"User-Agent": USER_AGENT, "Accept": "application/octet-stream"}
    if headers:
        request_headers.update(headers)
    request = urllib.request.Request(url, headers=request_headers, method="GET")
    try:
        response = opener.open(request, timeout=REQUEST_TIMEOUT_SECONDS)
    except (urllib.error.URLError, OSError, SyncError) as exc:
        raise SyncError(f"request failed for {url}: {exc}") from exc
    with response:
        status = getattr(response, "status", response.getcode())
        final_url = response.geturl()
        if status != 200 or final_url != url:
            raise SyncError(f"unexpected response for {url}: status {status}, final URL {final_url}")
        length = response.headers.get("Content-Length")
        if length is not None:
            try:
                if int(length, 10) > maximum:
                    raise SyncError(f"response for {url} exceeds {maximum} bytes")
            except ValueError as exc:
                raise SyncError(f"response for {url} has invalid Content-Length") from exc
        payload = response.read(maximum + 1)
        if len(payload) > maximum:
            raise SyncError(f"response for {url} exceeds {maximum} bytes")
        return payload, response.headers


def _store_hash(opener: Any, limiter: RequestLimiter) -> str:
    payload, _ = _request_bytes(
        opener, limiter, STORE_HASH_URL, MAX_HASH_BYTES,
        {"Accept": "application/json"},
    )
    try:
        document = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise SyncError(f"invalid HB-Store hash response: {exc}") from exc
    if not isinstance(document, dict) or set(document) != {"hash"}:
        raise SyncError("HB-Store hash response must contain only hash")
    value = document["hash"]
    if not isinstance(value, str) or not MD5_RE.fullmatch(value):
        raise SyncError("HB-Store hash response is not an MD5 value")
    return value.lower()


def fetch_database(opener: Any, limiter: RequestLimiter) -> tuple[bytes, str]:
    before = _store_hash(opener, limiter)
    payload, _ = _request_bytes(opener, limiter, STORE_DB_URL, MAX_DATABASE_BYTES)
    after = _store_hash(opener, limiter)
    if before != after:
        raise SyncError("HB-Store database changed during retrieval")
    # The upstream protocol publishes MD5. TLS supplies authenticity; this check detects
    # snapshot races and transport/storage corruption, not adversarial collision resistance.
    actual = hashlib.md5(payload).hexdigest()
    if actual != before:
        raise SyncError(f"HB-Store database MD5 mismatch: expected {before}, got {actual}")
    if not payload.startswith(b"SQLite format 3\x00"):
        raise SyncError("HB-Store response is not a SQLite 3 database")
    return payload, actual


def _package_url(title_id: str) -> str:
    return PACKAGE_URL_PREFIX + urllib.parse.quote(title_id, safe="")


def probe_package(opener: Any, limiter: RequestLimiter, title_id: str) -> int:
    url = _package_url(title_id)
    limiter.wait()
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "application/octet-stream",
            "Range": "bytes=0-3",
        },
        method="GET",
    )
    try:
        response = opener.open(request, timeout=REQUEST_TIMEOUT_SECONDS)
    except (urllib.error.URLError, OSError, SyncError) as exc:
        raise SyncError(f"package probe failed for {title_id}: {exc}") from exc
    with response:
        status = getattr(response, "status", response.getcode())
        final_url = response.geturl()
        if status != 206 or final_url != url:
            raise SyncError(
                f"package probe for {title_id} returned status {status}, final URL {final_url}"
            )
        content_range = response.headers.get("Content-Range", "")
        match = CONTENT_RANGE_RE.fullmatch(content_range)
        if not match:
            raise SyncError(f"package probe for {title_id} has invalid Content-Range")
        size = int(match.group(1), 10)
        if size <= len(PKG_MAGIC):
            raise SyncError(f"package probe for {title_id} reports an invalid size")
        if size > MAX_UINT64:
            raise SyncError(f"package probe for {title_id} reports an invalid size")
        content_length = response.headers.get("Content-Length")
        if content_length is not None and content_length != str(len(PKG_MAGIC)):
            raise SyncError(f"package probe for {title_id} has invalid Content-Length")
        if response.headers.get_content_type().lower() != "application/octet-stream":
            raise SyncError(f"package probe for {title_id} has an unexpected Content-Type")
        body = response.read(len(PKG_MAGIC) + 1)
        if body != PKG_MAGIC:
            raise SyncError(f"package probe for {title_id} did not return PS4 PKG magic")
        return size


def load_state(path: Path, *, work: bool = False) -> CacheState:
    if not path.exists():
        return CacheState(source_db_md5="", packages={})
    document = _json_object(path, MAX_STATE_BYTES)
    if work:
        expected = {"schema", "kind", "source_db_md5", "packages"}
        valid_schema = (
            set(document) == expected
            and type(document["schema"]) is int
            and document["schema"] == WORK_STATE_SCHEMA
            and document["kind"] == WORK_STATE_KIND
        )
    else:
        valid_schema = (
            set(document) == {"schema", "source_db_md5", "packages"}
            and type(document["schema"]) is int
            and document["schema"] == PUBLISHED_STATE_SCHEMA
        )
    if not valid_schema:
        kind = "work state" if work else "state"
        raise SyncError(f"{path} has an unsupported {kind} schema")
    source_hash = document["source_db_md5"]
    if (
        not isinstance(source_hash, str)
        or not MD5_RE.fullmatch(source_hash)
        or source_hash != source_hash.lower()
    ):
        raise SyncError(f"{path} has an invalid source_db_md5")
    packages = document["packages"]
    if not isinstance(packages, dict) or len(packages) > MAX_SOURCE_ROWS:
        raise SyncError(
            f"{path}.packages must be an object with at most {MAX_SOURCE_ROWS} entries"
        )
    required = {
        "name", "version", "source_md5", "size", "app_type", "platform_marker", "release"
    }
    for title_id, entry in packages.items():
        if not isinstance(title_id, str) or not TITLE_ID_RE.fullmatch(title_id):
            raise SyncError(f"{path} contains an invalid package title ID")
        if not isinstance(entry, dict) or set(entry) != required:
            raise SyncError(f"{path} package {title_id} has invalid fields")
        _bounded_text(entry["name"], f"state {title_id}.name", 96)
        version = _bounded_text(entry["version"], f"state {title_id}.version", 16)
        if version is None or not VERSION_RE.fullmatch(version):
            raise SyncError(f"state {title_id}.version is invalid")
        source_md5 = entry["source_md5"]
        if source_md5 is not None and (
            not isinstance(source_md5, str)
            or not MD5_RE.fullmatch(source_md5)
            or source_md5 != source_md5.lower()
        ):
            raise SyncError(f"state {title_id}.source_md5 is invalid")
        size = entry["size"]
        if (
            not isinstance(size, str)
            or len(size) > 20
            or not re.fullmatch(r"[0-9]+", size)
            or int(size, 10) <= 0
            or int(size, 10) > MAX_UINT64
        ):
            raise SyncError(f"state {title_id}.size is invalid")
        app_type = _bounded_text(entry["app_type"], f"state {title_id}.app_type", 64)
        assert app_type is not None
        if app_type != app_type.strip().casefold():
            raise SyncError(f"state {title_id}.app_type is not canonical")
        platform = entry["platform_marker"]
        if not isinstance(platform, str):
            raise SyncError(f"state {title_id}.platform_marker is invalid")
        if platform:
            _bounded_text(platform, f"state {title_id}.platform_marker", 128)
        if platform != platform.strip().casefold():
            raise SyncError(f"state {title_id}.platform_marker is not canonical")
        release = entry["release"]
        if release is not None and (
            not isinstance(release, str) or not DATE_RE.fullmatch(release)
        ):
            raise SyncError(f"state {title_id}.release is invalid")
    return CacheState(source_db_md5=source_hash.lower(), packages=packages)

def load_recent_work_state(path: Path) -> CacheState:
    if not path.exists():
        return CacheState(source_db_md5="", packages={})
    try:
        age = time.time() - path.stat().st_mtime
    except OSError as exc:
        raise SyncError(f"cannot inspect work cache {path}: {exc}") from exc
    if age < 0 or age > WORK_CACHE_MAX_AGE_SECONDS:
        path.unlink(missing_ok=True)
        return CacheState(source_db_md5="", packages={})
    return load_state(path, work=True)



def _cache_size(row: SourceRow, source_db_md5: str, state: CacheState) -> int | None:
    cached = state.packages.get(row.title_id)
    if cached is None or cached["version"] != row.version:
        return None
    if row.source_md5 is not None:
        if cached["source_md5"] != row.source_md5:
            return None
    elif cached["source_md5"] is not None or state.source_db_md5 != source_db_md5:
        return None
    return int(cached["size"], 10)

def _state_entry(row: SourceRow, size: int) -> dict[str, Any]:
    return {
        "name": row.name,
        "version": row.version,
        "source_md5": row.source_md5,
        "size": str(size),
        "app_type": row.app_type,
        "platform_marker": row.platform_marker,
        "release": row.release,
    }



def _metadata(row: SourceRow, size: int) -> dict[str, Any]:
    metadata: dict[str, Any] = {
        "title_id": row.title_id,
        "region": "HB-Store",
        "name": row.name,
        "version": row.version,
        "size": str(size),
        "cover_url": f"{COVER_URL_PREFIX}{row.title_id}/cover.png",
    }
    if row.release is not None:
        metadata["release"] = row.release
    if row.platform_marker and len(row.platform_marker.encode("utf-8")) <= 16:
        metadata["min_fw"] = row.platform_marker
    return metadata


def build_outputs(rows: list[SourceRow], sizes: dict[str, int], source_db_md5: str) -> tuple[dict[str, Any], dict[str, Any]]:
    data: dict[str, Any] = {}
    packages: dict[str, Any] = {}
    for row in sorted(rows, key=lambda item: item.title_id):
        size = sizes[row.title_id]
        data[_package_url(row.title_id)] = _metadata(row, size)
        packages[row.title_id] = _state_entry(row, size)
    catalog = {"DATA": data}
    state = {
        "schema": PUBLISHED_STATE_SCHEMA,
        "source_db_md5": source_db_md5,
        "packages": packages,
    }
    validate_document(catalog)
    catalog_bytes = _json_bytes(catalog)
    if len(catalog_bytes) > MAX_CATALOG_BYTES:
        raise SyncError(f"generated catalog exceeds {MAX_CATALOG_BYTES} bytes")
    state_bytes = _json_bytes(state)
    if len(state_bytes) > MAX_STATE_BYTES:
        raise SyncError(f"generated state exceeds {MAX_STATE_BYTES} bytes")
    return catalog, state


def _json_bytes(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        try:
            os.fchmod(descriptor, 0o600)
        except AttributeError:
            pass
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

def _publish_outputs(catalog_path: Path, state_path: Path,
                     catalog: dict[str, Any], state: dict[str, Any]) -> None:
    state_existed = state_path.exists()
    previous_state = _read_bounded_file(state_path, MAX_STATE_BYTES) if state_existed else b""
    _atomic_write(state_path, _json_bytes(state))
    try:
        _atomic_write(catalog_path, _json_bytes(catalog))
    except Exception as publish_error:
        try:
            if state_existed:
                _atomic_write(state_path, previous_state)
            else:
                state_path.unlink(missing_ok=True)
        except Exception as rollback_error:
            raise SyncError(
                f"catalog publication failed and state rollback failed: {rollback_error}"
            ) from publish_error
        raise


def verify_outputs(catalog_path: Path, state_path: Path, policy_path: Path) -> int:
    policy = load_policy(policy_path)
    catalog = _json_object(catalog_path, MAX_CATALOG_BYTES)
    validate_document(catalog)
    state = load_state(state_path)
    data = catalog.get("DATA")
    if not isinstance(data, dict) or not data:
        raise SyncError("maintained HB-Store catalog must not be empty")
    if len(data) != len(state.packages):
        raise SyncError("catalog and HB-Store state package counts differ")
    for title_id, cached in state.packages.items():
        row = SourceRow(
            title_id=title_id,
            name=cached["name"],
            version=cached["version"],
            package=PACKAGE_URL_PREFIX + title_id,
            source_md5=cached["source_md5"],
            app_type=cached["app_type"],
            platform_marker=cached["platform_marker"],
            release=cached["release"],
        )
        reason = policy_exclusion(row, policy)
        if reason is not None:
            raise SyncError(f"catalog title {title_id} violates policy: {reason}")
        url = _package_url(title_id)
        if data.get(url) != _metadata(row, int(cached["size"], 10)):
            raise SyncError(f"catalog metadata differs from state for {title_id}")
    return len(data)


def synchronize(database_path: Path | None, catalog_path: Path, state_path: Path,
                policy_path: Path, *, offline: bool, request_delay: float,
                reprobe_all: bool = False, opener: Any | None = None,
                sleeper: Callable[[float], None] = time.sleep) -> tuple[int, int, dict[str, int]]:
    if offline and reprobe_all:
        raise SyncError("--offline and --reprobe-all cannot be combined")
    policy = load_policy(policy_path)
    old_state = (
        CacheState(source_db_md5="", packages={})
        if reprobe_all else load_state(state_path)
    )
    work_state_path = Path(str(state_path) + ".work")
    work_state = (
        CacheState(source_db_md5="", packages={})
        if offline else load_recent_work_state(work_state_path)
    )
    opener = opener or _make_opener()
    limiter = RequestLimiter(request_delay, sleeper)
    temporary_database: Path | None = None
    try:
        if database_path is None:
            if offline:
                raise SyncError("--offline requires --database")
            payload, source_db_md5 = fetch_database(opener, limiter)
        else:
            payload = _read_bounded_file(database_path, MAX_DATABASE_BYTES)
            source_db_md5 = hashlib.md5(payload).hexdigest()
        if not payload.startswith(b"SQLite format 3\x00"):
            source = STORE_DB_URL if database_path is None else str(database_path)
            raise SyncError(f"{source} is not a SQLite 3 database")

        descriptor, temporary_name = tempfile.mkstemp(prefix="fpkgi-hb-store-", suffix=".db")
        temporary_database = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        database = temporary_database

        rows = load_source_rows(database)
        selected, excluded = select_rows(rows, policy)
        sizes: dict[str, int] = {}
        work_packages: dict[str, dict[str, Any]] = {}
        for row in selected:
            reusable_work_size = _cache_size(row, source_db_md5, work_state)
            if reusable_work_size is not None:
                work_packages[row.title_id] = _state_entry(row, reusable_work_size)
        probe_errors: list[str] = []
        probes = 0
        for row in selected:
            cached_size: int | None = None
            cached_from_work = False
            if offline:
                cached_size = _cache_size(row, source_db_md5, old_state)
            else:
                cached_size = _cache_size(row, source_db_md5, work_state)
                cached_from_work = cached_size is not None
                if cached_size is None and not reprobe_all:
                    cached_size = _cache_size(row, source_db_md5, old_state)
            if cached_size is not None:
                sizes[row.title_id] = cached_size
                if cached_from_work:
                    work_packages[row.title_id] = _state_entry(row, cached_size)
                continue
            if offline:
                raise SyncError(f"offline state has no valid package probe for {row.title_id}")
            try:
                size = probe_package(opener, limiter, row.title_id)
            except SyncError as exc:
                probe_errors.append(f"{row.title_id}: {exc}")
                continue
            sizes[row.title_id] = size
            work_packages[row.title_id] = _state_entry(row, size)
            probes += 1
            partial_state = {
                "schema": WORK_STATE_SCHEMA,
                "kind": WORK_STATE_KIND,
                "source_db_md5": source_db_md5,
                "packages": work_packages,
            }
            _atomic_write(work_state_path, _json_bytes(partial_state))

        if probe_errors:
            details = "; ".join(probe_errors[:8])
            if len(probe_errors) > 8:
                details += f"; and {len(probe_errors) - 8} more"
            raise SyncError(f"{len(probe_errors)} package probes failed: {details}")

        catalog, new_state = build_outputs(selected, sizes, source_db_md5)
        # State is staged first and rolled back if the runtime catalog cannot publish.
        _publish_outputs(catalog_path, state_path, catalog, new_state)
        if not offline:
            work_state_path.unlink(missing_ok=True)
        return len(selected), probes, excluded
    finally:
        if temporary_database is not None:
            temporary_database.unlink(missing_ok=True)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, help="read a local authorized store.db snapshot")
    parser.add_argument("--catalog", type=Path, default=Path("catalog/catalog.json"))
    parser.add_argument("--state", type=Path, default=Path("catalog/hb_store_state.json"))
    parser.add_argument("--policy", type=Path, default=Path("catalog/hb_store_policy.json"))
    parser.add_argument("--offline", action="store_true", help="forbid network and require reusable probe state")
    parser.add_argument(
        "--reprobe-all", action="store_true",
        help="ignore published package probes; recent online recovery work remains reusable",
    )
    parser.add_argument("--verify-output", action="store_true", help="validate catalog/state/policy parity only")
    parser.add_argument("--request-delay", type=float, default=1.1,
                        help="minimum seconds between official API requests (default: 1.1)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(argv or sys.argv[1:])
    try:
        if arguments.verify_output:
            count = verify_outputs(arguments.catalog, arguments.state, arguments.policy)
            print(f"HB-Store catalog/state OK: {count} PS4 homebrew packages")
            return 0
        if (not math.isfinite(arguments.request_delay) or
                arguments.request_delay < MIN_LIVE_REQUEST_DELAY_SECONDS):
            raise SyncError(
                f"request delay must be at least {MIN_LIVE_REQUEST_DELAY_SECONDS:.1f} seconds"
            )
        if arguments.offline and arguments.reprobe_all:
            raise SyncError("--offline and --reprobe-all cannot be combined")
        selected, probes, excluded = synchronize(
            arguments.database,
            arguments.catalog,
            arguments.state,
            arguments.policy,
            offline=arguments.offline,
            request_delay=arguments.request_delay,
            reprobe_all=arguments.reprobe_all,
        )
        excluded_count = sum(excluded.values())
        print(
            f"HB-Store sync OK: {selected} selected, {excluded_count} excluded, "
            f"{probes} package probes"
        )
        for reason, count in sorted(excluded.items()):
            print(f"  excluded {count}: {reason}")
        return 0
    except (OSError, sqlite3.Error, SyncError, ValueError) as exc:
        print(f"HB-Store sync failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
