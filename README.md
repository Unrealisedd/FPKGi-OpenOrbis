# FPKGi Native

FPKGi Native is a small OpenOrbis PS4 homebrew client for lawful homebrew and owner-supplied FPKGs. It uses no Unity runtime and no proprietary Sony SDK.

## Supported scope

Version 1.00 provides catalog browsing, HTTP/HTTPS catalog and package transfer, exact size/PS4 PKG magic/optional SHA-256 verification, BGFT installation, AppInstUtil completion checks, and SDL2 progress/error UI. Cross downloads and installs the selected item, Triangle refreshes the catalog, and Circle exits only while idle.

Full Unity visual parity, a durable queue, transfer resume/retry, and cancellation are outside this native-core release. The bundled catalog is intentionally empty and the application does not provide package content, credentials, entitlement bypasses, or commercial-package links.

Firmware 14.00 runtime behavior remains unverified until the hardware qualification procedure is run on a console. Build success is not hardware qualification.

## Catalog and storage

The maintained remote catalog is:

```text
https://raw.githubusercontent.com/Unrealisedd/FPKGi-OpenOrbis/main/catalog/catalog.json
```

Validated catalog cache and downloads use `/data/FPKGi`. A download is written to a `.part` file, bounded by the declared size, and renamed to `.pkg` only after exact size, `7F 43 4E 54` magic, and optional SHA-256 verification. Failed or ambiguous transfers/installations retain local data for inspection. See [docs/catalog.md](docs/catalog.md).

HTTPS validation uses the pinned Mozilla CA root bundle in `assets/cacert.pem`; automatic redirects are disabled so every redirect is revalidated and HTTPS-to-HTTP downgrade is rejected. The bundle SHA-256 is `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.

## Host tests

Requirements: Python 3.10+ and a C++11 compiler.

```sh
make host-tests
python3 scripts/validate_catalog.py catalog/catalog.json
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

The package output is `FPKGi-Native-v1.00.pkg`. CI downloads the exact v0.5.4 asset URL, verifies this digest, builds `eboot.bin` and the FPKG, validates it with LibOrbisPkg `pkg_validate`, records its SHA-256, and uploads build artifacts. CI does not publish releases.

## PS4 installation

1. Enable the PS4 debug settings supplied by the console's supported homebrew environment.
2. Copy `FPKGi-Native-v1.00.pkg` to USB storage or transfer it with a trusted local package sender.
3. Open **Debug Settings → Game → Package Installer** and select the package.
4. Launch **FPKGi Native**. Use only catalog entries and FPKGs you created or are authorized to use.

The runtime requires the privileged AppInstUtil and BGFT services exposed by a compatible homebrew environment. Native hexadecimal result codes remain visible in the UI for diagnosis.

## License and attribution

FPKGi Native is licensed under GNU GPL v2.0; see [LICENSE](LICENSE). Native source portions were derived from the MIT-licensed OrbisShelf project; its preserved notice is in [LICENSES/OrbisShelf-MIT.txt](LICENSES/OrbisShelf-MIT.txt). The packaged Mozilla CA root bundle is under MPL 2.0; see [LICENSES/Mozilla-CA-MPL-2.0.txt](LICENSES/Mozilla-CA-MPL-2.0.txt). OpenOrbis and LibOrbisPkg provide the open toolchain and packaging implementation used by the build. See [NOTICE](NOTICE).
