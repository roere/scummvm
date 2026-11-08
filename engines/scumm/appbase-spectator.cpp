/* AppBase receive-only observer tape. GPL-3.0-or-later.
 * Independent of player ACKs, peer queues and network readiness. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "common/file.h"
#include "common/fs.h"
#include "common/memstream.h"
#include "common/serializer.h"
#include "gui/gui-manager.h"
#include "scumm/actor.h"
#include "scumm/resource.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v2.h"

namespace Scumm {
void appbaseCoopObserverSync(Common::Serializer &s);
void appbaseTextObserverSync(Common::Serializer &s);
static int observerRole = 0, observerFrame = 0, observerFailure = 0;
static bool observerInside = false, observerCheckpoint = false, observerReset = false;
static int observerInput[10] = {};

bool ScummEngine::appbaseSpectatorReplay() const { return observerRole == 2; }
int ScummEngine::appbaseSpectatorWait(int original) const {
    if (observerRole != 2) return original;
    return EM_ASM_INT({ return Module.scummSpectatorHasFrame?.() ? 0 : 4; });
}
void ScummEngine::appbaseSpectatorContext(Common::Serializer &s) {
    appbaseCoopObserverSync(s);
    appbaseTextObserverSync(s);
    ScummEngine_v2 *v2 = static_cast<ScummEngine_v2 *>(this);
    s.syncAsByte(v2->_appbaseManualRetry);s.syncAsByte(v2->_appbaseManualCancelRequested);s.syncAsSint32LE(v2->_appbaseManualOwner);
    s.syncAsSint32LE(v2->_appbaseGivingItem);
    int offset = v2->appbasePeerInventory(-1);s.syncAsSint32LE(offset);
    if (s.isLoading()) v2->appbasePeerInventory(offset);
    // Loading an ordinary save may normalize the pending sentence. Observers
    // resume the exact frame boundary, including an unconfirmed garment.
    for (int i = 0; i < _numVariables; ++i) s.syncAsSint32LE(_scummVars[i]);
    uint32 seed = _rnd.getSeed();s.syncAsUint32LE(seed);
    if (s.isLoading()) _rnd.setSeed(seed);
}
void ScummEngine::appbaseSpectatorIdle() {
    if (observerRole != 1 || observerInside || _game.version != 2 || (_game.id != GID_MANIAC && _game.id != GID_ZAK)) return;
        // A dedicated network snapshot is taken here, at the outer VM boundary, including
        // when paused. No serialization runs from a JavaScript callback. It does not enter any player save
        // barrier and does not change ordinary save-menu eligibility.
        if (observerCheckpoint && _setupIsComplete && !_saveLoadFlag && !g_gui.isActive()) {
            Common::MemoryWriteStreamDynamic save(DisposeAfterUse::YES), context(DisposeAfterUse::YES);
            if (saveState(&save)) {
                context.writeUint32BE(MKTAG('A','B','S','P'));context.writeUint32LE(1);
                Common::Serializer s(nullptr, &context);appbaseSpectatorContext(s);
                EM_ASM({ Module.scummSpectatorCheckpoint?.($0, HEAPU8.slice($1, $1+$2), HEAPU8.slice($3, $3+$4)); }, observerFrame, save.getData(), (int)save.size(), context.getData(), (int)context.size());
                observerCheckpoint = false;observerReset = false;
            }
        }
}
bool ScummEngine::appbaseSpectatorBegin(int &delta) {
    if (!observerRole || _game.version != 2 || (_game.id != GID_MANIAC && _game.id != GID_ZAK)) return false;
    if (observerFailure) return true;
    if (observerRole == 1) {
        appbaseSpectatorIdle();
        if (observerReset) return false;
        ++observerFrame;observerInside = true;
        EM_ASM({ Module.scummSpectatorBegin?.($0, [$1,$2>>>0,$3,$4,$5,$6,$7,$8,$9,$10]); }, observerFrame, delta, _rnd.getSeed(), _mouse.x, _mouse.y, _leftBtnPressed, _rightBtnPressed, _keyPressed.keycode, _keyPressed.ascii, _keyPressed.flags, _defaultTextSpeed);
        return false;
    }
    if(_saveLoadFlag==2&&_saveLoadSlot==254){scummLoop_handleSaveLoad();return true;}
    const int accepted = EM_ASM_INT({const row=Module.scummSpectatorTakeFrame?.($0);if(!row)return 0;if(!Array.isArray(row)||row.length!==10)return -1;for(let i=0;i<10;++i)HEAP32[($1>>2)+i]=row[i]|0;return 1;}, observerFrame+1, observerInput);
    if (accepted <= 0) { if (accepted < 0) observerFailure = 1;return true; }
    ++observerFrame;observerInside = true;delta = observerInput[0];_rnd.setSeed((uint32)observerInput[1]);
    _mouse.x=observerInput[2];_mouse.y=observerInput[3];_leftBtnPressed=observerInput[4];_rightBtnPressed=observerInput[5];
    _keyPressed=Common::KeyState(static_cast<Common::KeyCode>(observerInput[6]),observerInput[7],observerInput[8]);
    _defaultTextSpeed=observerInput[9];setTalkSpeed(_defaultTextSpeed);
    return false;
}
void ScummEngine::appbaseSpectatorInput() {
    if (!observerInside) return;
    if (observerRole == 1) {
        EM_ASM({ Module.scummSpectatorInput?.([$0,$1,$2,$3,$4,$5,$6]); }, _mouse.x,_mouse.y,_leftBtnPressed,_rightBtnPressed,_keyPressed.keycode,_keyPressed.ascii,_keyPressed.flags);
    } else {
        int input[7];const int good=EM_ASM_INT({const row=Module.scummSpectatorCurrent?.input;if(!Array.isArray(row)||row.length!==7)return 0;for(let i=0;i<7;++i)HEAP32[($0>>2)+i]=row[i]|0;return 1;},input);
        if (!good) { observerFailure=2;return; }
        _mouse.x=input[0];_mouse.y=input[1];_leftBtnPressed=input[2];_rightBtnPressed=input[3];
        _keyPressed=Common::KeyState(static_cast<Common::KeyCode>(input[4]),input[5],input[6]);
    }
}
int ScummEngine::appbaseSpectatorValue(int kind,int id,int value) const {
    if (!observerInside) return value;
    if (observerRole == 1) { EM_ASM({Module.scummSpectatorValue?.([$0,$1,$2]);},kind,id,value);return value; }
    const int result=EM_ASM_INT({const row=Module.scummSpectatorCurrent?.values?.shift();if(!row||row[0]!==$0||row[1]!==$1){Module.scummSpectatorMismatch=true;return $2;}return row[2]|0;},kind,id,value);
    return result;
}
void ScummEngine::appbaseSpectatorEnd() {
    if (!observerInside) return;
    observerInside=false;
    if (observerReset) { EM_ASM({Module.scummSpectatorDiscard?.();});return; }
    uint32 hash=2166136261u;const auto add=[&](int v){hash=(hash^uint32(v))*16777619u;};
    add(_currentRoom);add(VAR(VAR_EGO));add(_userPut);add(camera._cur.x);
    for(int i=0;i<_numVariables;++i)if(i!=VAR_HAVE_MSG&&i!=VAR_CHARCOUNT)add(_scummVars[i]);
    for(int i=0;i<NUM_SCRIPT_SLOT;++i){add(vm.slot[i].number);add(vm.slot[i].status);add(vm.slot[i].delay);add(vm.slot[i].offs);}
    for(int i=1;i<_numGlobalObjects;++i){add(getOwner(i));add(getState(i));}
    for(int i=1;i<_numActors;++i){const Actor *a=_actors[i];add(a->_room);add(a->getRealPos().x);add(a->getRealPos().y);add(a->_frame);add(a->_costume);add(a->_moving);}
    for(int i=0;i<appbaseItem(4,0);++i){const int id=appbaseItem(5,i);add(id);add(appbaseItem(2,id));}
    if(_appbaseExtensions.active())add(_appbaseExtensions.stateHash());
    const int good=EM_ASM_INT({return Module.scummSpectatorEnd?.($0,$1>>>0)===false?0:1;},observerFrame,hash);
    if (!good) observerFailure=3;
}
void ScummEngine::appbaseSpectatorLoaded() {
    if (observerRole == 1) {
        observerReset=true;observerCheckpoint=true;observerFrame=0;
        EM_ASM({Module.scummSpectatorReset?.();});return;
    }
    if (observerRole != 2) return;
    Common::File file;
    if (!file.open(Common::FSNode(Common::Path("/appbase-spectator/context.bin")))) { observerFailure=4;return; }
    if(file.size()<8||file.size()>1048576||file.readUint32BE()!=MKTAG('A','B','S','P')||file.readUint32LE()!=1){observerFailure=4;return;}
    Common::Serializer s(&file,nullptr);appbaseSpectatorContext(s);
    if(file.err()||file.pos()!=file.size())observerFailure=4;
    observerFrame=EM_ASM_INT({return Module.scummSpectatorBaseFrame||0;});
    observerInside=false;observerReset=false;
    EM_ASM({Module.scummSpectatorRestored?.($0);},observerFrame);
}
extern "C" {
EMSCRIPTEN_KEEPALIVE int scumm_spectator_version(){return 1;}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_mode(int role){
    if(role<0||role>2||g_scumm&&(g_scumm->_game.version!=2||(g_scumm->_game.id!=GID_MANIAC&&g_scumm->_game.id!=GID_ZAK)))return 0;
    observerRole=role;observerFrame=observerFailure=0;observerInside=observerReset=observerCheckpoint=false;return 1;
}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_get(int field){return field==0?observerFrame:field==1?observerFailure:field==2?int(observerInside):observerRole;}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_checkpoint(){if(observerRole!=1)return 0;observerCheckpoint=true;return 1;}
}
} // namespace Scumm
#endif
