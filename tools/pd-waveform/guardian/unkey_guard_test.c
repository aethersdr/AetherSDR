#define main unkey_guard_program_main
#include "unkey_guard.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static const char kReady[] = "S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1";
static const char kTransmit[] = "S1234|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1";
static void bytes(struct unkey_state* s, const char* text)
{
    struct unkey_input input = {0};
    for (size_t i = 0; text[i]; ++i) { feed(s, &input, (const unsigned char*)text + i, 1); }
}
int main(void)
{
    struct unkey_state s = {.client = 0x1234, .owner = 0x5678, .query = kRefresh};
    parse(&s, kReady); assert(s.valid && s.idle && !s.software_owner);
    parse(&s, kTransmit); assert(s.valid && !s.idle && s.software_owner);
    parse(&s, "S9999|interlock tx_client_handle=0x00005678 state=READY reason= source= tx_allowed=1");
    assert(s.software_owner); // Unrelated client's status cannot change this connection's view.
    parse(&s, "S0|interlock tx_client_handle=0x00009999 state=TRANSMITTING reason= source=SW tx_allowed=1");
    assert(s.valid && !s.software_owner && !s.idle);
    parse(&s, "S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=MIC tx_allowed=1");
    assert(s.valid && !s.software_owner);
    parse(&s, "S0|interlock tx_client_handle=0x00005678 state=UNKEY_REQUESTED reason= source= tx_allowed=1");
    assert(s.valid && s.unkeying && !s.software_owner);
    const char* bad[] = {
        "S0|interlock state=TRANSMITTING source=SW",
        "S0|interlock tx_client_handle=0x5678 state=TRANSMITTING reason= source=SW tx_allowed=1",
        "S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING state=READY reason= source=SW tx_allowed=1",
        "S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW source=MIC tx_allowed=1",
        "S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=garbage",
        "S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\177"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        parse(&s, kTransmit); assert(s.software_owner); parse(&s, bad[i]); assert(!s.valid && !s.software_owner);
    }
    parse(&s, kTransmit); parse(&s, "S0|interlock timeout=20000"); assert(s.software_owner);
    parse(&s, "S0|interlock band 4 state=READY"); assert(s.software_owner);
    bytes(&s, "S0|waveform installed_list=Example\177\177v1\n"); assert(s.software_owner);
    // An embedded NUL is fed explicitly; it cannot become a valid completed line.
    struct unkey_input in = {0};
    const unsigned char nul[] = "S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=S\0W tx_allowed=1\n";
    feed(&s, &in, nul, sizeof(nul) - 1); assert(!s.valid);
    parse(&s, "R910001|0|"); assert(!s.acknowledged);
    parse(&s, "R910002|0|"); assert(s.acknowledged && !s.rejected);
    s.acknowledged = 0; parse(&s, "R910002|1|"); assert(!s.acknowledged && s.rejected);
    s.rejected = 0; parse(&s, "R910002|0junk|"); assert(s.rejected);
    uint32_t h; assert(handle("0xFFFFFFFF", &h) && h == UINT32_MAX);
    assert(!handle("0x0000000g", &h) && !handle("0x000000001", &h));
    assert(!before_deadline(-2, 2000));
    int duration = 0;
    assert(bounded_duration("1", &duration) && duration == 1);
    assert(bounded_duration("15000", &duration) && duration == 15000);
    const char* invalid_durations[] = {"", "0", "01", "15001", "-1", "+1", "1 ", "999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid_durations) / sizeof(invalid_durations[0]); ++i) {
        assert(!bounded_duration(invalid_durations[i], &duration));
    }
    struct bounded_deadline deadline = {0};
    assert(!deadline_start(&deadline, 0, 1000));
    assert(!deadline_start(&deadline, 15001, 1000));
    assert(!deadline_start(&deadline, 1000, INT64_MAX - 999));
    assert(!deadline_start(&deadline, 1000, -1));
    assert(deadline_start(&deadline, 1000, 1000));
    assert(deadline_wait(&deadline, 1000) == 500);
    assert(deadline_wait(&deadline, 1700) == 300);
    assert(deadline_wait(&deadline, 1999) == 1);
    assert(deadline_wait(&deadline, 2000) == 0 && deadline.due == 2000);
    assert(deadline_start(&deadline, 1000, 1000));
    assert(deadline_wait(&deadline, 1001) == 500 && deadline_wait(&deadline, 1000) == 0);
    struct unkey_state bounded = {.client = 0x1234, .owner = 0x5678, .bounded = 1};
    parse(&bounded, kReady); assert(!bounded.owned_seen && !bounded.observation_ended);
    parse(&bounded, kTransmit); assert(bounded.owned_seen && !bounded.observation_ended);
    parse(&bounded, kTransmit); assert(!bounded.observation_ended);
    parse(&bounded, kReady); assert(bounded.observation_ended);
    parse(&bounded, kTransmit); assert(bounded.observation_ended); // Never adopt a second cycle.
    bounded = (struct unkey_state){.client = 0x1234, .owner = 0x5678, .bounded = 1};
    parse(&bounded, "S0|interlock tx_client_handle=0x00005678 state=UNKEY_REQUESTED reason= source= tx_allowed=1");
    assert(bounded.observation_ended); // Also retire if key-on status was missed.
    bounded = (struct unkey_state){.client = 0x1234, .owner = 0x5678, .bounded = 1};
    parse(&bounded, kTransmit); parse(&bounded, bad[0]); parse(&bounded, kTransmit);
    assert(bounded.observation_ended); // Incomplete ownership cannot bridge two intervals.
    puts("unkey guard parser/ownership tests passed");
    return 0;
}
