# Vendored Opus for RADE

This directory stores the prepared local Opus snapshot used by RADE-enabled
builds. The build consumes `opus-rade-prepared.tar.gz` via CMake
`ExternalProject`, so no network download is required at configure or build
time.

- Upstream Opus commit: `940d4e5af64351ca8ba8390df3f555484c567fbb`
- Archive SHA256:
  `118539b194c82b3aedb843b4ef8499f223bd12c433871b80c4cbb853bcd463a7`
- RADE export patch applied from:
  `third_party/radae/src/opus-nnet.h.diff`
- Generated DNN data included from model hash:
  `4ed9445b96698bad25d852e912b41495ddfa30c8dbc8a55f9cde5826ed793453`

The prepared archive intentionally omits `dnn/models/*.pth`. The build only
needs the generated `*_data.c` / `*_data.h` sources that are already baked into
the archive.

To refresh this dependency, download the pinned upstream snapshot, extract the
matching Opus model tarball into the source tree, apply the RADE patch, remove
`dnn/models/`, and recreate `opus-rade-prepared.tar.gz`.
