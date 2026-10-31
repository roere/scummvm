/* AppBase V2 presentation overlay. GPL-3.0-or-later.
 * Original resource bytes, VM instruction pointers and save format stay intact.
 * Catalogs are owned local data supplied by the authenticated AppBase host. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "common/system.h"
#include "scumm/actor.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v2.h"
#include "scumm/verbs.h"
#include "gui/gui-manager.h"

namespace Scumm {
static byte buffers[8][512];
static uint nextBuffer = 0;
static bool originalExpansion = false, packedExpansion = false;
static bool countedSpeech = false;
static int originalCount = 0, translatedCount = 0, renderedCount = 0, mappedCount = 0;
static int initialTalkDelay = 0, messageTextSpeed = 0;
static int language = 0, requestedLanguage = 0;
static bool loadedFramePending = false, initialFramePresented = false;
bool appbaseInitialFramePresented() { return initialFramePresented; }

bool ScummEngine::appbaseGerman() const { return (_game.id == GID_MANIAC || _game.id == GID_ZAK) && _game.version == 2 && language == 1; }
Common::String ScummEngine::appbaseTextKey() const {
    if (_currentScript == 0xff || !_scriptPointer || !_scriptOrgPointer) return Common::String();
    const ScriptSlot &slot = vm.slot[_currentScript];
    const int offset = _scriptPointer - _scriptOrgPointer;
    if (slot.where == WIO_GLOBAL) return Common::String::format("s.%d.%d", slot.number, offset);
    if (slot.where == WIO_INVENTORY || slot.where == WIO_FLOBJECT) return Common::String::format("o.%d.%d", slot.number, offset);
    return Common::String::format("r.%d.%d", _roomResource, offset);
}
const byte *ScummEngine::appbaseText(const byte *source, const Common::String &key, int kind) {
    if (!source || !appbaseGerman() || originalExpansion || (kind == 1 && packedExpansion)) return source;
    const int length = resStrLen(source);
    if (length <= 0 || length > 500) return source;
    byte *dest = buffers[(nextBuffer++) % 8];
    const int count = EM_ASM_INT({
        return Module.mmTranslate ? Module.mmTranslate(HEAPU8, $0, UTF8ToString($1), $2, $3, $4, $5) : 0;
    }, kind, key.c_str(), source, length, dest, 511);
    if (count <= 0 || count > 500) return source;
    dest[count] = 0;return dest;
}
static int visibleCount(const byte *text, int length) {
    int total = 0;
    for (int i = 0; i < length; ++i) {
        if (text[i] == 255) { if (++i < length && text[i] > 3) i += 2; }
        else if (text[i] >= 32 && text[i] != '@') ++total;
    }
    return total;
}
void ScummEngine::appbaseTalk(const byte *source, const Common::String &key) {
    const byte *translated = appbaseText(source, key, 0);
    byte stableText[512];
    if (translated != source) {
        memcpy(stableText, translated, resStrLen(translated) + 1);
        translated = stableText;
    }
    countedSpeech = false;
    if (translated != source) {
        byte original[512],rendered[512];
        originalExpansion = true;convertMessageToString(source, original, sizeof(original));originalExpansion = false;
        packedExpansion = true;convertMessageToString(translated, rendered, sizeof(rendered));packedExpansion = false;
        originalCount = visibleCount(original, resStrLen(original));
        translatedCount = visibleCount(rendered, resStrLen(rendered));
        renderedCount = mappedCount = 0;countedSpeech = originalCount > 0 && translatedCount > 0;
    }
    packedExpansion = translated != source;actorTalk(translated);packedExpansion = false;
    initialTalkDelay = _talkDelay;messageTextSpeed = _defaultTextSpeed;
}
void ScummEngine::appbaseTextCount() {
    if (!countedSpeech) { VAR(VAR_CHARCOUNT)++;return; }
    ++renderedCount;
    const int count = MIN(originalCount, renderedCount * originalCount / translatedCount);
    const int increment = count - mappedCount;
    // The caller added one displayed character. Charge only its original-text
    // share to script timing as well; longer translations must not delay NPCs.
    _talkDelay += (increment - 1) * _defaultTextSpeed;
    _msgCount += increment - 1;
    VAR(VAR_CHARCOUNT) += increment;mappedCount = count;
}
bool ScummEngine::appbaseTextCanSave() const {
    // Presentation buffers and their counter mapping are deliberately transient.
    // A fully rendered final caption has already charged its original timing
    // into the serialized VM. No transient character mapping remains to resume.
    return !countedSpeech || !_haveMsg ||
        (_haveMsg == 1 && renderedCount >= translatedCount && mappedCount == originalCount);
}
void ScummEngine::appbaseTextReset() { if (!packedExpansion) countedSpeech = false; }
int ScummEngine::appbaseVerbGeometry(int verb, const byte *source, int field) {
    if (!source || (_game.id != GID_MANIAC && _game.id != GID_ZAK) || _game.version != 2) return -1;
    return EM_ASM_INT({
        return Module.mmVerbGeometry ? Module.mmVerbGeometry(HEAPU8, $0, $1, $2, $3, $4) : -1;
    }, verb, source, resStrLen(source), language, field);
}
void ScummEngine::appbaseTextLoaded() {
    appbaseManual(3);
    loadedFramePending = (_game.id == GID_MANIAC || _game.id == GID_ZAK) && _game.version == 2;
}
void ScummEngine::appbaseTextPresented() { loadedFramePending = false; initialFramePresented = true; }
void ScummEngine::appbaseTextTick() {
    if ((_game.id != GID_MANIAC && _game.id != GID_ZAK) || _game.version != 2 || g_gui.isActive()) return;
    // A loaded co-op guest can be paused before its first normal drawing pass.
    // Rebuild presentation only: no scripts, walking, costume animation, speech
    // timing, camera motion or palette cycling may advance while paused.
    // Loading clears all screen pixels, including any serialized caption's mask.
    // Repaint that blank frame even when a saved message is still pending.
    // A normal language change never gains this permission to erase a caption.
    if (isPaused() && _completeScreenRedraw && (!_haveMsg || loadedFramePending) && _currentRoom) {
        _completeScreenRedraw = false;
        _fullRedraw = true;
        scummLoop_handleDrawing();
        setActorRedrawFlags();
        resetActorBgs();
        Common::Array<Actor *> actors;
        for (int i = 1; i < _numActors; ++i)
            if (_actors[i]->isInCurrentRoom()) actors.push_back(_actors[i]);
        // Preserve the original v2 actor ordering, but do not animate costumes.
        for (uint j = 0; j < actors.size(); ++j)
            for (uint i = 0; i < actors.size(); ++i)
                if (actors[j]->getPos().y - actors[j]->_layer * 2000 <
                    actors[i]->getPos().y - actors[i]->_layer * 2000)
                    SWAP(actors[i], actors[j]);
        for (uint i = 0; i < actors.size(); ++i) actors[i]->drawActorCostume();
        redrawVerbs();
        drawSentence();
        static_cast<ScummEngine_v2 *>(this)->redrawV2Inventory();
        updatePalette();
        drawDirtyScreenParts();
        _system->updateScreen();
        appbaseTextPresented();
        _fullRedraw = false;
    }
    if (language == requestedLanguage) return;
    // Paused captions keep their existing bytes and timing; only the UI changes.
    // Continuing lines still finish before changing the presentation language.
    if (!isPaused() && _haveMsg && (countedSpeech || _talkDelay > 0)) return;
    language = requestedLanguage;
    EM_ASM({ Module.mmLanguage = $0 ? 'de' : 'en'; }, language);
    _completeScreenRedraw = true;
    // Clear all old hitbox backgrounds first: the two layouts overlap.
    // Otherwise a later verb can erase an already redrawn neighbour.
    for (int i = 1; i < _numVerbs; ++i) restoreVerbBG(i);
    for (int i = 1; i < _numVerbs; ++i) drawVerb(i, 0);
    drawSentence();
    static_cast<ScummEngine_v2 *>(this)->redrawV2Inventory();
    if (isPaused()) drawDirtyScreenParts();
}
extern "C" {
EMSCRIPTEN_KEEPALIVE int mm_text_version() { return 1; }
EMSCRIPTEN_KEEPALIVE int mm_text_language(int value) {
    if (value == 0 || value == 1) requestedLanguage = value;
    if (!g_scumm) {
        language = requestedLanguage;
        EM_ASM({ Module.mmLanguage = $0 ? 'de' : 'en'; }, language);
    }
    return language;
}
EMSCRIPTEN_KEEPALIVE int mm_text_stat(int field) {
    if (field == 0) return language;
    if (field == 1) return originalCount;
    if (field == 2) return translatedCount;
    if (field == 3) return mappedCount;
    if (field == 4) return renderedCount;
    if (field == 6) return initialTalkDelay;
    if (field == 7) return messageTextSpeed;
    return countedSpeech ? 1 : 0;
}
}
} // namespace Scumm
#endif
