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
	bool loadCostume(ScummEngine &vm, int id);
	void restoreResourceDirectory(ScummEngine &vm);
	void initializeActors(ScummEngine &vm);
	void restoreFigureState(ScummEngine &vm);
	const char *figureVerbName(int verb) const;
	bool acceptsSave(Common::SeekableReadStream &in) const;
	void saveIdentity(Common::WriteStream &out) const;
	bool partyActor(unsigned id) const;
	int partyVerbActor(int verb) const;
	const char *roomRefusal(unsigned actor, unsigned room) const;
	unsigned scale(unsigned actor) const;
	void tick(ScummEngine &vm);
	int commitEncounter(unsigned index);
	unsigned stateTrailerSize() const { return _encounters.empty() ? 0 : 32 + (_objectRules ? 640 : 0); }
	void scriptContext(ScummEngine &vm, unsigned slot, int object = 0, int verb = 0);
	void chainContext(unsigned slot);
	void sentenceContext(ScummEngine &vm);
	void objectChanged(ScummEngine &vm, unsigned object, unsigned before, unsigned after);
	bool readState(Common::SeekableReadStream &in, bool apply);
	uint32 stateHash() const;
	bool active() const { return _active; }
	const Common::String &identity() const { return _identity; }
	int info(int kind, int id) const;
	bool isActor(unsigned id) const;
	// Only callable while configure() is building the frozen configuration.
	bool registerActor(unsigned id, unsigned capacityLimit, unsigned roomOwner);
private:
	struct Script { Common::Array<byte> bytes; Common::String originalMd5; unsigned loads = 0; };
	struct Costume { Common::Array<byte> bytes; unsigned loads = 0; };
	struct InitialActor {
		unsigned id, costume, room, x, y, direction, talkColor, scale;
		Common::String name;
		Common::Array<unsigned> palette;
	};
	struct ActorPolicy { unsigned id; Common::Array<unsigned> rooms; Common::String refusal; };
	struct EncounterLine { unsigned actor; Common::String text; };
	struct Encounter { Common::String id; unsigned actors[2], room, distance; unsigned actor = 0, verb = 0, object = 0, mask = 0, before = 0, after = 0; Common::Array<EncounterLine> lines; };
	struct ActionContext { uint32 action = 0, object = 0; };
	ActionContext _contexts[80], _chain;
	bool _objectRules = false;
	Common::Array<Encounter> _encounters;
	uint32 _fired = 0, _pending = 0, _activeEncounter = 0, _nextLine = 0;
	unsigned _requestClock = 0;
	Common::Array<ActorPolicy> _policies;
	Common::Array<unsigned> _partyActors;
	Common::HashMap<int, int> _partyVerbs;
	Common::HashMap<int, Costume> _costumes;
	Common::Array<InitialActor> _initialActors;
	unsigned _originalCostumes = 0;
	bool _active;
	unsigned _originalActors, _capacity;
	Common::String _identity, _saveIdentity;
	struct FigureState { unsigned actor, base; Common::Array<unsigned> costumes; };
	Common::Array<FigureState> _figureStates;
	Common::Array<unsigned> _actors;
	Common::HashMap<int, Script> _scripts;
};
} // namespace Scumm
#endif
