# FPKGi Native

FPKGi Native is a small OpenOrbis PS4 homebrew client for lawful homebrew and owner-supplied FPKGs. It uses no Unity runtime and no proprietary Sony SDK.

## Supported scope

Version 1.03 provides catalog browsing, HTTP/HTTPS catalog and package transfer, exact size/PS4 PKG magic/SHA-256 verification (required for HTTP packages, optional for HTTPS packages), BGFT installation, AppInstUtil completion checks, and SDL2 progress/error UI. Startup presents the UI before catalog storage initialization or network access; Cross downloads and installs the selected item, Triangle explicitly refreshes the catalog, and Circle requests a clean System Service exit only while idle.

The maintained catalog is a deterministic, policy-filtered snapshot of the official PS4 HB-Store database. It includes supported homebrew categories and excludes PS5-only records, retail-style `CUSA`/`PPSA` IDs, proprietary media/dev-menu/theme/mod categories, and explicit deny-list entries that are outside scope or lack a probeable direct package. It does not provide credentials, entitlement bypasses, or direct commercial-package links. Full Unity visual parity, a durable queue, transfer resume/retry, and cancellation remain outside this native core.

Firmware 14.00 hardware testing found that version 1.00 returned CE-34878-0 before presenting a frame. Version 1.01 presented its UI, then exposed two target-only defects: OpenOrbis `lstat` returns `ENOSYS` on PS4 and disabled storage validation, while the inherited Circle path tore down SDL and returned from `main` instead of requesting a PS4 System Service exit. Version 1.02 replaced both paths. Version 1.03 adds the HB-Store catalog path and source-specific request identity; its final storage, exit, refresh, and install behavior remains hardware-unverified until qualification is rerun. Build success is not hardware qualification.

## Catalog and storage

The maintained remote catalog is:

```text
https://raw.githubusercontent.com/Unrealisedd/FPKGi-OpenOrbis/main/catalog/catalog.json
```

`scripts/sync_hb_store.py` reads the permitted `https://api.pkg-zone.com/store.db` source over verified TLS, checks the source's before/after database fingerprint, validates the ordinary bounded-work SQLite table and every selected record, and probes bytes `0-3` of every new or upstream-changed package. A probe must return HTTP 206, an exact total size in `Content-Range`, `application/octet-stream`, and PS4 PKG magic. Unchanged published probes are reusable only while title version and upstream package MD5 still match; records without an MD5 additionally require the whole database fingerprint to match. `--reprobe-all` performs a full audit and may resume from a recent fingerprint-bound work cache. The adapter rate-limits requests, validates the generated FPKGi JSON/state pair, and publishes the runtime catalog atomically last only after the complete run succeeds. Normal application/build CI consumes the committed snapshot and never contacts PKG-Zone.

The source database does not publish SHA-256 digests. Generated package URLs are therefore HTTPS-only and retain FPKGi's existing HTTPS policy: verified TLS, no downgrade redirects, exact probed byte size, and PS4 PKG magic are mandatory; SHA-256 remains mandatory for any HTTP package URL. See [docs/catalog.md](docs/catalog.md) for the source policy and operator flow.

Validated catalog cache and downloads use private `0700` directories under `/data/FPKGi`; each directory is opened with no-follow/directory flags and validated through its descriptor because OpenOrbis PS4 `lstat` is unavailable. Cache, staging, and verification opens reject final-component symlinks. A download is written to a `.part` file, bounded by the declared size, and renamed to `.pkg` only after exact size, `7F 43 4E 54` magic, and the required SHA-256 policy. Failed or ambiguous transfers/installations retain local data for inspection. See [docs/catalog.md](docs/catalog.md).

HTTPS validation uses the pinned Mozilla CA root bundle in `assets/cacert.pem`; automatic redirects are disabled so every redirect is revalidated and HTTPS-to-HTTP downgrade is rejected. The bundle SHA-256 is `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.

## Host tests

Requirements: Python 3.10+ and a C++11 compiler.

```sh
make host-tests
python3 scripts/validate_catalog.py catalog/catalog.json
python3 scripts/sync_hb_store.py --verify-output
```

## Local OpenOrbis build

Use OpenOrbis PS4 Toolchain v0.5.4 from `https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz`. The required archive SHA-256 is:

```text
3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526
```

After verifying and extracting the archive:

```sh
export OO_PS4_TOOLCHAIN=/absolute/path/to/openorbis
make host-tests
make
```

Default `make` is the maintained release path and requires a matching nonempty HB-Store state snapshot. To package a schema-valid owner-supplied `catalog/catalog.json` instead, use the explicit `make FPKGi-Native-v1.03.pkg` target; it retains the generic catalog safety validator without imposing HB-Store source policy. Release CI always runs the stricter maintained-snapshot verifier before packaging.

The package output is `FPKGi-Native-v1.03.pkg`. CI downloads the exact v0.5.4 asset URL, verifies this digest, builds `eboot.bin` and the FPKG, validates it with LibOrbisPkg `pkg_validate`, records its SHA-256, and uploads build artifacts. CI does not publish releases.

## PS4 installation

1. Enable the PS4 debug settings supplied by the console's supported homebrew environment.
2. Copy `FPKGi-Native-v1.03.pkg` to USB storage or transfer it with a trusted local package sender.
3. Open **Debug Settings → Game → Package Installer** and select the package.
4. Launch **FPKGi Native**. Use only catalog entries and FPKGs you created or are authorized to use.

The runtime requires the privileged AppInstUtil and BGFT services exposed by a compatible homebrew environment. Native hexadecimal result codes remain visible in the UI for diagnosis.

Version 1.03 appends bounded boot-stage lines to `FPKGi-Native-boot.log` at `/mnt/usb0`, `/mnt/usb1`, and `/data` when those paths are writable. Before each qualification launch, delete or rename any existing USB log; then a missing fresh `ENTRY - VERSION 1.03` proves that execution did not reach `main`. `EXIT_REQUESTED` records Circle input and `EXIT_RETURNED` records an unexpected API return; only a visible return to the PS4 shell without CE-34878-0 qualifies as a clean exit. Keeping that USB connected makes pre-UI SDL, storage, refresh, and exit failures observable without weakening certificate or package verification.

## License and attribution

FPKGi Native is licensed under GNU GPL v2.0; see [LICENSE](LICENSE). Native source portions were derived from the MIT-licensed OrbisShelf project; its preserved notice is in [LICENSES/OrbisShelf-MIT.txt](LICENSES/OrbisShelf-MIT.txt). The packaged Mozilla CA root bundle is under MPL 2.0; see [LICENSES/Mozilla-CA-MPL-2.0.txt](LICENSES/Mozilla-CA-MPL-2.0.txt). HB-Store/PKG-Zone catalog metadata and package endpoints are operated by Darksoftware LLC and LightningMods and are consumed under the maintainer's written permission; no HB-Store source code is copied into this GPLv2 project. OpenOrbis and LibOrbisPkg provide the open toolchain and packaging implementation used by the build. See [NOTICE](NOTICE).
