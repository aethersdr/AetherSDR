# CTR2 USB link: firmware reference implementation

`ctr2_link.h` and `ctr2_link.c` implement both ends of the CTR2 USB link
(wire format version 0) in portable C99. They have no heap use and no
dependencies beyond `<stdint.h>` and `<stddef.h>`.

These two files are **MIT-licensed**, unlike the rest of AetherSDR (GPLv3), so
they can be copied into the controller firmware as-is or used as a guide.

The specification, message flow and hex test vectors are in
[`docs/ctr2-usb-relay-design.md`](../../docs/ctr2-usb-relay-design.md).
AetherSDR's test suite compiles these files and checks them against the
application's own codec (`tests/ctr2_hid_reference_test.cpp`), so they stay
correct as the host side changes.

## Minimal use

```c
static ctr2_tx tx;
static ctr2_rx rx;
static int waiting_for_host;  /* after HELLO, until READY or CLOSED */

static void send_report(void *ctx, const uint8_t r[CTR2_REPORT_BYTES])
{
    uint8_t buf[1 + CTR2_REPORT_BYTES] = {CTR2_REPORT_ID};
    memcpy(buf + 1, r, CTR2_REPORT_BYTES);
    usb_hid_send(buf, sizeof buf);          /* your USB stack */
}

void link_start(void)                       /* entering USB mode, or after an error */
{
    ctr2_rx_reset(&rx);
    ctr2_tx_reset(&tx);
    waiting_for_host = 1;
    ctr2_tx_send(&tx, CTR2_TYPE_HELLO, NULL, 0, send_report, NULL);
}

void on_hid_report(const uint8_t r[CTR2_REPORT_BYTES])  /* after report ID */
{
    uint8_t type;
    const uint8_t *data;
    uint16_t len;
    if (waiting_for_host) {
        /* Skip anything left over from before HELLO; data reports never
         * start with 0xFF, so only a real READY/CLOSED header ends this. */
        if (r[0] != CTR2_MARKER || (r[3] != CTR2_TYPE_READY && r[3] != CTR2_TYPE_CLOSED)) {
            return;
        }
        waiting_for_host = 0;
        ctr2_rx_reset(&rx);
    }
    switch (ctr2_rx_feed(&rx, r, &type, &data, &len)) {
    case CTR2_RX_MESSAGE:
        if (type == CTR2_TYPE_READY)  radio_connected();       /* = Wi-Fi TCP connect */
        if (type == CTR2_TYPE_CLOSED) radio_disconnected();    /* = Wi-Fi TCP close   */
        if (type == CTR2_TYPE_DATA)   radio_bytes(data, len);  /* same parser as TCP  */
        if (type == CTR2_TYPE_DATAGRAM)                        /* one UDP datagram    */
            radio_udp((uint16_t)((data[0] << 8) | data[1]), data + 2, (uint16_t)(len - 2));
        break;
    case CTR2_RX_ERROR:
        radio_disconnected();
        link_start();                       /* restart the link */
        break;
    case CTR2_RX_PENDING:
        break;
    }
}

void send_to_radio(const uint8_t *cmd, uint16_t len)  /* only after READY; <= 512 */
{
    ctr2_tx_send(&tx, CTR2_TYPE_DATA, cmd, len, send_report, NULL);
}

void send_udp_to_radio(uint16_t port, const uint8_t *d, uint16_t len)  /* <= 1472 */
{
    /* e.g. the UDP registration datagram to port 4992 */
    ctr2_tx_send_datagram(&tx, port, d, len, send_report, NULL);
}
```
