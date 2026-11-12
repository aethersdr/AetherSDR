// Cooperative cancellation for work that is allowed a time budget (decision-directed decoding): the
// caller opens a Scope with a predicate, the long loops underneath call check(), and the first one to
// see the predicate true throws Cancelled, which unwinds to the Scope's owner.
//
// The active predicate is per thread, and pool::parallel_for hands the caller's to the tasks it runs on
// other threads (and rethrows the first exception), so nothing between the Scope and the loops needs to
// know about it, and two threads with their own Scopes never see each other's.
#pragma once

#include <functional>

namespace data2g::cancel {

struct Cancelled {};

using Expired = std::function<bool()>;

// The calling thread's predicate (null: none); what parallel_for copies into its tasks.
const Expired* current();

// Throws Cancelled if there is a predicate and it says so. Cheap when there is none.
void check();

class Scope {
public:
    explicit Scope(const Expired* expired);  // null: none (also hides an outer one)
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const Expired* prev_;
};

}  // namespace data2g::cancel
