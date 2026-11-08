/* Optional SCUMM resource overlays. GPL-3.0-or-later; see COPYING. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/formats/json.h"
#include "common/md5.h"
#include "common/memstream.h"
#include "common/textconsole.h"
#include "scumm/scumm.h"
#include "scumm/resource.h"
#include "scumm/actor.h"
#include "scumm/extension-costume-validation.h"
#include "scumm/usage_bits.h"
#include "scumm/appbase-extensions.h"
#include "scumm/extension-validation.h"
namespace Scumm {
namespace {
void validateJsonEnvelope(const Common::Array<byte> &raw) {
	struct Scope { bool object; Common::HashMap<Common::String, bool> keys; };
	Common::Array<Scope> scopes;
	for (uint i = 0; i < raw.size(); ++i) {
		if (!raw[i]) error("NUL byte in extension manifest");
		if (raw[i] == '{' || raw[i] == '[') {
			if (scopes.size() >= 32) error("Extension manifest nesting too deep");
			Scope scope; scope.object = raw[i] == '{'; scopes.push_back(scope);
		} else if (raw[i] == '}' || raw[i] == ']') {
			if (scopes.empty() || scopes.back().object != (raw[i] == '}')) error("Unbalanced extension manifest");
			scopes.pop_back();
		} else if (raw[i] == '"') {
			uint start = ++i;
			for (; i < raw.size() && raw[i] != '"'; ++i) { if (!raw[i]) error("NUL in manifest string"); if (raw[i] == '\\') ++i; }
			if (i >= raw.size()) error("Unterminated manifest string");
			uint next = i + 1;
			while (next < raw.size() && (raw[next] == ' ' || raw[next] == '\n' || raw[next] == '\r' || raw[next] == '\t')) ++next;
			if (next < raw.size() && raw[next] == ':') {
				Common::String key((const char *)raw.data() + start, i - start);
				if (scopes.empty() || !scopes.back().object || key.contains("\\") || scopes.back().keys.contains(key)) error("Duplicate or escaped extension manifest key");
				scopes.back().keys[key] = true;
			}
		}
	}
	if (!scopes.empty()) error("Unbalanced extension manifest");
}
void keys(Common::JSONValue *value, const char *allowed) {
	if (!value || !value->isObject()) error("Extension manifest object required");
	Common::String list = Common::String(",") + allowed + ",";
	Common::Array<Common::String> actual = value->objectKeys();
	for (uint i = 0; i < actual.size(); ++i) if (!list.contains(Common::String(",") + actual[i] + ",")) error("Unknown extension manifest key: %s", actual[i].c_str());
}
Common::String digest(const byte *data, uint32 size) {
	Common::MemoryReadStream stream(data, size);
	return Common::computeStreamMD5AsString(stream);
}
bool token(const Common::String &s, uint max = 80) {
	if (s.empty() || s.size() > max || s.contains("..")) return false;
	for (uint i = 0; i < s.size(); ++i)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.')) return false;
	return s[0] != '.';
}
Common::String text(Common::JSONValue *v, const char *key) {
	if (!v || !v->isObject() || !v->hasChild(key) || !v->child(key)->isString()) error("Extension manifest: missing string %s", key);
	return v->child(key)->asString();
}
unsigned number(Common::JSONValue *v, const char *key) {
	if (!v || !v->isObject() || !v->hasChild(key) || !v->child(key)->isIntegerNumber() || v->child(key)->asIntegerNumber() < 0 || v->child(key)->asIntegerNumber() > UINT32_MAX) error("Extension manifest: invalid number %s", key);
	return (unsigned)v->child(key)->asIntegerNumber();
}
const Common::JSONArray &array(Common::JSONValue *v, const char *key) {
	if (!v || !v->isObject() || !v->hasChild(key) || !v->child(key)->isArray()) error("Extension manifest: invalid array %s", key);
	return v->child(key)->asArray();
}
Common::Array<byte> read(const Common::Path &path, uint32 max) {
	Common::File f;
	// Extension files live outside the original game's SearchMan archive.
	if (!f.open(Common::FSNode(path)) || f.size() <= 0 || f.size() > max) error("Extension resource missing or too large: %s", path.toString().c_str());
	Common::Array<byte> data; data.resize((uint32)f.size());
	if (f.read(data.data(), data.size()) != data.size() || f.err()) error("Cannot read extension resource");
	return data;
}
}
bool AppbaseExtensions::registerActor(unsigned id, unsigned capacityLimit, unsigned roomOwner) {
	if (_active || !validExtensionActor(id, _originalActors, capacityLimit, roomOwner)) return false;
	for (uint i = 0; i < _actors.size(); ++i) if (_actors[i] == id) return false;
	_actors.push_back(id);
	if (id + 1 > _capacity) _capacity = id + 1;
	return true;
}
void AppbaseExtensions::configure(ScummEngine &vm, const Common::String &path) {
	if (_active) error("Extension configuration is immutable during a game");
	if (vm._game.version != 2 || !(vm._game.features & GF_OLD_BUNDLE) || vm._game.platform != Common::kPlatformAmiga)
		error("Extension manifests currently require classic SCUMM v2 Amiga resources");
	const Common::Array<byte> raw = read(Common::Path(path), 262144);
	validateJsonEnvelope(raw);
	_identity = digest(raw.data(), raw.size());
	_saveIdentity = _identity;
	if (ConfMan.hasKey("appbase_save_identity")) {
		_saveIdentity = ConfMan.get("appbase_save_identity");
		if (_saveIdentity.size() != 32) error("Invalid figure save identity");
		for (uint i = 0; i < _saveIdentity.size(); ++i)
			if (!strchr("0123456789abcdef", _saveIdentity[i])) error("Invalid figure save identity");
	}
	if (vm._targetName != Common::String(vm._game.gameid) + "-ext-" + _identity)
		error("Extensions require a separate target named <gameid>-ext-<manifest-md5>");
	Common::String json((const char *)raw.data(), raw.size());
	Common::JSONValue *manifest = Common::JSON::parse(json.c_str());
	if (!manifest || !manifest->isObject() || (number(manifest, "schemaVersion") != 1 && number(manifest, "schemaVersion") != 2 && number(manifest, "schemaVersion") != 3 && number(manifest, "schemaVersion") != 4 && number(manifest, "schemaVersion") != 5 && number(manifest, "schemaVersion") != 6) || !token(text(manifest, "variant"))) error("Invalid extension manifest/version");
	const unsigned schema = number(manifest, "schemaVersion");
	keys(manifest, schema >= 6 ? "schemaVersion,variant,game,extensions,party,wardrobeItems" : schema >= 3 ? "schemaVersion,variant,game,extensions,party" : "schemaVersion,variant,game,extensions");
	Common::JSONValue *game = manifest->child("game");
	keys(game, "id,version,platform,indexMd5,fingerprint");
	Common::String id = text(game, "id"), platform = text(game, "platform"), md5 = text(game, "indexMd5");
	Common::String detected;
	for (uint i = 0; i < sizeof(vm._gameMD5); ++i) detected += Common::String::format("%02x", vm._gameMD5[i]);
	if (id != vm._game.gameid || number(game, "version") != vm._game.version || platform != "amiga" || md5 != detected) error("Extension manifest does not match detected game data");
	_originalActors = _capacity = vm._numActors;
	const unsigned byteCapacity = (1U << (sizeof(vm._numActors) * 8)) - 1;
	const unsigned capacityLimit = MIN<unsigned>(byteCapacity, USAGE_BIT_RESTORED);
	const Common::JSONArray &extensions = array(manifest, "extensions");
	if (extensions.empty()) error("An extension variant must enable at least one extension");
	Common::HashMap<Common::String, bool> ids;
	for (uint i = 0; i < extensions.size(); ++i) {
		Common::JSONValue *ext = extensions[i];
		keys(ext, schema == 1 ? "id,version,actors,scripts" : (schema >= 4 ? "id,version,actors,scripts,costumes,actorInitializers,actorPolicies,encounters" : schema >= 3 ? "id,version,actors,scripts,costumes,actorInitializers,actorPolicies" : "id,version,actors,scripts,costumes,actorInitializers"));
		Common::String key = text(ext, "id"), version = text(ext, "version");
		if (!token(key) || !token(version) || ids.contains(key)) error("Conflicting extension id/version");
		ids[key] = true;
		const Common::JSONArray &actors = array(ext, "actors");
		for (uint j = 0; j < actors.size(); ++j) {
			if (!actors[j]->isIntegerNumber() || actors[j]->asIntegerNumber() < 0 || actors[j]->asIntegerNumber() >= capacityLimit || !registerActor((unsigned)actors[j]->asIntegerNumber(), capacityLimit, vm.OF_OWNER_ROOM)) error("Conflicting or out-of-format extension actor id");
		}
	}
	// Reject actual object/actor ID collisions. Holes in the expanded array
	// remain objects: objIsActor() consults the frozen actor registration set.
	auto present = [&vm](ResType type, int index) {
		const ResourceManager::Resource &resource = vm._res->_types[type][index];
		const int room = type == rtRoom ? index : resource._roomno;
		return resource._roomoffs != RES_INVALID_OFFSET && room < vm._numRooms && Common::File::exists(vm.generateFilename(room));
	};
	for (int roomId = 1; _capacity > _originalActors && roomId < vm._numRooms; ++roomId) {
		if (!present(rtRoom, roomId)) continue;
		const byte *room = vm.getResourceAddress(rtRoom, roomId);
		uint32 size = vm.getResourceSize(rtRoom, roomId);
		if (!room || size < 28) error("Invalid original v2 room resource");
		unsigned count = room[20];
		if (28 + count * 4 > size) error("Invalid original v2 object index");
		for (unsigned j = 0; j < count; ++j) {
			unsigned off = READ_LE_UINT16(room + 28 + 2 * count + 2 * j);
			if (off + 6 > size) error("Invalid original v2 object offset");
			unsigned object = READ_LE_UINT16(room + off + 4);
			if (object >= _originalActors && isActor(object)) error("Extension actor id conflicts with original object %u", object);
		}
	}
	Common::Path directory = Common::Path(path).getParent();
	bool actorIds[UINT8_MAX] = {};
	for (unsigned i = 0; i < _capacity; ++i) actorIds[i] = isActor(i);
	bool roomIds[UINT8_MAX + 1] = {}, scriptIds[UINT8_MAX + 1] = {}, costumeIds[UINT8_MAX + 1] = {}, soundIds[UINT8_MAX + 1] = {};
	for (int i = 0; i < vm._numRooms; ++i) roomIds[i] = i == 0 || present(rtRoom, i);
	for (int i = 0; i < vm._numScripts; ++i) scriptIds[i] = i != 0 && present(rtScript, i);
	for (int i = 0; i < vm._numCostumes; ++i) costumeIds[i] = i == 0 || present(rtCostume, i);
	for (int i = 0; i < vm._numSounds; ++i) soundIds[i] = i == 0 || present(rtSound, i);
	_originalCostumes = vm._numCostumes;
	if (schema >= 2) {
		for (Common::JSONValue *ext : extensions) {
			if (!ext->hasChild("costumes")) continue;
			for (Common::JSONValue *value : array(ext, "costumes")) {
				keys(value, "id,file,format,bytes,md5");
				const unsigned id = number(value, "id");
				const Common::String file = text(value, "file");
				if (id < _originalCostumes || id > UINT8_MAX || _costumes.contains(id) || !token(file) || text(value, "format") != "scumm-v2-costume")
					error("Conflicting or incompatible extension costume");
				Costume costume;
				costume.bytes = read(directory.join(file), UINT16_MAX);
				if (costume.bytes.size() != number(value, "bytes") || digest(costume.bytes.data(), costume.bytes.size()) != text(value, "md5"))
					error("Costume checksum/size mismatch");
				const char *why = nullptr;
				if (!validateExtensionCostumeV2(costume.bytes.data(), costume.bytes.size(), &why))
					error("Invalid extension costume: %s", why);
				_costumes[id] = costume;
				costumeIds[id] = true;
			}
		}
	}
	unsigned costumeCapacity = vm._numCostumes;
	for (auto it = _costumes.begin(); it != _costumes.end(); ++it)
		costumeCapacity = MAX<unsigned>(costumeCapacity, it->_key + 1);
	if (schema >= 2) {
		for (Common::JSONValue *ext : extensions) {
			if (!ext->hasChild("actorInitializers")) continue;
			for (Common::JSONValue *value : array(ext, "actorInitializers")) {
				keys(value, schema >= 4 ? "id,name,costume,room,x,y,direction,talkColor,palette,scale" : "id,name,costume,room,x,y,direction,talkColor,palette");
				InitialActor a;
				a.scale = schema >= 4 && value->hasChild("scale") ? number(value, "scale") : 100;
				if (a.scale < 1 || a.scale > 100) error("Actor scale must be 1..100 percent");
				a.id = number(value, "id");
				a.name = text(value, "name");
				a.costume = number(value, "costume");
				a.room = number(value, "room");
				a.x = number(value, "x");
				a.y = number(value, "y");
				a.direction = number(value, "direction");
				a.talkColor = number(value, "talkColor");
				if (a.id < _originalActors || !isActor(a.id) || !a.costume || a.costume >= costumeCapacity || !costumeIds[a.costume] ||
				    a.room >= vm._numRooms || !roomIds[a.room] || a.x > 32767 || a.y > 32767 ||
				    (a.direction != 0 && a.direction != 90 && a.direction != 180 && a.direction != 270) ||
				    a.talkColor > 15 || a.name.empty() || a.name.size() > 64)
					error("Invalid extension actor initializer");
				for (uint j = 0; j < a.name.size(); ++j)
					if ((unsigned char)a.name[j] < 32 || (unsigned char)a.name[j] > 126)
						error("Initializer actor names require printable ASCII");
				for (const InitialActor &old : _initialActors)
					if (old.id == a.id) error("Duplicate actor initializer");
				for (unsigned j = 0; j < 16; ++j) a.palette.push_back(j);
				if (value->hasChild("palette")) {
					bool used[16] = {};
					for (Common::JSONValue *color : array(value, "palette")) {
						keys(color, "index,color");
						const unsigned index = number(color, "index"), mapped = number(color, "color");
						if (index > 15 || mapped > 15 || used[index]) error("Invalid initializer palette");
						used[index] = true;
						a.palette[index] = mapped;
					}
				}
				_initialActors.push_back(a);
			}
		}
	}
	if (schema >= 3) {
		if (manifest->hasChild("party")) {
			Common::JSONValue *party = manifest->child("party"); keys(party, "actors,selectionVerbs");
			for (Common::JSONValue *actor : array(party, "actors")) {
				if (!actor->isIntegerNumber() || actor->asIntegerNumber() < _originalActors || actor->asIntegerNumber() >= vm.OF_OWNER_ROOM - 1) error("Playable extension actor must fit original V2 inventory ownership");
				unsigned id = actor->asIntegerNumber();
				if (!isActor(id) || partyActor(id)) error("Unregistered or duplicate party actor");
				bool initialized = false; for (const InitialActor &a : _initialActors) if (a.id == id) initialized = true;
				if (!initialized) error("Party actor requires initializer");
				_partyActors.push_back(id);
			}
			if (_partyActors.empty()) error("Empty extension party roster");
            if (party->hasChild("selectionVerbs")) {
                if (vm._game.id != GID_ZAK) error("Native character menu requires Zak V2");
                bool targets[256] = {};
                for (Common::JSONValue *binding : array(party, "selectionVerbs")) {
                    keys(binding, "verb,actor");
                    unsigned verb = number(binding, "verb"), actor = number(binding, "actor");
                    if (verb < 46 || verb > 59 || actor < 1 || actor > 13 ||
                        (actor > 4 && !partyActor(actor)) || _partyVerbs.contains(verb) || targets[actor])
                        error("Invalid or duplicate character menu binding");
                    _partyVerbs[verb] = actor; targets[actor] = true;
                }
                if (_partyVerbs.empty()) error("Empty character menu bindings");
            }
		}
		for (Common::JSONValue *ext : extensions) {
			if (!ext->hasChild("actorPolicies")) continue;
			for (Common::JSONValue *value : array(ext, "actorPolicies")) {
				keys(value, "id,allowedRooms,refusal"); ActorPolicy policy;
				policy.id = number(value, "id"); policy.refusal = text(value, "refusal");
				if (policy.id < _originalActors || !isActor(policy.id) || policy.refusal.empty() || policy.refusal.size() > 160) error("Invalid extension room policy");
				for (uint i = 0; i < policy.refusal.size(); ++i) if ((byte)policy.refusal[i] < 32 || (byte)policy.refusal[i] > 126) error("Policy refusal requires printable ASCII");
				for (const ActorPolicy &old : _policies) if (old.id == policy.id) error("Duplicate actor room policy");
				bool seen[256] = {};
				for (Common::JSONValue *room : array(value, "allowedRooms")) {
					if (!room->isIntegerNumber() || room->asIntegerNumber() <= 0 || room->asIntegerNumber() > 255 || !roomIds[room->asIntegerNumber()] || seen[room->asIntegerNumber()]) error("Invalid allowed room");
					unsigned id = room->asIntegerNumber(); seen[id] = true; policy.rooms.push_back(id);
				}
				if (policy.rooms.empty()) error("Empty actor room policy");
				bool initialized = false;
				for (const InitialActor &a : _initialActors) if (a.id == policy.id) { initialized = true; if (!seen[a.room]) error("Initializer outside allowed rooms"); }
				if (!initialized) error("Room policy requires initialized extension actor");
				_policies.push_back(policy);
			}
		}
	}
	if (schema >= 4) {
		Common::HashMap<Common::String, bool> encounterIds;
		for (Common::JSONValue *ext : extensions) {
			if (!ext->hasChild("encounters")) continue;
			for (Common::JSONValue *value : array(ext, "encounters")) {
				keys(value, schema >= 5 ? "id,actors,room,distance,once,lines,trigger" : "id,actors,room,distance,once,lines");
				Encounter e; e.id = text(value, "id");
				if (!token(e.id) || encounterIds.contains(e.id) || _encounters.size() >= 32 ||
				    !value->hasChild("once") || !value->child("once")->isBool() || !value->child("once")->asBool()) error("Invalid once-only encounter");
				encounterIds[e.id] = true;
				const Common::JSONArray &actors = array(value, "actors");
				if (actors.size() != 2) error("Encounter requires two actors");
				for (uint i = 0; i < 2; ++i) {
					if (!actors[i]->isIntegerNumber() || actors[i]->asIntegerNumber() <= 0 || actors[i]->asIntegerNumber() >= _capacity || !isActor(actors[i]->asIntegerNumber())) error("Invalid encounter actor");
					e.actors[i] = actors[i]->asIntegerNumber();
				}
				if (e.actors[0] == e.actors[1]) error("Encounter actors must differ");
				e.room = number(value, "room"); e.distance = number(value, "distance");
				if (!e.room || e.room > 255 || !roomIds[e.room] || !e.distance || e.distance > 1024) error("Invalid encounter room or pixel distance");
				if (value->hasChild("trigger")) {
					Common::JSONValue *t = value->child("trigger"); keys(t, "type,actor,verb,object,mask,from,to");
					e.actor = number(t, "actor"); e.verb = number(t, "verb"); e.object = number(t, "object");
					e.mask = number(t, "mask"); e.before = number(t, "from"); e.after = number(t, "to");
					if (text(t, "type") != "object-state-change" || (e.actor != e.actors[0] && e.actor != e.actors[1]) ||
					    !e.verb || e.verb >= 250 || !e.object || e.object >= (unsigned)vm._numGlobalObjects || isActor(e.object) ||
					    !e.mask || e.mask > 15 || (e.before & ~e.mask) || (e.after & ~e.mask) || e.before == e.after)
						error("Invalid object action trigger");
					_objectRules = true;
				}
				for (Common::JSONValue *line : array(value, "lines")) {
					keys(line, "actor,text"); EncounterLine l; l.actor = number(line, "actor"); l.text = text(line, "text");
					if ((l.actor != e.actors[0] && l.actor != e.actors[1]) || l.text.empty() || l.text.size() > 160 || e.lines.size() >= 16) error("Invalid encounter line");
					for (uint i = 0; i < l.text.size(); ++i) if ((byte)l.text[i] < 32 || (byte)l.text[i] > 126) error("Encounter lines require printable ASCII");
					e.lines.push_back(l);
				}
				if (e.lines.empty()) error("Encounter requires dialogue lines");
				_encounters.push_back(e);
			}
		}
	}
    if (schema >= 6) {
        const Common::JSONArray &items = array(manifest, "wardrobeItems");
        if (vm._game.id != GID_ZAK || items.empty() || items.size() > 16) error("Wardrobes require Zak V2 and a bounded item list");
        bool usedItems[33] = {}, usedCostumes[256] = {};
        for (Common::JSONValue *item : items) {
            keys(item, "id,key,kind,names,initialOwner,wearer,baseCostume,costume,image");
            const unsigned id = number(item, "id"), wearer = number(item, "wearer"), base = number(item, "baseCostume"), costume = number(item, "costume");
            if (id < 10003 || id > 10032 || usedItems[id - 10000] || wearer < _originalActors || !isActor(wearer) || number(item, "initialOwner") != wearer || base == costume || !_costumes.contains(base) || !_costumes.contains(costume) || usedCostumes[costume]) error("Invalid wardrobe actor/costume binding");
            usedItems[id - 10000] = true; usedCostumes[costume] = true;
            if (text(item, "kind") != "costume" || !token(text(item, "key"))) error("Invalid wardrobe item kind/key");
            Common::JSONValue *names = item->child("names"); keys(names, "de,en");
            if (text(names, "de").empty() || text(names, "de").size() > 64 || text(names, "en").empty() || text(names, "en").size() > 64) error("Invalid wardrobe item name");
            Common::JSONValue *image = item->child("image"); keys(image, "file,bytes,md5,sha256,width,height");
            const Common::String file = text(image, "file"); if (!token(file)) error("Invalid wardrobe image path");
            const Common::Array<byte> bytes = read(directory.join(file), 1048576);
            if (bytes.size() < 33 || bytes.size() != number(image, "bytes") || digest(bytes.data(), bytes.size()) != text(image, "md5") || READ_BE_UINT32(bytes.data()) != 0x89504e47 || READ_BE_UINT32(bytes.data() + 4) != 0x0d0a1a0a || READ_BE_UINT32(bytes.data() + 16) != number(image, "width") || READ_BE_UINT32(bytes.data() + 20) != number(image, "height") || number(image, "width") > 256 || number(image, "height") > 256) error("Invalid wardrobe item image");
#ifdef __EMSCRIPTEN__
            const int matches = EM_ASM_INT({const item=(Module.scummItems||[]).find(item=>item.id===$0);return item&&item.wearer===$1&&item.baseCostume===$2&&item.costume===$3?1:0;}, id, wearer, base, costume);
            if (!matches) error("Runtime wardrobe differs from pinned manifest");
#endif
        }
    }
	ExtensionV2Limits limits = {_capacity, (unsigned)vm._numRooms, (unsigned)vm._numScripts, (unsigned)vm._numGlobalObjects, costumeCapacity, (unsigned)vm._numVariables, vm.OF_OWNER_ROOM, actorIds, (unsigned)vm._numSounds, roomIds, scriptIds, costumeIds, soundIds};
	for (uint i = 0; i < extensions.size(); ++i) {
		const Common::JSONArray &scripts = array(extensions[i], "scripts");
		for (uint j = 0; j < scripts.size(); ++j) {
			Common::JSONValue *value = scripts[j];
			keys(value, "id,file,format,bytes,md5,originalMd5,originalBodyOffset");
			unsigned scriptId = number(value, "id");
			Common::String file = text(value, "file");
			const Common::String format = text(value, "format");
			if (!scriptId || scriptId >= (unsigned)vm._numScripts || _scripts.contains(scriptId) || !token(file, 80) || (format != "scumm-v2" && format != "scumm-v2-prefix")) error("Conflicting or incompatible script override");
			if (!scriptIds[scriptId]) error("Override refers to missing original script");
			const byte *original = vm.getResourceAddress(rtScript, scriptId);
			uint32 originalSize = vm.getResourceSize(rtScript, scriptId);
			if (!original || originalSize < 4 || digest(original, originalSize) != text(value, "originalMd5")) error("Original script checksum mismatch");
			byte header[2] = {original[2], original[3]};
			Script script; script.originalMd5 = text(value, "originalMd5");
			script.bytes = read(directory.join(file), UINT16_MAX);
			if (script.bytes.size() != number(value, "bytes") || digest(script.bytes.data(), script.bytes.size()) != text(value, "md5")) error("Override resource checksum/size mismatch");
			const char *why = nullptr;
			if (script.bytes.size() < 5 || script.bytes[2] != header[0] || script.bytes[3] != header[1] || READ_LE_UINT16(script.bytes.data()) != script.bytes.size()) error("Invalid v2 override header");
			Common::Array<byte> validation = script.bytes;
			if (format == "scumm-v2-prefix") {
				unsigned offset = number(value, "originalBodyOffset");
				if (offset < 4 || offset >= script.bytes.size() || script.bytes.size() - offset != originalSize - 4 || memcmp(script.bytes.data() + offset, original + 4, originalSize - 4)) error("Override original suffix is not byte-identical");
				// Validate only newly introduced code; the unchanged original
				// suffix retains the original VM's validated resource provenance.
				validation.resize(offset + 1); validation[offset] = 0;
				WRITE_LE_UINT16(validation.data(), validation.size());
			} else if (value->hasChild("originalBodyOffset")) error("Unexpected original suffix offset");
			if (!validateExtensionV2(validation.data(), validation.size(), limits, &why)) error("Invalid script override: %s", why);
			_scripts[scriptId] = script;
		}
	}
	delete manifest;
	if (ConfMan.hasKey("appbase_figure_binding")) {
		const Common::Array<byte> bytes = read(Common::Path(ConfMan.get("appbase_figure_binding")), 32768);
		validateJsonEnvelope(bytes);
		Common::String bindingJson((const char *)bytes.data(), bytes.size());
		Common::JSONValue *binding = Common::JSON::parse(bindingJson.c_str());
		if (!binding || !binding->isArray() || binding->asArray().size() > _initialActors.size()) error("Invalid figure bindings");
		for (Common::JSONValue *value : binding->asArray()) {
			keys(value, "actor,baseCostume,activeCostumes");
			FigureState state; state.actor = number(value, "actor"); state.base = number(value, "baseCostume");
			bool found = false; for (const InitialActor &a : _initialActors) if (a.id == state.actor && a.costume == state.base) found = true;
			Common::JSONValue *costumes = value->child("activeCostumes");
			if (!found || !costumes || !costumes->isArray() || costumes->asArray().empty() || costumes->asArray().size() > 8) error("Invalid figure costume bindings");
			for (const Common::JSONValue *costume : costumes->asArray()) {
				if (!costume->isIntegerNumber() || costume->asIntegerNumber() < 0 || costume->asIntegerNumber() > 255 || !_costumes.contains(costume->asIntegerNumber())) error("Invalid figure costume slot");
				state.costumes.push_back(costume->asIntegerNumber());
			}
			_figureStates.push_back(state);
		}
		delete binding;
	}
	_active = true;
	vm._numActors = _capacity;
	restoreResourceDirectory(vm);
	vm._res->allocResTypeData(rtActorName, 0, _capacity, kDynamicResTypeMode);
	for (Common::HashMap<int, Script>::const_iterator it = _scripts.begin(); it != _scripts.end(); ++it) vm._res->nukeResource(rtScript, it->_key);
}
bool AppbaseExtensions::applyScript(ScummEngine &vm, int id) {
	if (!_active || !_scripts.contains(id)) return false;
	Script &script = _scripts[id];
	const byte *original = vm.getResourceAddress(rtScript, id);
	if (digest(original, vm.getResourceSize(rtScript, id)) != script.originalMd5) error("Original script changed after extension configuration");
	// V2 createResource deliberately returns an already cached script without
	// resizing it. This original, newly loaded entry is not executing yet.
	vm._res->nukeResource(rtScript, id);
	byte *replacement = vm._res->createResource(rtScript, id, script.bytes.size());
	memcpy(replacement, script.bytes.data(), script.bytes.size());
	++script.loads;
	return true;
}
void AppbaseExtensions::saveIdentity(Common::WriteStream &out) const {
	if (!_active) return;
	if (!_encounters.empty()) {
		if (_objectRules) for (const ActionContext &c : _contexts) { out.writeUint32LE(c.action); out.writeUint32LE(c.object); }
		out.writeUint32BE(MKTAG('A','B','E','V')); out.writeUint32LE(_objectRules ? 2 : 1); out.writeUint32LE(_encounters.size());
		out.writeUint32LE(_fired); out.writeUint32LE(_pending); out.writeUint32LE(_activeEncounter); out.writeUint32LE(_nextLine); out.writeUint32LE(stateHash());
	}
	out.writeUint32BE(MKTAG('A','B','E','X')); out.writeUint32LE(1); out.writeUint32LE(_capacity); out.write(_saveIdentity.c_str(), 32);
}
bool AppbaseExtensions::acceptsSave(Common::SeekableReadStream &in) const {
	const int64 position = in.pos();
	bool marked = false;
	if (in.size() >= 44) { in.seek(in.size() - 44); marked = in.readUint32BE() == MKTAG('A','B','E','X'); }
	bool valid = !marked && !_active;
	if (marked && _active) {
		uint32 version = in.readUint32LE(), capacity = in.readUint32LE(); char identity[33] = {};
		valid = in.read(identity, 32) == 32 && version == 1 && capacity == _capacity && _saveIdentity == identity;
	}
	// A header marker makes a missing/truncated trailer rejectable as well.
	if (in.size() >= 8) { in.seek(4); if (in.readUint32LE() == MKTAG('A','B','E','X') && !marked) valid = false; }
	if (valid && _active) valid = const_cast<AppbaseExtensions *>(this)->readState(in, false);
	in.seek(position); return valid;
}
bool AppbaseExtensions::loadCostume(ScummEngine &vm, int id) {
	if (!_active || !_costumes.contains(id)) return false;
	Costume &costume = _costumes[id];
	byte *out = vm._res->createResource(rtCostume, id, costume.bytes.size());
	memcpy(out, costume.bytes.data(), costume.bytes.size());
	++costume.loads;
	return true;
}
void AppbaseExtensions::restoreResourceDirectory(ScummEngine &vm) {
	if (!_active || _costumes.empty()) return;
	if ((unsigned)vm._numCostumes != _originalCostumes) error("Original costume directory changed");
	unsigned capacity = _originalCostumes;
	for (auto it = _costumes.begin(); it != _costumes.end(); ++it)
		capacity = MAX<unsigned>(capacity, it->_key + 1);
	// Runs immediately after index load, before any costume is used. Resource
	// owns its cache pointer; only empty cache entries may move during resize.
	for (const auto &resource : vm._res->_types[rtCostume])
		if (resource._address) error("Costume directory must be extended before resource use");
	vm._res->_types[rtCostume].resize(capacity);
	for (unsigned i = _originalCostumes; i < capacity; ++i) {
		vm._res->_types[rtCostume][i]._roomno = 0;
		vm._res->_types[rtCostume][i]._roomoffs = RES_INVALID_OFFSET;
	}
	vm._numCostumes = capacity;
}
void AppbaseExtensions::initializeActors(ScummEngine &vm) {
	if (!_active) return;
	_fired = _pending = _activeEncounter = _nextLine = _requestClock = 0;
	for (const InitialActor &initial : _initialActors) {
		Actor *a = vm._actors[initial.id];
		vm.loadPtrToResource(rtActorName, initial.id, (const byte *)initial.name.c_str());
		a->setActorCostume(initial.costume);
		a->_talkColor = initial.talkColor;
		for (unsigned i = 0; i < 16; ++i) a->setPalette(i, initial.palette[i]);
		a->setDirection(initial.direction);
		a->putActor(initial.x, initial.y, initial.room);
		a->startAnimActor(a->_standFrame);
	}
}
void AppbaseExtensions::restoreFigureState(ScummEngine &vm) {
	if (!_active || _figureStates.empty()) return;
	for (const FigureState &state : _figureStates) {
		for (const InitialActor &initial : _initialActors) if (initial.id == state.actor) {
			vm._res->nukeResource(rtActorName, initial.id);
			vm.loadPtrToResource(rtActorName, initial.id, (const byte *)initial.name.c_str());
		}
		Actor *a = vm._actors[state.actor];
		bool active = false; for (unsigned costume : state.costumes) if (costume == a->_costume) active = true;
		if (!active) a->_costume = state.base;
		// Preserve position, walk state and palette; only rebuild animation channels
		// against the current artwork rather than serialized old frame indices.
		a->_cost.reset();
		a->startAnimActor(a->_moving ? a->_walkFrame : a->_standFrame);
	}
}
const char *AppbaseExtensions::figureVerbName(int verb) const {
	if (_figureStates.empty() || !_partyVerbs.contains(verb)) return nullptr;
	for (const InitialActor &initial : _initialActors) if (initial.id == (unsigned)_partyVerbs[verb]) return initial.name.c_str();
	return nullptr;
}
int AppbaseExtensions::partyVerbActor(int verb) const {
    if (_partyVerbs.empty()) return -1; // Older manifests retain their exact UI.
    return _partyVerbs.contains(verb) ? _partyVerbs[verb] : 0;
}
bool AppbaseExtensions::partyActor(unsigned id) const {
	for (unsigned actor : _partyActors) if (actor == id) return true;
	return false;
}
const char *AppbaseExtensions::roomRefusal(unsigned actor, unsigned room) const {
	for (const ActorPolicy &policy : _policies) if (policy.id == actor) {
		for (unsigned allowed : policy.rooms) if (allowed == room) return nullptr;
		return policy.refusal.c_str();
	}
	return nullptr;
}
unsigned AppbaseExtensions::scale(unsigned actor) const {
	if (!_active) return 100;
	for (const InitialActor &a : _initialActors) if (a.id == actor) return a.scale;
	return 100;
}
uint32 AppbaseExtensions::stateHash() const {
	uint32 hash = 2166136261u;
	for (uint32 value : {(uint32)_encounters.size(), _fired, _pending, _activeEncounter, _nextLine}) hash = (hash ^ value) * 16777619u;
	if (_objectRules) for (const ActionContext &c : _contexts) for (uint32 value : {c.action, c.object}) hash = (hash ^ value) * 16777619u;
	return hash;
}
bool AppbaseExtensions::readState(Common::SeekableReadStream &in, bool apply) {
	if (_encounters.empty()) return true;
	const int64 pos = in.pos();
	if (in.size() < 76) return false;
	in.seek(in.size() - 76);
	const uint32 marker = in.readUint32BE(), version = in.readUint32LE(), count = in.readUint32LE();
	const uint32 fired = in.readUint32LE(), pending = in.readUint32LE(), active = in.readUint32LE(), line = in.readUint32LE(), checksum = in.readUint32LE();
	ActionContext contexts[80];
	if (_objectRules) {
		if (version != 2 || in.size() < 716) { in.seek(pos); return false; }
		in.seek(in.size() - 716);
		for (ActionContext &c : contexts) { c.action = in.readUint32LE(); c.object = in.readUint32LE(); }
	}
	in.seek(pos);
	const uint32 mask = count == 32 ? 0xffffffffu : count < 32 ? (1u << count) - 1 : 0;
	uint32 hash = 2166136261u; for (uint32 value : {count, fired, pending, active, line}) hash = (hash ^ value) * 16777619u;
	if (_objectRules) for (const ActionContext &c : contexts) {
		const unsigned actor = c.action & 0xffff, verb = c.action >> 16;
		if ((c.action == 0) != (c.object == 0) || (c.action && (!actor || !isActor(actor) || !verb || verb >= 250 || c.object > 65535))) return false;
		for (uint32 value : {c.action, c.object}) hash = (hash ^ value) * 16777619u;
	}
	if (marker != MKTAG('A','B','E','V') || version != (_objectRules ? 2u : 1u) || count != _encounters.size() || (fired & ~mask) || (pending & ~fired) || active > count || checksum != hash ||
	    (!active && line) || (active && (!(fired & (1u << (active - 1))) || (pending & (1u << (active - 1))) || line > _encounters[active - 1].lines.size()))) return false;
	if (apply) { _fired = fired; _pending = pending; _activeEncounter = active; _nextLine = line; _requestClock = 0; if (_objectRules) for (unsigned i = 0; i < 80; ++i) _contexts[i] = contexts[i]; _chain = ActionContext(); }
	return true;
}
int AppbaseExtensions::commitEncounter(unsigned index) {
	if (!_active || index >= _encounters.size()) return -1;
	const uint32 bit = 1u << index;
	if (_fired & bit) return 0;
	_fired |= bit; _pending |= bit; return 1;
}
void AppbaseExtensions::scriptContext(ScummEngine &vm, unsigned slot, int object, int verb) {
	if (!_objectRules || slot >= 80) return;
	ActionContext c = vm._currentScript < 80 ? _contexts[vm._currentScript] : _chain;
	_chain = ActionContext();
	if (object) {
		c = ActionContext();
		const ActionContext parent = vm._currentScript < 80 ? _contexts[vm._currentScript] : ActionContext();
		// Preserve the actor that started the native sentence before walking,
		// rather than whichever EGO may be selected when the verb runs later.
		if (parent.object == unsigned(object) && (parent.action >> 16) == unsigned(verb) &&
		    !vm.vm.cutSceneStackPointer && vm._userPut > 0) c = parent;
	}
	_contexts[slot] = c;
}
void AppbaseExtensions::sentenceContext(ScummEngine &vm) {
	if (!_objectRules) return;
	_chain = ActionContext();
	const unsigned actor = vm._scummVars[vm.VAR_EGO], verb = vm._scummVars[vm.VAR_ACTIVE_VERB], object = vm._scummVars[vm.VAR_ACTIVE_OBJECT1];
	if (actor && isActor(actor) && verb && verb < 250 && object && object < unsigned(vm._numGlobalObjects) &&
	    !vm.vm.cutSceneStackPointer && vm._userPut > 0) {
		_chain.action = actor | (verb << 16); _chain.object = object;
	}
}
void AppbaseExtensions::chainContext(unsigned slot) { if (_objectRules && slot < 80) _chain = _contexts[slot]; }
void AppbaseExtensions::objectChanged(ScummEngine &vm, unsigned object, unsigned before, unsigned after) {
	if (!_objectRules || before == after || !vm._setupIsComplete || vm._currentScript >= 80 || vm._saveLoadFlag || vm.vm.cutSceneStackPointer) return;
	const ActionContext &c = _contexts[vm._currentScript];
	for (unsigned i = 0; i < _encounters.size(); ++i) {
		const Encounter &e = _encounters[i];
		if (!e.object || e.object != object || c.object != object || c.action != (e.actor | (uint32(e.verb) << 16)) ||
		    (_fired & (1u << i)) || (before & e.mask) != e.before || (after & e.mask) != e.after || vm._currentRoom != int(e.room)) continue;
		Actor *a = vm._actors[e.actors[0]], *b = vm._actors[e.actors[1]];
		const int dx = a->getPos().x - b->getPos().x, dy = a->getPos().y - b->getPos().y;
		if (a->_room != e.room || b->_room != e.room || !a->_costume || !b->_costume || dx * dx + dy * dy > int(e.distance * e.distance)) continue;
#ifdef __EMSCRIPTEN__
		if (vm.appbaseRequestObjectReaction(i, e.room, e.actor, object, e.verb, before, after)) continue;
#endif
		commitEncounter(i);
	}
}
void AppbaseExtensions::tick(ScummEngine &vm) {
	if (_encounters.empty() || !vm._setupIsComplete || !vm._currentRoom || vm._saveLoadFlag || vm._userPut <= 0 || vm.vm.cutSceneStackPointer || vm._haveMsg) return;
	++_requestClock;
	if (!_activeEncounter) {
		for (uint i = 0; i < _encounters.size(); ++i) {
			const Encounter &e = _encounters[i]; Actor *a = vm._actors[e.actors[0]], *b = vm._actors[e.actors[1]];
			if (a->_room != e.room || b->_room != e.room || vm._currentRoom != (int)e.room || !a->_costume || !b->_costume) continue;
			if (!e.object && !(_fired & (1u << i))) {
				const int dx = a->getPos().x - b->getPos().x, dy = a->getPos().y - b->getPos().y;
				if (a->_moving || b->_moving || dx * dx + dy * dy > int(e.distance * e.distance)) continue;
#ifdef __EMSCRIPTEN__
				// Independent contexts ask the canonical sequencer. Shared contexts
				// already replay identical native frames, including this rule.
				if (vm.appbaseRequestEncounter(i, e.room, e.actors[0], e.actors[1], _requestClock % 30 == 1)) continue;
#endif
				commitEncounter(i);
			}
			if (_pending & (1u << i)) { _pending &= ~(1u << i); _activeEncounter = i + 1; _nextLine = 0; break; }
		}
	}
	if (!_activeEncounter) return;
	const Encounter &e = _encounters[_activeEncounter - 1];
	if (vm._currentRoom != (int)e.room || vm._actors[e.actors[0]]->_room != e.room || vm._actors[e.actors[1]]->_room != e.room) return;
	if (_nextLine >= e.lines.size()) { _activeEncounter = _nextLine = 0; return; }
	const EncounterLine &line = e.lines[_nextLine++];
	vm._actorToPrintStrFor = line.actor; vm.actorTalk(reinterpret_cast<const byte *>(line.text.c_str()));
}
unsigned ScummEngine::appbaseExtensionScale(unsigned actor) const { return _appbaseExtensions.scale(actor); }
void ScummEngine::appbaseExtensionTick() { _appbaseExtensions.tick(*this); }
int ScummEngine::appbaseCommitEncounter(unsigned index) { return _appbaseExtensions.commitEncounter(index); }
int AppbaseExtensions::info(int kind, int id) const {
	if (kind == 12) return _encounters.size();
	if (kind == 13) return id >= 0 && (uint)id < _encounters.size() ? (!(_fired & (1u << id)) ? 0 : _pending & (1u << id) ? 1 : _activeEncounter == (uint)id + 1 ? 2 : 3) : -1;
	if (kind == 14) return _activeEncounter == (uint)id + 1 ? _nextLine : 0;
	if (kind == 15) return scale(id);

	if (kind == 10) return partyActor(id) ? 1 : 0;
	if (kind == 11) return _partyActors.size();
	if (kind == 5) return _costumes.contains(id) ? _costumes[id].loads : -1;
	if (kind == 6) return _costumes.contains(id) ? _costumes[id].bytes.size() : 0;
	if (kind == 7) return _originalCostumes;
	if (kind == 0) return _active ? 1 : 0;
	if (kind == 1) return _active ? _capacity : 0;
	if (kind == 2) { for (uint i = 0; i < _actors.size(); ++i) if (_actors[i] == (unsigned)id) return 1; return 0; }
	if (kind == 3) return _scripts.contains(id) ? _scripts[id].loads : -1;
	return -1;
}
bool AppbaseExtensions::isActor(unsigned id) const {
	if (id < _originalActors) return true;
	for (uint i = 0; i < _actors.size(); ++i) if (_actors[i] == id) return true;
	return false;
}
const byte *ScummEngine::appbaseCachedScript(int id) const {
	return id >= 0 && (uint)id < _res->_types[rtScript].size() ? _res->_types[rtScript][id]._address : nullptr;
}
bool ScummEngine::appbaseExtensionRoomAllowed(int actor, int room) {
	const char *refusal = _appbaseExtensions.roomRefusal(actor, room);
	if (!refusal) return true;
	if (actor > 0 && actor < _numActors) _actors[actor]->stopActorMoving();
	resetSentence();
	if (_currentScript != 0xff) stopObjectCode();
	_actorToPrintStrFor = actor;
	actorTalk(reinterpret_cast<const byte *>(refusal));
	return false;
}
int ScummEngine::appbaseExtensionsInfo(int kind, int id) const {
	if (kind == 1) return _numActors;
	if (kind == 8) return _numCostumes;
	if (kind == 9) return id >= 0 && id < _numCostumes && _res->_types[rtCostume][id]._address ? _res->_types[rtCostume][id]._size : 0;
	if (kind == 4) return appbaseCachedScript(id) ? _res->_types[rtScript][id]._size : 0;
	return _appbaseExtensions.info(kind, id);
}
#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE int scumm_extensions_version() { return 6; }
extern "C" EMSCRIPTEN_KEEPALIVE int scumm_extensions_info(int kind, int id) { return g_scumm ? g_scumm->appbaseExtensionsInfo(kind, id) : -1; }
extern "C" EMSCRIPTEN_KEEPALIVE int scumm_extensions_commit_encounter(int id) { return g_scumm && id >= 0 ? g_scumm->appbaseCommitEncounter(id) : -1; }
extern "C" EMSCRIPTEN_KEEPALIVE const byte *scumm_extensions_cached_script(int id) { return g_scumm ? g_scumm->appbaseCachedScript(id) : nullptr; }
#endif
} // namespace Scumm

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE int scumm_figure_links_version() { return 1; }
#endif
