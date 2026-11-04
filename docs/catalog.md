# Catalog contract

FPKGi Native accepts one JSON object named `DATA`. Each `DATA` key is the package URL and each value is package metadata:

```json
{
  "DATA": {
    "http://lan.example/owner-homebrew.pkg": {
      "title_id": "FPKG00001",
      "region": "ALL",
      "name": "Owner Homebrew",
      "version": "01.00",
      "release": "01-01-2026",
      "size": "8388608",
      "min_fw": "5.05",
      "cover_url": null,
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  }
}
```

`title_id`, `name`, `version`, and `size` are required. `title_id` must match `[A-Z]{4}[0-9]{5}`. `version` is 1-16 letters, digits, dots, hyphens, or underscores, cannot be `.`, and cannot contain `..`. `size` is the exact nonzero byte length written as a base-10 string and must fit an unsigned 64-bit integer; it is never parsed through floating point. `sha256`, when present, is exactly 64 hexadecimal characters and is normalized to lowercase at runtime. It is mandatory for an HTTP package URL and optional for an HTTPS package URL.

`region`, `release`, and `min_fw` may be bounded strings or `null`. `cover_url` may be absent, `null`, or an absolute safe HTTP/HTTPS URL. Unknown fields, duplicate JSON keys, duplicate title-ID/version identities, oversized or invalid UTF-8 strings, control characters, malformed URLs, credentials, and fragments are rejected. Catalogs are limited to 2 MiB, 4,096 package entries, 16 JSON nesting levels, and 65,536 JSON values.

Package and cover URLs may use HTTPS or explicit HTTP for a controlled LAN source. Redirects are limited to six. Automatic platform redirects and protocol-relative redirects are disabled, and HTTPS-to-HTTP downgrade is rejected. No authorization header is sent.

The bundled and maintained remote catalog is generated from the permitted official PS4 HB-Store source. An empty `DATA` object remains valid for owner-supplied catalogs, but release CI requires the maintained HB-Store catalog and its source state to be nonempty and policy-consistent.

At startup, a cached or bundled catalog containing the reserved `HB-Store` region marker receives the source-specific nonempty/origin checks below. A generic owner-supplied catalog—including an empty one—retains the base contract; use a different region marker for owner content.

## HB-Store source adapter

`scripts/sync_hb_store.py` is the only component that reads HB-Store's SQLite format. The PS4 runtime still consumes this document's bounded JSON contract; it does not ship SQLite or parse upstream records.

The live adapter performs this fail-closed sequence:

1. Fetch the official database fingerprint, `store.db`, and the fingerprint again over verified HTTPS with redirects disabled and a minimum 1.0-second request interval.
2. Require a stable upstream fingerprint, a matching database payload, one ordinary bounded-work `homebrews` table with the expected columns, 1-512 rows, unique title IDs, bounded metadata, and package identities on `api.pkg-zone.com/download.php?tid=<TITLE_ID>`.
3. Apply `catalog/hb_store_policy.json`. Supported PS4 homebrew types are allowed. PS5-only markers, audited Sony retail/product-code prefixes, unapproved categories, and explicit deny-list IDs are excluded. Explicit allow-list entries remain auditable exceptions.
4. Reuse a published exact-size probe only while the title version and upstream package MD5 match. A record without an upstream MD5 is reusable only while the whole database fingerprint is unchanged. New or changed packages are probed; `--reprobe-all` ignores published probes for a full online audit. A recent, Git-ignored work cache may resume interrupted online work, but offline mode never promotes recovery-cache entries.
5. For every cache miss, request only bytes `0-3`. Require HTTP 206, `Content-Range: bytes 0-3/<exact-total>`, a four-byte body, `application/octet-stream`, and `7F 43 4E 54`.
6. Persist each successful cache-miss probe in the ignored `hb_store_state.json.work` recovery file under a distinct probe-work schema; copies of published state are rejected as recovery input. Generate canonical sorted JSON and run the executable catalog validator. Upstream and validation failures leave the published catalog and state unchanged. Final publication writes the state atomically and the runtime catalog atomically last; a filesystem failure cannot expose a partial catalog, and catalog/state parity verification detects an interrupted two-file publication.

`WILI00000` is the sole app-type exception. HB-Store labels wiliwili as `Media`, but it is a [GPL-3.0 open-source third-party client with a PS4 target](https://github.com/xfangfang/wiliwili), not a proprietary media-service package. The explicit ID keeps that reviewable exception from admitting the rest of the media category.

The explicit deny-list records two review boundaries. `GBTX00000` is an external storefront advertising a large third-party content catalog and is outside FPKGi's direct-homebrew scope. The direct URLs for `APOL00004`, `AZIF00001`, `CUSB99997`, `DELT00001`, `FLTZ00001`, `FLTZ00003`, `FMSX00000`, `LAPY10014`, `LAPY10020`, `LAPY10021`, `LAPY10022`, `LAPY10024`, and `LAPY20001` repeatedly failed bounded range probes during integration. They remain excluded until each direct URL passes the same probe as every published entry.

The upstream database publishes MD5 package fingerprints but no SHA-256 values. MD5 is used only as a cache invalidation key; it is not presented as a cryptographic package guarantee. Generated package URLs are HTTPS-only, so the runtime enforces verified TLS, redirect policy, exact byte size, and PS4 PKG magic under the existing optional-SHA-for-HTTPS contract. HTTP package URLs from any other catalog still require SHA-256.

The normal build is offline and validates the committed snapshot:

```sh
python3 scripts/validate_catalog.py catalog/catalog.json
python3 scripts/sync_hb_store.py --verify-output
```

Default `make` additionally runs the maintained-snapshot parity check. An owner may replace `catalog/catalog.json` with a generic contract-compliant catalog and invoke the explicit `make FPKGi-Native-v1.03.pkg` target; that path runs `validate_catalog.py` but intentionally does not require HB-Store state. Release CI always runs both validators before the package target.

An authorized operator refreshes the snapshot with:

```sh
python3 scripts/sync_hb_store.py
```

The scheduled `sync-hb-store.yml` workflow sends `TERM` to each of three attempts at 25 minutes, allows up to 10 seconds of kill grace, and runs inside a 90-minute job. It retains only atomically written probe-work entries between attempts and commits only validated changes. Scheduled runs probe new or changed packages; a manual dispatch can select `reprobe_all` for a full audit. Access and redistribution require upstream permission; changing the endpoint, request identity, rate, policy, or reuse scope requires rechecking that grant.

The destination filename is derived as `TITLEID_v<hex-encoded-version>_Sanitized Name.pkg` under `/data/FPKGi/downloads`; metadata cannot supply a path, and different accepted title/version identities cannot collide. `/data/FPKGi` and its download directory are opened with no-follow/directory flags, validated with descriptor-based `fstat`, and forced to mode `0700`; OpenOrbis PS4 `lstat` is not used because its `fstatat` backend returns `ENOSYS`. Writable files use mode `0600` and no-follow opens. The transfer first uses the same filename with `.part`. Known free space must cover the declared size plus a 64 MiB reserve. An unavailable free-space reading is shown as unknown and does not become a false zero-space failure.

A package reaches BGFT only after exact size, PS4 PKG magic `7F 43 4E 54`, and the catalog's SHA-256 policy pass. Existing final files are reused only after the same verification. BGFT task errors and AppInstUtil result codes remain visible. Installed or updating titles are not overwritten. The verified local package is deleted only after BGFT reports full transfer, preparation, and local-copy completion and repeated AppInstUtil observations confirm `exists && !updating`; all failure and ambiguous paths keep it.
