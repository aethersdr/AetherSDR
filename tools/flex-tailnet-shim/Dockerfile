# Generic image for the radio's Docker waveform installer. It carries no
# secret: AetherSDR provisions the Tailscale auth key afterwards over the LAN.
#
#   ./build.sh            # builds flex-tailnet-shim + LICENSES for linux/arm64
#   docker buildx build --platform linux/arm64 \
#     --output type=oci,compression=gzip,dest=flex-tailnet-shim.tar.gz .
FROM scratch
COPY flex-tailnet-shim /flex-tailnet-shim
COPY LICENSES /LICENSES
LABEL com.flexradio.waveform.name=flex-tailnet-shim
LABEL com.flexradio.waveform.version=0.3.0
ENTRYPOINT ["/flex-tailnet-shim"]
