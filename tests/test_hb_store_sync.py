from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import sqlite3
import sys
import tempfile
import unittest
import urllib.error
from email.message import Message
from pathlib import Path
from typing import Any, Callable
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "sync_hb_store", ROOT / "scripts" / "sync_hb_store.py"
)
assert SPEC and SPEC.loader
SYNC = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = SYNC
SPEC.loader.exec_module(SYNC)
POLICY = ROOT / "catalog" / "hb_store_policy.json"


class FakeResponse:
    def __init__(self, url: str, body: bytes, *, status: int = 200,
                 headers: dict[str, str] | None = None) -> None:
        self.status = status
        self._url = url
        self._body = io.BytesIO(body)
        self.headers = Message()
        for key, value in (headers or {}).items():
            self.headers[key] = value

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def getcode(self) -> int:
        return self.status

    def geturl(self) -> str:
        return self._url

    def read(self, size: int = -1) -> bytes:
        return self._body.read(size)


class FakeOpener:
    def __init__(self, handler: Callable[[Any], FakeResponse]) -> None:
        self.handler = handler
        self.requests: list[Any] = []

    def open(self, request: Any, timeout: int) -> FakeResponse:
        self.requests.append(request)
        return self.handler(request)


def create_database(path: Path, rows: list[dict[str, object]]) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.execute(
            "CREATE TABLE homebrews ("
            "pid INTEGER NOT NULL, id TEXT, name TEXT, package TEXT, version TEXT, "
            "Size TEXT, Author TEXT, apptype TEXT, pv TEXT, releaseddate TEXT, md5 TEXT)"
        )
        for index, row in enumerate(rows, 1):
            values = {
                "pid": index,
                "id": "TEST00001",
                "name": "Test Homebrew",
                "package": "http://api.pkg-zone.com/download.php?tid=TEST00001",
                "version": "1.00",
                "Size": "4.06 MB",
                "Author": "Test Author",
                "apptype": "Utility",
                "pv": "5.05+",
                "releaseddate": "2026-01-02",
                "md5": "a" * 32,
            }
            values.update(row)
            connection.execute(
                "INSERT INTO homebrews "
                "(pid,id,name,package,version,Size,Author,apptype,pv,releaseddate,md5) "
                "VALUES (:pid,:id,:name,:package,:version,:Size,:Author,:apptype,:pv,:releaseddate,:md5)",
                values,
            )
        connection.commit()
    finally:
        connection.close()


def source_row(**overrides: object) -> Any:
    values: dict[str, object] = {
        "title_id": "TEST00001",
        "name": "Test Homebrew",
        "version": "1.00",
        "package": "http://api.pkg-zone.com/download.php?tid=TEST00001",
        "source_md5": "a" * 32,
        "app_type": "utility",
        "platform_marker": "5.05+",
        "release": "2026-01-02",
    }
    values.update(overrides)
    return SYNC.SourceRow(**values)


