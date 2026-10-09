#!/bin/sh
# Build the static arm64 shim and the third-party license bundle that must
# travel with it (BSD/MIT/ISC/Apache-2.0 attribution).
set -eu
cd "$(dirname "$0")"
export CGO_ENABLED=0 GOOS=linux GOARCH=arm64
# The image bytes must not depend on the builder's machine. `sort` orders the
# LICENSES sections by locale, so pin it; and `go list` must not try to stamp
# VCS details from whatever checkout (or none) the tree sits in.
export LC_ALL=C GOFLAGS=-buildvcs=false
# -buildvcs=false: the binary must not record the commit or a dirty tree, or
# the pinned image could only be rebuilt from the exact checkout it came from.
go build -trimpath -buildvcs=false -ldflags='-s -w' -o flex-tailnet-shim .
{
  echo "flex-tailnet-shim is part of AetherSDR and is licensed under the GNU GPL v3."
  echo "It includes the following third-party Go modules, under their own licenses."
  # Modules come from vendor/ (RFC #6271 D1), where Go reports no module
  # directory; read each module's license from its vendored copy.
  go list -deps -f '{{if .Module}}{{.Module.Path}} {{.Module.Version}}{{end}}' . |
    sort -u | while read -r path version; do
      [ "$path" = "github.com/aethersdr/AetherSDR/tools/flex-tailnet-shim" ] && continue
      dir="vendor/$path"
      lic=$(ls "$dir" | grep -i -E '^(licen[cs]e|copying)' | head -1)
      echo
      echo "================================================================"
      echo "$path $version"
      echo "================================================================"
      cat "$dir/$lic"
      for n in $(ls "$dir" | grep -i '^notice'); do echo; cat "$dir/$n"; done
    done
  echo
  echo "================================================================"
  echo "Go standard library $(go env GOVERSION)"
  echo "================================================================"
  cat "$(go env GOROOT)/LICENSE"
} > LICENSES
echo "built flex-tailnet-shim ($(wc -c < flex-tailnet-shim) bytes) and LICENSES ($(grep -c '^====' LICENSES) separators)"

# The container image, reproducibly (RFC #6271 D2: AetherSDR pins its SHA-256).
VERSION="$(sed -n 's/^const shimVersion = "\(.*\)"$/\1/p' main.go)"
python3 -I mkimage.py flex-tailnet-shim LICENSES "$VERSION" "flex-tailnet-shim-$VERSION.tar.gz"
