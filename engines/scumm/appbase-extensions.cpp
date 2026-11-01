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
		error("Extension schema 1 currently requires classic SCUMM v2 Amiga resources");
	const Common::Array<byte> raw = read(Common::Path(path), 262144);
	validateJsonEnvelope(raw);
	_identity = digest(raw.data(), raw.size());
	if (vm._targetName != Common::String(vm._game.gameid) + "-ext-" + _identity)
		error("Extensions require a separate target named <gameid>-ext-<manifest-md5>");
	Common::String json((const char *)raw.data(), raw.size());
	Common::JSONValue *manifest = Common::JSON::parse(json.c_str());
	if (!manifest || !manifest->isObject() || number(manifest, "schemaVersion") != 1 || !token(text(manifest, "variant"))) error("Invalid extension manifest/version");
	keys(manifest, "schemaVersion,variant,game,extensions");
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
		keys(ext, "id,version,actors,scripts");
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
	ExtensionV2Limits limits = {_capacity, (unsigned)vm._numRooms, (unsigned)vm._numScripts, (unsigned)vm._numGlobalObjects, (unsigned)vm._numCostumes, (unsigned)vm._numVariables, vm.OF_OWNER_ROOM, actorIds, (unsigned)vm._numSounds, roomIds, scriptIds, costumeIds, soundIds};
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
	_active = true;
	vm._numActors = _capacity;
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
	out.writeUint32BE(MKTAG('A','B','E','X')); out.writeUint32LE(1); out.writeUint32LE(_capacity); out.write(_identity.c_str(), 32);
}
bool AppbaseExtensions::acceptsSave(Common::SeekableReadStream &in) const {
	const int64 position = in.pos();
	bool marked = false;
	if (in.size() >= 44) { in.seek(in.size() - 44); marked = in.readUint32BE() == MKTAG('A','B','E','X'); }
	bool valid = !marked && !_active;
	if (marked && _active) {
		uint32 version = in.readUint32LE(), capacity = in.readUint32LE(); char identity[33] = {};
		valid = in.read(identity, 32) == 32 && version == 1 && capacity == _capacity && _identity == identity;
	}
	// A header marker makes a missing/truncated trailer rejectable as well.
	if (in.size() >= 8) { in.seek(4); if (in.readUint32LE() == MKTAG('A','B','E','X') && !marked) valid = false; }
	in.seek(position); return valid;
}
int AppbaseExtensions::info(int kind, int id) const {
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
int ScummEngine::appbaseExtensionsInfo(int kind, int id) const {
	if (kind == 1) return _numActors;
	if (kind == 4) return appbaseCachedScript(id) ? _res->_types[rtScript][id]._size : 0;
	return _appbaseExtensions.info(kind, id);
}
#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE int scumm_extensions_version() { return 1; }
extern "C" EMSCRIPTEN_KEEPALIVE int scumm_extensions_info(int kind, int id) { return g_scumm ? g_scumm->appbaseExtensionsInfo(kind, id) : -1; }
extern "C" EMSCRIPTEN_KEEPALIVE const byte *scumm_extensions_cached_script(int id) { return g_scumm ? g_scumm->appbaseCachedScript(id) : nullptr; }
#endif
} // namespace Scumm
