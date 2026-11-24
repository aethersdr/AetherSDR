#include "util/cancel.hpp"

namespace data2g::cancel {
namespace {

thread_local const Expired* active = nullptr;

}  // namespace

const Expired* current() { return active; }

void check() {
    if (active && (*active)()) throw Cancelled{};
}

Scope::Scope(const Expired* expired) : prev_(active) { active = expired; }

Scope::~Scope() { active = prev_; }

}  // namespace data2g::cancel
