#!/usr/bin/env bash
# Split debug info out of release binaries into a symbol directory, before
# they are packaged (docs/debugging-crashes.md).
#
#   split-debug-symbols.sh OUT_DIR BINARY [BINARY...]
#
# Linux: each binary must carry a GNU build ID, which is what gdb, eu-addr2line,
# coredumpctl and debuginfod key on. Its DWARF moves to
# OUT_DIR/.build-id/<xx>/<rest>.debug (gdb's own layout, so pointing
# debug-file-directory at OUT_DIR is enough) and is removed from the binary.
# The build ID survives the later install, patchelf and linuxdeploy strip.
#
# macOS: dsymutil collects the DWARF from the object files into
# OUT_DIR/<name>.dSYM, keyed by the binary's UUID, then strip -S drops the
# debug map so the shipped binary carries no build paths. Run it before
# macdeployqt and codesign: the signature must cover the final bytes.
#
# Both write OUT_DIR/SYMBOLS.txt: one line per binary with its ID, so a crash
# report's build ID or UUID can be matched to an archive without unpacking it.
# A binary that does not exist is an error; a caller that builds an optional
# helper checks for it first.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 OUT_DIR BINARY [BINARY...]" >&2
    exit 2
fi
out=$1
shift
mkdir -p "$out"
manifest="$out/SYMBOLS.txt"
: > "$manifest"

case "$(uname -s)" in
Linux)
    for bin in "$@"; do
        [ -f "$bin" ] || { echo "error: $bin not found" >&2; exit 1; }
        id=$(readelf -n "$bin" | sed -n 's/^ *Build ID: *\([0-9a-f]*\)$/\1/p' | head -1)
        if [ -z "$id" ]; then
            echo "error: $bin has no GNU build ID; its symbols could never be matched" >&2
            exit 1
        fi
        if ! readelf -S "$bin" | grep -q '\.debug_info'; then
            echo "error: $bin has no .debug_info; was it built with -g?" >&2
            exit 1
        fi
        dbg="$out/.build-id/${id:0:2}/${id:2}.debug"
        mkdir -p "$(dirname "$dbg")"
        objcopy --only-keep-debug --compress-debug-sections "$bin" "$dbg"
        objcopy --strip-debug --add-gnu-debuglink="$dbg" "$bin"
        printf '%s  build-id %s  %s\n' "$(basename "$bin")" "$id" ".build-id/${id:0:2}/${id:2}.debug" >> "$manifest"
        echo "split $bin -> $dbg"
    done
    ;;
Darwin)
    for bin in "$@"; do
        [ -f "$bin" ] || { echo "error: $bin not found" >&2; exit 1; }
        name=$(basename "$bin")
        dsym="$out/$name.dSYM"
        dsymutil "$bin" -o "$dsym"
        dwarf="$dsym/Contents/Resources/DWARF/$name"
        if [ ! -s "$dwarf" ] || ! dwarfdump --debug-info "$dwarf" | grep -q 'DW_TAG_compile_unit'; then
            echo "error: $dsym holds no DWARF; was $bin built with -g?" >&2
            exit 1
        fi
        # One UUID per architecture slice; the binary's and the dSYM's must agree.
        bin_uuids=$(dwarfdump --uuid "$bin" | awk '{print $2, $3}' | sort)
        dsym_uuids=$(dwarfdump --uuid "$dsym" | awk '{print $2, $3}' | sort)
        if [ "$bin_uuids" != "$dsym_uuids" ]; then
            echo "error: UUID mismatch between $bin and $dsym" >&2
            exit 1
        fi
        strip -S "$bin"
        while read -r uuid arch; do
            printf '%s  uuid %s %s  %s\n' "$name" "$uuid" "$arch" "$name.dSYM" >> "$manifest"
        done <<< "$dsym_uuids"
        echo "split $bin -> $dsym"
    done
    ;;
*)
    echo "error: unsupported platform $(uname -s); Windows symbols are staged by packaging/windows/stage-debug-symbols.ps1" >&2
    exit 1
    ;;
esac

cat "$manifest"
