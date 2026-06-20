# Shared SHA256 verifier for setup-script downloads — supply-chain hardening (#3665).
# Source this, then: verify_sha256 <file> <expected-hex>
# Fails hard (returns non-zero) on mismatch or if no hasher is available, so a
# tampered/MITM'd download never reaches the extract/build step.
#
# Not meant to be executed directly — it only defines a function.

verify_sha256() {
    local file="$1" expected="$2" actual=""
    if [ ! -f "$file" ]; then
        echo "ERROR: verify_sha256: file not found: $file" >&2
        return 1
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$file" | cut -d' ' -f1)"
    elif command -v shasum >/dev/null 2>&1; then
        actual="$(shasum -a 256 "$file" | cut -d' ' -f1)"
    else
        echo "ERROR: verify_sha256: neither sha256sum nor shasum found" >&2
        return 1
    fi
    if [ "$actual" != "$expected" ]; then
        echo "ERROR: SHA256 mismatch for $file" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        return 1
    fi
    echo "Verified SHA256 of $(basename "$file")"
}
