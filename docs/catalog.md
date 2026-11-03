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

The repository's bundled and maintained remote catalogs are intentionally empty. An empty `DATA` object is valid; after a successful refresh the UI reports `REMOTE CATALOG VALIDATED: 0 PACKAGES` until authorized entries are published.

The destination filename is derived as `TITLEID_v<hex-encoded-version>_Sanitized Name.pkg` under `/data/FPKGi/downloads`; metadata cannot supply a path, and different accepted title/version identities cannot collide. `/data/FPKGi` and its download directory are opened with no-follow/directory flags, validated with descriptor-based `fstat`, and forced to mode `0700`; OpenOrbis PS4 `lstat` is not used because its `fstatat` backend returns `ENOSYS`. Writable files use mode `0600` and no-follow opens. The transfer first uses the same filename with `.part`. Known free space must cover the declared size plus a 64 MiB reserve. An unavailable free-space reading is shown as unknown and does not become a false zero-space failure.

A package reaches BGFT only after exact size, PS4 PKG magic `7F 43 4E 54`, and the catalog's SHA-256 policy pass. Existing final files are reused only after the same verification. BGFT task errors and AppInstUtil result codes remain visible. Installed or updating titles are not overwritten. The verified local package is deleted only after BGFT reports full transfer, preparation, and local-copy completion and repeated AppInstUtil observations confirm `exists && !updating`; all failure and ambiguous paths keep it.
