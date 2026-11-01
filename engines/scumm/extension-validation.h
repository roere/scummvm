/* Optional SCUMM extension validation. GPL-3.0-or-later; see COPYING. */
#ifndef SCUMM_EXTENSION_VALIDATION_H
#define SCUMM_EXTENSION_VALIDATION_H
#include <stddef.h>
#include <stdint.h>

namespace Scumm {
struct ExtensionV2Limits {
	unsigned actors, rooms, scripts, objects, costumes, variables, ownerRoom;
	const bool *actorIds;
	unsigned sounds;
	const bool *roomIds, *scriptIds, *costumeIds, *soundIds;
};

// Validation only: execution continues to use the original opcode handlers.
// Phase 1 deliberately accepts a documented direct-operand subset. Unknown
// instructions and indirect references fail closed, rather than guessing widths.
inline bool validateExtensionV2(const unsigned char *p, size_t size, const ExtensionV2Limits &l, const char **why) {
	#define EXT_BAD(message) do { *why = message; return false; } while (0)
	if (size < 5 || size > UINT16_MAX || (size_t)(p[0] | (p[1] << 8)) != size) EXT_BAD("invalid v2 resource size");
	unsigned char *boundary = new unsigned char[size]();
	bool valid = true, stopped = false;
	const char *reason = "invalid v2 bytecode";
	for (size_t pos = 4; valid && pos < size;) {
		boundary[pos] = 1;
		unsigned op = p[pos++], n = 0;
		switch (op) {
		case 0x00: case 0xa0: stopped = true; break;
		case 0x80: break;
		case 0x01: case 0x1e: n = 3; break; // putActor / walkActorTo
		case 0x11: case 0x2d: case 0x2c: n = 2; break;
		case 0x13: n = 3; break; // actorOps; direct, fixed-width operations below
		case 0x09: case 0x1a: case 0x29: n = 3; break;
		case 0x18: n = 2; break;
		case 0x42: case 0x52: case 0x62: case 0x72: n = 1; break;
		default: valid = false; reason = "unsupported v2 opcode or indirect reference"; break;
		}
		if (!valid) break;
		if (n > size - pos) { valid = false; reason = "truncated v2 instruction"; break; }
		const unsigned char *a = p + pos;
		if (op == 0x01 || op == 0x1e || op == 0x09 || op == 0x11 || op == 0x13 || op == 0x2d || op == 0x52)
			if (!a[0] || a[0] >= l.actors || (l.actorIds && !l.actorIds[a[0]])) { valid = false; reason = "invalid actor reference"; }
		if (op == 0x2d && a[1] >= l.rooms) { valid = false; reason = "invalid room reference"; }
		if (op == 0x72 && a[0] >= l.rooms) { valid = false; reason = "invalid room reference"; }
		if ((op == 0x2d || op == 0x72) && l.roomIds) { unsigned room = op == 0x2d ? a[1] : a[0]; if (room >= l.rooms || !l.roomIds[room]) { valid = false; reason = "missing room resource"; } }
		if ((op == 0x42 || op == 0x62) && (!a[0] || a[0] >= l.scripts)) { valid = false; reason = "invalid script reference"; }
		if ((op == 0x42 || op == 0x62) && l.scriptIds && (a[0] >= l.scripts || !l.scriptIds[a[0]])) { valid = false; reason = "missing script resource"; }
		if ((op == 0x1a || op == 0x2c) && a[0] >= l.variables) { valid = false; reason = "invalid variable reference"; }
		if (op == 0x13 && a[2] != 1 && a[2] != 4 && a[2] != 5) { valid = false; reason = "unsupported variable-width actor operation"; }
		if (op == 0x13 && a[2] == 4 && a[1] >= l.costumes) { valid = false; reason = "invalid costume reference"; }
		if (op == 0x13 && a[2] == 1 && a[1] >= l.sounds) { valid = false; reason = "invalid sound reference"; }
		if (op == 0x13 && a[2] == 4 && l.costumeIds && (a[1] >= l.costumes || !l.costumeIds[a[1]])) { valid = false; reason = "missing costume resource"; }
		if (op == 0x13 && a[2] == 1 && l.soundIds && (a[1] >= l.sounds || !l.soundIds[a[1]])) { valid = false; reason = "missing sound resource"; }
		if (op == 0x09 && (!(a[1] | (a[2] << 8)) || (unsigned)(a[1] | (a[2] << 8)) >= l.objects)) { valid = false; reason = "invalid facing reference"; }
		if (op == 0x29 && (!(a[0] | (a[1] << 8)) || (unsigned)(a[0] | (a[1] << 8)) >= l.objects || (a[2] && a[2] != l.ownerRoom && (a[2] >= l.actors || (l.actorIds && !l.actorIds[a[2]]))))) { valid = false; reason = "invalid object/owner reference"; }
		pos += n;
		if (stopped && pos != size) { valid = false; reason = "bytes after script terminator"; }
	}
	if (valid && !stopped) { valid = false; reason = "script has no final terminator"; }
	// All branch destinations must be opcode boundaries. Backward branches must
	// yield, so a malformed technical override cannot spin forever in one frame.
	for (size_t pos = 4; valid && pos < size; ++pos) {
		if (!boundary[pos] || p[pos] != 0x18) continue;
		int target = (int)pos + 3 + (int16_t)(p[pos + 1] | (p[pos + 2] << 8));
		if (target < 4 || target >= (int)size || !boundary[target]) { valid = false; reason = "jump outside instruction boundaries"; break; }
		if (target <= (int)pos) {
			bool yields = false;
			for (int i = target; i < (int)pos; ++i) if (boundary[i] && p[i] == 0x80) yields = true;
			if (!yields) { valid = false; reason = "backward jump without yield"; }
		}
	}
	delete[] boundary;
	if (!valid) { *why = reason; return false; }
	return true;
	#undef EXT_BAD
}
inline bool validExtensionActor(unsigned id, unsigned originalCount, unsigned capacityLimit, unsigned roomOwner) {
	return id >= originalCount && id < capacityLimit && id != roomOwner;
}
} // namespace Scumm
#endif
