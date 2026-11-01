/* Optional SCUMM resource overlays. GPL-3.0-or-later; see COPYING. */
#ifndef SCUMM_APPBASE_EXTENSIONS_H
#define SCUMM_APPBASE_EXTENSIONS_H
#include "common/array.h"
#include "common/hashmap.h"
#include "common/str.h"
#include "common/stream.h"
namespace Scumm {
class ScummEngine;
class AppbaseExtensions {
public:
	AppbaseExtensions() : _active(false), _originalActors(0), _capacity(0) {}
	void configure(ScummEngine &vm, const Common::String &path);
	bool applyScript(ScummEngine &vm, int id);
	bool acceptsSave(Common::SeekableReadStream &in) const;
	void saveIdentity(Common::WriteStream &out) const;
	bool active() const { return _active; }
	const Common::String &identity() const { return _identity; }
	int info(int kind, int id) const;
	bool isActor(unsigned id) const;
	// Only callable while configure() is building the frozen configuration.
	bool registerActor(unsigned id, unsigned capacityLimit, unsigned roomOwner);
private:
	struct Script { Common::Array<byte> bytes; Common::String originalMd5; unsigned loads = 0; };
	bool _active;
	unsigned _originalActors, _capacity;
	Common::String _identity;
	Common::Array<unsigned> _actors;
	Common::HashMap<int, Script> _scripts;
};
} // namespace Scumm
#endif
