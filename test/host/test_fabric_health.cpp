// fabric_health's bookkeeping (components/mb_core/include/fabric_health_logic.h)

#include <cstdio>
#include <cstdlib>

#include "fabric_health_logic.h"

using namespace fabric_health;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// `seconds` without contact in steps of a minute
int run_offline(Record& r, uint64_t id, uint32_t seconds, bool counting, int* stale_notes = nullptr) {
    int saves = 0;
    for (uint32_t t = 0; t < seconds; t += 60) {
        const Step s = advance(r, id, false, counting, 60);
        saves += s.save ? 1 : 0;
        if (stale_notes != nullptr && s.note_stale) ++*stale_notes;
    }
    return saves;
}

}  // namespace

int main() {
    const uint64_t ID = 0xB648F47697119871ULL;

    {  // A new controller: stored once, nothing dropped while connected
        Record r;
        const Step first = advance(r, ID, true, true, 0);
        check(first.save, "new record is saved");
        const Step again = advance(r, ID, true, true, 60);
        check(!again.save, "connected: no writes");
    }
    {  // Off the network itself: that time does not count
        Record r;
        advance(r, ID, true, true, 0);
        run_offline(r, ID, 3 * STALE_S, false);
        check(r.offline_s == 0, "not counted while off the network");
    }
    {  // Coming back resets the count, and is stored
        Record r;
        advance(r, ID, true, true, 0);
        run_offline(r, ID, 2 * 3600, true);
        const Step back = advance(r, ID, true, true, 60);
        check(back.save && r.offline_s == 0, "contact again: reset and saved");
    }
    {  // Saved about once an hour while away
        Record r;
        advance(r, ID, true, true, 0);
        int saves = 0;
        for (int i = 0; i < 180; ++i) saves += advance(r, ID, false, true, 60).save ? 1 : 0;
        check(saves == 3, "three saves in three hours");
    }
    {  // Stale after 30 days, noted once
        Record r;
        advance(r, ID, true, true, 0);
        int notes = 0;
        run_offline(r, ID, STALE_S - 60, true, &notes);
        check(notes == 0, "not stale before 30 days");
        run_offline(r, ID, 3600, true, &notes);
        check(notes == 1 && r.offline_s >= STALE_S, "stale noted once");
    }
    {  // Stored subscriptions of nodes that are gone
        check(!drop_orphan(true, ORPHAN_S * 10, 100), "reachable node: never dropped");
        check(!drop_orphan(false, ORPHAN_S - 1, ORPHAN_RETRIES - 1), "unreachable briefly: kept");
        check(drop_orphan(false, ORPHAN_S, 0), "unreachable for ORPHAN_S: dropped");
        check(drop_orphan(false, 0, ORPHAN_RETRIES), "failed attempts (kept across restarts): dropped at once");
    }
    {  // A reused fabric index is another controller: count starts over
        Record r;
        r.id = ID;
        r.offline_s = STALE_S;
        const Step s = advance(r, ID + 1, false, true, 0);
        check(s.save && r.offline_s == 0, "other fabric id: fresh record");
    }
    {  // No overflow
        Record r;
        r.id = ID;
        r.offline_s = UINT32_MAX - 10;
        advance(r, ID, false, true, 60);
        check(r.offline_s == UINT32_MAX, "count saturates");
    }

    if (failures == 0) std::printf("fabric_health: all tests passed\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
