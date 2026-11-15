#include "core/dsp/WdspChannel.h"

#include <iostream>
#include <utility>

namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
}

int main()
{
    using Channel = WdspChannel;
    check(!Channel::reserveChannels(0), "empty reservations are refused");
    check(!Channel::reserveChannels(33), "oversized reservations are refused");
    {
        auto all = Channel::reserveChannels(32);
        check(all && all->remaining() == 32, "reserve complete shared pool without planning");
        check(!Channel::reserveChannels(1), "pool reservation excludes another reservation");
        std::string error;
        check(!Channel::create({}, &error) && !error.empty(), "legacy create competes with reserved pool");
    }
    {
        auto first = Channel::reserveChannels(31);
        check(first.has_value(), "released batch restores pool");
        check(!Channel::reserveChannels(2), "partial batch refuses atomically");
        auto last = Channel::reserveChannels(1);
        check(last.has_value(), "failed batch did not consume last free slot");
        if (!first || !last) {
            return 1;
        }
        Channel::Reservation moved(std::move(*first));
        check(first->remaining() == 0 && moved.remaining() == 31, "move construction transfers sole ownership");
        moved = std::move(moved);
        check(moved.remaining() == 31, "self move preserves reservation");
        *last = std::move(moved);
        check(moved.remaining() == 0 && last->remaining() == 31, "move assignment releases prior slot");
        auto available = Channel::reserveChannels(1);
        check(available.has_value(), "move assignment returned previous reservation to actual pool");
    }
    {
        auto held = Channel::reserveChannels(31);
        std::string error;
        auto live = Channel::create({}, &error);
        check(live != nullptr, "ordinary create takes remaining unreserved slot");
        check(!Channel::reserveChannels(1), "ordinary channel holds its actual pool slot");
        if (!held || !live) {
            return 1;
        }
        Channel::Config invalid;
        invalid.inputBlockSize = 0;
        check(!Channel::create(invalid, *held, &error), "invalid config refuses reserved create");
        check(held->remaining() == 31, "failed create preserves its reservation");
        auto reservedLive = Channel::create({}, *held, &error);
        check(reservedLive && held->remaining() == 30, "reserved create transfers one slot into channel lifetime");
        check(!Channel::reserveChannels(1), "consuming reservation does not free a live channel ID");
        held.reset();
        auto rest = Channel::reserveChannels(30);
        check(rest.has_value(), "unconsumed reservation destruction releases exactly its remainder");
        live.reset();
        auto returned = Channel::reserveChannels(1);
        check(returned.has_value(), "ordinary channel destruction returns its own slot");
    }
    check(Channel::reserveChannels(32).has_value(), "all channels and reservations return to shared pool");
    std::cout << "WDSP reservation failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
