#!/bin/sh
# Build the static arm64 shim and the third-party license bundle that must
# travel with it (BSD/MIT/ISC/Apache-2.0 attribution).
set -eu
cd "$(dirname "$0")"
export CGO_ENABLED=0 GOOS=linux GOARCH=arm64
go build -trimpath -ldflags='-s -w' -o flex-tailnet-shim .
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