class HbStoreSyncTests(unittest.TestCase):
    def test_policy_selects_only_supported_ps4_homebrew(self) -> None:
        policy = SYNC.load_policy(POLICY)
        expected_retail_prefixes = {
            "CUSA", "PCAS", "PCJS", "PCKS", "PCSA", "PCSB", "PCSE", "PCSF",
            "PCSG", "PCSH", "PLAS", "PLJM", "PLJS", "PLPM", "PPSA",
        }
        self.assertTrue(expected_retail_prefixes <= set(policy.denied_title_prefixes))
        rows = [
            source_row(),
            source_row(title_id="WILI00000", app_type="media"),
            source_row(title_id="GBTX00000"),
            source_row(title_id="CUSA00001", app_type="game"),
            source_row(title_id="PSFI00001", platform_marker="ps5_supported"),
            source_row(title_id="MEDI00001", app_type="media"),
        ]
        selected, excluded = SYNC.select_rows(rows, policy)
        self.assertEqual([row.title_id for row in selected], ["TEST00001", "WILI00000"])
        self.assertEqual(sum(excluded.values()), 4)
        self.assertEqual(excluded["explicitly denied title ID"], 1)
        self.assertEqual(excluded["retail-style title prefix"], 1)
        self.assertEqual(excluded["PS5-only platform marker"], 1)

    def test_policy_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            policy = Path(directory) / "policy.json"
            duplicate = POLICY.read_text(encoding="utf-8").replace(
                "{", '{"allowed_app_types":["utility"],', 1
            )
            policy.write_text(duplicate, encoding="utf-8")
            with self.assertRaisesRegex(SYNC.SyncError, "duplicate JSON object key"):
                SYNC.load_policy(policy)

    def test_database_rows_require_official_package_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "store.db"
            create_database(database, [{}])
            rows = SYNC.load_source_rows(database)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0].title_id, "TEST00001")

            bad_database = Path(directory) / "bad.db"
            create_database(bad_database, [{
                "package": "https://example.test/download.php?tid=TEST00001",
            }])
            with self.assertRaisesRegex(SYNC.SyncError, "source package host"):
                SYNC.load_source_rows(bad_database)

            empty_md5 = Path(directory) / "empty-md5.db"
            create_database(empty_md5, [{"md5": ""}])
            self.assertIsNone(SYNC.load_source_rows(empty_md5)[0].source_md5)

            expanding_casefold = Path(directory) / "expanding-casefold.db"
            create_database(expanding_casefold, [{"apptype": "İ" * 32}])
            with self.assertRaisesRegex(SYNC.SyncError, "normalized apptype"):
                SYNC.load_source_rows(expanding_casefold)

    def test_database_rejects_duplicate_identity_and_missing_schema(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            duplicate = Path(directory) / "duplicate.db"
            create_database(duplicate, [{}, {"version": "2.00"}])
            with self.assertRaisesRegex(SYNC.SyncError, "duplicate source title ID"):
                SYNC.load_source_rows(duplicate)

            incomplete = Path(directory) / "incomplete.db"
            connection = sqlite3.connect(incomplete)
            connection.execute("CREATE TABLE homebrews (id TEXT)")
            connection.execute("INSERT INTO homebrews VALUES ('TEST00001')")
            connection.commit()
            connection.close()
            with self.assertRaisesRegex(SYNC.SyncError, "missing columns"):
                SYNC.load_source_rows(incomplete)

            view = Path(directory) / "view.db"
            create_database(view, [{}])
            connection = sqlite3.connect(view)
            connection.execute("ALTER TABLE homebrews RENAME TO backing")
            connection.execute("CREATE VIEW homebrews AS SELECT * FROM backing")
            connection.commit()
            connection.close()
            with self.assertRaisesRegex(SYNC.SyncError, "ordinary SQLite table"):
                SYNC.load_source_rows(view)

            too_many = Path(directory) / "too-many.db"
            create_database(too_many, [
                {"id": f"TSTX{index:05d}",
                 "package": SYNC.PACKAGE_URL_PREFIX + f"TSTX{index:05d}"}
                for index in range(SYNC.MAX_SOURCE_ROWS + 1)
            ])
            with self.assertRaisesRegex(SYNC.SyncError, "row count must be 1-512"):
                SYNC.load_source_rows(too_many)

    def test_package_probe_requires_bounded_range_magic(self) -> None:
        url = SYNC.PACKAGE_URL_PREFIX + "TEST00001"

        def valid(request: Any) -> FakeResponse:
            self.assertEqual(request.headers["Range"], "bytes=0-3")
            self.assertIn("FPKGi-Native-Catalog/1.03", request.headers["User-agent"])
            return FakeResponse(url, SYNC.PKG_MAGIC, status=206, headers={
                "Content-Range": "bytes 0-3/4259840",
                "Content-Length": "4",
                "Content-Type": "application/octet-stream",
            })

        size = SYNC.probe_package(FakeOpener(valid), SYNC.RequestLimiter(0), "TEST00001")
        self.assertEqual(size, 4259840)

        def bad_magic(request: Any) -> FakeResponse:
            return FakeResponse(url, b"NOPE", status=206, headers={
                "Content-Range": "bytes 0-3/4259840",
                "Content-Length": "4",
                "Content-Type": "application/octet-stream",
            })

        with self.assertRaisesRegex(SYNC.SyncError, "PKG magic"):
            SYNC.probe_package(FakeOpener(bad_magic), SYNC.RequestLimiter(0), "TEST00001")

        def oversized(request: Any) -> FakeResponse:
            return FakeResponse(url, SYNC.PKG_MAGIC, status=206, headers={
                "Content-Range": "bytes 0-3/18446744073709551616",
                "Content-Length": "4",
                "Content-Type": "application/octet-stream",
            })

        with self.assertRaisesRegex(SYNC.SyncError, "invalid size"):
            SYNC.probe_package(FakeOpener(oversized), SYNC.RequestLimiter(0), "TEST00001")

    def test_database_fetch_requires_stable_matching_upstream_hash(self) -> None:
        database = b"SQLite format 3\x00" + b"test database bytes"
        digest = hashlib.md5(database).hexdigest()
        hash_body = json.dumps({"hash": digest}).encode()
        responses = [
            FakeResponse(SYNC.STORE_HASH_URL, hash_body, headers={
                "Content-Length": str(len(hash_body)), "Content-Type": "application/json"
            }),
            FakeResponse(SYNC.STORE_DB_URL, database, headers={
                "Content-Length": str(len(database)), "Content-Type": "application/octet-stream"
            }),
            FakeResponse(SYNC.STORE_HASH_URL, hash_body, headers={
                "Content-Length": str(len(hash_body)), "Content-Type": "application/json"
            }),
        ]
        opener = FakeOpener(lambda request: responses.pop(0))
        payload, actual = SYNC.fetch_database(opener, SYNC.RequestLimiter(0))
        self.assertEqual(payload, database)
        self.assertEqual(actual, digest)
        self.assertEqual(len(opener.requests), 3)

        duplicate_hash = (
            b'{"hash":"' + digest.encode() + b'","hash":"' + digest.encode() + b'"}'
        )
        duplicate_opener = FakeOpener(lambda request: FakeResponse(
            SYNC.STORE_HASH_URL, duplicate_hash,
            headers={"Content-Type": "application/json"},
        ))
        with self.assertRaisesRegex(SYNC.SyncError, "duplicate JSON object key"):
            SYNC._store_hash(duplicate_opener, SYNC.RequestLimiter(0))

    def test_offline_cache_regenerates_and_verifies_catalog(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "store.db"
            catalog = root / "catalog.json"
            state = root / "state.json"
            create_database(database, [{}])
            source_hash = hashlib.md5(database.read_bytes()).hexdigest()
            rows = SYNC.load_source_rows(database)
            document, cached = SYNC.build_outputs(rows, {"TEST00001": 4259840}, source_hash)
            catalog.write_bytes(SYNC._json_bytes(document))
            state.write_bytes(SYNC._json_bytes(cached))
            ignored_work = Path(str(state) + ".work")
            ignored_work.write_text("not JSON", encoding="utf-8")

            selected, probes, excluded = SYNC.synchronize(
                database, catalog, state, POLICY,
                offline=True, request_delay=0, opener=FakeOpener(
                    lambda request: self.fail("offline sync attempted network access")
                ), sleeper=lambda seconds: None,
            )
            self.assertEqual((selected, probes, excluded), (1, 0, {}))
            self.assertEqual(SYNC.verify_outputs(catalog, state, POLICY), 1)
            self.assertEqual(ignored_work.read_text(encoding="utf-8"), "not JSON")
            ignored_work.unlink()

            incremental = FakeOpener(
                lambda request: self.fail("unchanged package was re-probed")
            )
            selected, probes, excluded = SYNC.synchronize(
                database, catalog, state, POLICY,
                offline=False, request_delay=0, opener=incremental,
                sleeper=lambda seconds: None,
            )
            self.assertEqual((selected, probes, excluded), (1, 0, {}))
            self.assertEqual(len(incremental.requests), 0)

            def current_probe(request: Any) -> FakeResponse:
                return FakeResponse(request.full_url, SYNC.PKG_MAGIC, status=206, headers={
                    "Content-Range": "bytes 0-3/4259840",
                    "Content-Length": "4",
                    "Content-Type": "application/octet-stream",
                })

            online = FakeOpener(current_probe)
            state.write_text("invalid published state", encoding="utf-8")
            selected, probes, excluded = SYNC.synchronize(
                database, catalog, state, POLICY,
                offline=False, request_delay=0, reprobe_all=True, opener=online,
                sleeper=lambda seconds: None,
            )
            self.assertEqual((selected, probes, excluded), (1, 1, {}))
            self.assertEqual(len(online.requests), 1)
            self.assertEqual(SYNC.verify_outputs(catalog, state, POLICY), 1)

    def test_offline_sync_never_promotes_unpublished_work_cache(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "store.db"
            catalog = root / "catalog.json"
            state = root / "state.json"
            work_state = Path(str(state) + ".work")
            create_database(database, [{}])
            source_hash = hashlib.md5(database.read_bytes()).hexdigest()
            rows = SYNC.load_source_rows(database)
            _, recovered = SYNC.build_outputs(
                rows, {"TEST00001": 4259840}, source_hash
            )
            state.write_bytes(SYNC._json_bytes({
                "schema": 1, "source_db_md5": source_hash, "packages": {},
            }))
            recovery_work = dict(recovered)
            recovery_work["schema"] = SYNC.WORK_STATE_SCHEMA
            recovery_work["kind"] = SYNC.WORK_STATE_KIND
            work_state.write_bytes(SYNC._json_bytes(recovery_work))

            with self.assertRaisesRegex(
                SYNC.SyncError, "offline state has no valid package probe"
            ):
                SYNC.synchronize(
                    database, catalog, state, POLICY,
                    offline=True, request_delay=0,
                    opener=FakeOpener(
                        lambda request: self.fail("offline sync attempted network access")
                    ),
                    sleeper=lambda seconds: None,
                )
            self.assertTrue(work_state.exists())
            work_state.write_bytes(SYNC._json_bytes(recovered))
            with self.assertRaisesRegex(SYNC.SyncError, "unsupported work state schema"):
                SYNC.load_recent_work_state(work_state)

    def test_probe_failure_keeps_existing_catalog_and_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "store.db"
            catalog = root / "catalog.json"
            state = root / "state.json"
            create_database(database, [{}])
            source_hash = hashlib.md5(database.read_bytes()).hexdigest()
            rows = SYNC.load_source_rows(database)
            document, cached = SYNC.build_outputs(rows, {"TEST00001": 4259840}, source_hash)
            catalog.write_bytes(SYNC._json_bytes(document))
            state.write_bytes(SYNC._json_bytes(cached))
            original_catalog = catalog.read_bytes()
            original_state = state.read_bytes()

            connection = sqlite3.connect(database)
            connection.execute("UPDATE homebrews SET md5 = ?", ("b" * 32,))
            connection.commit()
            connection.close()
            failing = FakeOpener(
                lambda request: (_ for _ in ()).throw(urllib.error.URLError("offline"))
            )
            with self.assertRaisesRegex(SYNC.SyncError, "package probe failed"):
                SYNC.synchronize(
                    database, catalog, state, POLICY,
                    offline=False, request_delay=0, opener=failing,
                    sleeper=lambda seconds: None,
                )
            self.assertEqual(catalog.read_bytes(), original_catalog)
            self.assertEqual(state.read_bytes(), original_state)

    def test_validated_work_cache_resumes_without_publishing_partial_catalog(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "store.db"
            catalog = root / "catalog.json"
            state = root / "state.json"
            work_state = Path(str(state) + ".work")
            create_database(database, [
                {
                    "id": "AONE00001",
                    "name": "First Homebrew",
                    "package": "http://api.pkg-zone.com/download.php?tid=AONE00001",
                    "md5": "a" * 32,
                },
                {
                    "id": "BTWO00002",
                    "name": "Second Homebrew",
                    "package": "http://api.pkg-zone.com/download.php?tid=BTWO00002",
                    "md5": "b" * 32,
                },
            ])
            original_catalog = b'{"DATA": {}}\n'
            catalog.write_bytes(original_catalog)

            def first_run(request: Any) -> FakeResponse:
                if request.full_url.endswith("AONE00001"):
                    return FakeResponse(request.full_url, SYNC.PKG_MAGIC, status=206, headers={
                        "Content-Range": "bytes 0-3/4096",
                        "Content-Length": "4",
                        "Content-Type": "application/octet-stream",
                    })
                raise urllib.error.URLError("temporary failure")

            with self.assertRaisesRegex(SYNC.SyncError, "package probe failed"):
                SYNC.synchronize(
                    database, catalog, state, POLICY,
                    offline=False, request_delay=0, opener=FakeOpener(first_run),
                    sleeper=lambda seconds: None,
                )
            self.assertEqual(catalog.read_bytes(), original_catalog)
            self.assertFalse(state.exists())
            self.assertEqual(
                set(SYNC.load_state(work_state, work=True).packages), {"AONE00001"}
            )

            second = FakeOpener(lambda request: FakeResponse(
                request.full_url, SYNC.PKG_MAGIC, status=206, headers={
                    "Content-Range": "bytes 0-3/8192",
                    "Content-Length": "4",
                    "Content-Type": "application/octet-stream",
                },
            ))
            selected, probes, excluded = SYNC.synchronize(
                database, catalog, state, POLICY,
                offline=False, request_delay=0, opener=second,
                sleeper=lambda seconds: None,
            )
            self.assertEqual((selected, probes, excluded), (2, 1, {}))
            self.assertEqual(
                [request.full_url for request in second.requests],
                [SYNC.PACKAGE_URL_PREFIX + "BTWO00002"],
            )
            self.assertFalse(work_state.exists())
            self.assertEqual(SYNC.verify_outputs(catalog, state, POLICY), 2)

    def test_partial_probe_write_preserves_later_recovery_entries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "store.db"
            catalog = root / "catalog.json"
            state = root / "state.json"
            work_state = Path(str(state) + ".work")
            definitions = [
                ("AONE00001", "First Homebrew", "a" * 32),
                ("BTWO00002", "Second Homebrew", "b" * 32),
                ("CTHR00003", "Third Homebrew", "c" * 32),
                ("DFOR00004", "Fourth Homebrew", "d" * 32),
            ]
            create_database(database, [
                {
                    "id": title_id,
                    "name": name,
                    "package": SYNC.PACKAGE_URL_PREFIX + title_id,
                    "md5": source_md5,
                }
                for title_id, name, source_md5 in definitions
            ])
            source_hash = hashlib.md5(database.read_bytes()).hexdigest()
            rows = {row.title_id: row for row in SYNC.load_source_rows(database)}
            work_state.write_bytes(SYNC._json_bytes({
                "schema": SYNC.WORK_STATE_SCHEMA,
                "kind": SYNC.WORK_STATE_KIND,
                "source_db_md5": source_hash,
                "packages": {
                    "AONE00001": SYNC._state_entry(rows["AONE00001"], 4096),
                    "CTHR00003": SYNC._state_entry(rows["CTHR00003"], 12288),
                },
            }))

            def partial_probe(request: Any) -> FakeResponse:
                if request.full_url.endswith("BTWO00002"):
                    return FakeResponse(
                        request.full_url, SYNC.PKG_MAGIC, status=206, headers={
                            "Content-Range": "bytes 0-3/8192",
                            "Content-Length": "4",
                            "Content-Type": "application/octet-stream",
                        },
                    )
                if request.full_url.endswith("DFOR00004"):
                    raise urllib.error.URLError("temporary failure")
                self.fail("validated recovery entry was probed again")

            opener = FakeOpener(partial_probe)
            with self.assertRaisesRegex(SYNC.SyncError, "package probes failed"):
                SYNC.synchronize(
                    database, catalog, state, POLICY,
                    offline=False, request_delay=0, opener=opener,
                    sleeper=lambda seconds: None,
                )
            recovered = SYNC.load_state(work_state, work=True)
            self.assertEqual(
                set(recovered.packages), {"AONE00001", "BTWO00002", "CTHR00003"}
            )
            self.assertEqual(
                [request.full_url for request in opener.requests],
                [
                    SYNC.PACKAGE_URL_PREFIX + "BTWO00002",
                    SYNC.PACKAGE_URL_PREFIX + "DFOR00004",
                ],
            )

    def test_published_reuse_never_contaminates_full_reprobe_work(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "store.db"
            catalog = root / "catalog.json"
            state = root / "state.json"
            work_state = Path(str(state) + ".work")
            create_database(database, [
                {
                    "id": "AONE00001", "name": "First Homebrew",
                    "package": SYNC.PACKAGE_URL_PREFIX + "AONE00001", "md5": "a" * 32,
                },
                {
                    "id": "BTWO00002", "name": "Second Homebrew",
                    "package": SYNC.PACKAGE_URL_PREFIX + "BTWO00002", "md5": "b" * 32,
                },
                {
                    "id": "CTHR00003", "name": "Third Homebrew",
                    "package": SYNC.PACKAGE_URL_PREFIX + "CTHR00003", "md5": "c" * 32,
                },
            ])
            source_hash = hashlib.md5(database.read_bytes()).hexdigest()
            rows = SYNC.load_source_rows(database)
            document, published = SYNC.build_outputs(
                rows, {row.title_id: 4096 for row in rows}, source_hash
            )
            catalog.write_bytes(SYNC._json_bytes(document))
            state.write_bytes(SYNC._json_bytes(published))

            connection = sqlite3.connect(database)
            connection.execute(
                "UPDATE homebrews SET md5 = ? WHERE id = ?", ("d" * 32, "BTWO00002")
            )
            connection.execute(
                "UPDATE homebrews SET md5 = ? WHERE id = ?", ("e" * 32, "CTHR00003")
            )
            connection.commit()
            connection.close()

            def partial_probe(request: Any) -> FakeResponse:
                if request.full_url.endswith("BTWO00002"):
                    return FakeResponse(
                        request.full_url, SYNC.PKG_MAGIC, status=206, headers={
                            "Content-Range": "bytes 0-3/8192",
                            "Content-Length": "4",
                            "Content-Type": "application/octet-stream",
                        },
                    )
                if request.full_url.endswith("CTHR00003"):
                    raise urllib.error.URLError("temporary failure")
                self.fail("unchanged published package was probed")

            with self.assertRaisesRegex(SYNC.SyncError, "package probes failed"):
                SYNC.synchronize(
                    database, catalog, state, POLICY,
                    offline=False, request_delay=0, opener=FakeOpener(partial_probe),
                    sleeper=lambda seconds: None,
                )
            self.assertEqual(
                set(SYNC.load_state(work_state, work=True).packages), {"BTWO00002"}
            )

    def test_catalog_publication_failure_rolls_back_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.json"
            state = root / "state.json"
            old_document, old_state = SYNC.build_outputs(
                [source_row()], {"TEST00001": 4096}, "a" * 32
            )
            new_document, new_state = SYNC.build_outputs(
                [source_row()], {"TEST00001": 8192}, "b" * 32
            )
            catalog.write_bytes(SYNC._json_bytes(old_document))
            state.write_bytes(SYNC._json_bytes(old_state))
            original_catalog = catalog.read_bytes()
            original_state = state.read_bytes()
            atomic_write = SYNC._atomic_write

            def fail_catalog(path: Path, payload: bytes) -> None:
                if path == catalog:
                    raise OSError("simulated catalog replace failure")
                atomic_write(path, payload)

            with mock.patch.object(SYNC, "_atomic_write", side_effect=fail_catalog):
                with self.assertRaisesRegex(OSError, "simulated"):
                    SYNC._publish_outputs(catalog, state, new_document, new_state)
            self.assertEqual(catalog.read_bytes(), original_catalog)
            self.assertEqual(state.read_bytes(), original_state)

    def test_generated_state_and_request_delay_are_bounded(self) -> None:
        with mock.patch.object(SYNC, "MAX_STATE_BYTES", 32):
            with self.assertRaisesRegex(SYNC.SyncError, "generated state exceeds"):
                SYNC.build_outputs([source_row()], {"TEST00001": 4096}, "a" * 32)
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "state.json"
            entry = SYNC._state_entry(source_row(), 4096)
            oversized = {
                "schema": 1,
                "source_db_md5": "a" * 32,
                "packages": {
                    f"TSTX{index:05d}": dict(entry)
                    for index in range(SYNC.MAX_SOURCE_ROWS + 1)
                },
            }
            state.write_bytes(SYNC._json_bytes(oversized))
            with self.assertRaisesRegex(SYNC.SyncError, "at most 512"):
                SYNC.load_state(state)
            state.write_bytes(SYNC._json_bytes({
                "schema": True, "source_db_md5": "a" * 32, "packages": {},
            }))
            with self.assertRaisesRegex(SYNC.SyncError, "unsupported state schema"):
                SYNC.load_state(state)
        with mock.patch.object(SYNC, "synchronize") as synchronize:
            with mock.patch("sys.stderr", new=io.StringIO()):
                self.assertEqual(SYNC.main(["--request-delay", "nan"]), 1)
            with mock.patch("sys.stderr", new=io.StringIO()):
                self.assertEqual(SYNC.main(["--offline", "--reprobe-all"]), 1)
            synchronize.assert_not_called()

    def test_verifier_rejects_catalog_state_policy_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.json"
            state = root / "state.json"
            document, cached = SYNC.build_outputs(
                [source_row()], {"TEST00001": 4259840}, "c" * 32
            )
            catalog.write_bytes(SYNC._json_bytes(document))
            state.write_bytes(SYNC._json_bytes(cached))
            self.assertEqual(SYNC.verify_outputs(catalog, state, POLICY), 1)

            cached["source_db_md5"] = "C" * 32
            state.write_bytes(SYNC._json_bytes(cached))
            with self.assertRaisesRegex(SYNC.SyncError, "invalid source_db_md5"):
                SYNC.verify_outputs(catalog, state, POLICY)
            cached["source_db_md5"] = "c" * 32

            cached["packages"]["TEST00001"]["platform_marker"] = "PS5 SUPPORTED"
            state.write_bytes(SYNC._json_bytes(cached))
            with self.assertRaisesRegex(SYNC.SyncError, "not canonical"):
                SYNC.verify_outputs(catalog, state, POLICY)
            cached["packages"]["TEST00001"]["platform_marker"] = ""

            cached["packages"]["TEST00001"]["app_type"] = "media"
            state.write_bytes(SYNC._json_bytes(cached))
            with self.assertRaisesRegex(SYNC.SyncError, "violates policy"):
                SYNC.verify_outputs(catalog, state, POLICY)


if __name__ == "__main__":
    unittest.main()
