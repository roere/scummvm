/* ScummVM - shared AppBase party roster predicates.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SCUMM_APPBASE_PARTY_PROFILE_H
#define SCUMM_APPBASE_PARTY_PROFILE_H

namespace Scumm {
// Exact original playable sets. Actor capacity includes NPCs and must never
// grant control rights. Future manifest additions extend a roster, not a range.
inline bool appbasePartyOriginalActor(bool zak, int actor) {
    static const int maniacActors[] = {1, 2, 3, 4, 5, 6, 7};
    static const int zakActors[] = {1, 2, 3, 4};
    const int *ids = zak ? zakActors : maniacActors;
    const unsigned count = zak ? sizeof(zakActors)/sizeof(int) : sizeof(maniacActors)/sizeof(int);
    for (unsigned i = 0; i < count; ++i) if (ids[i] == actor) return true;
    return false;
}
}
#endif
