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
#include "scumm/gfx.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v2.h"

namespace Scumm {
void appbaseCoopObserverSync(Common::Serializer &s);
void appbaseTextObserverSync(Common::Serializer &s);
static int observerRole = 0, observerFrame = 0, observerFailure = 0;
static bool observerInside = false, observerCheckpoint = false, observerReset = false;
static int observerInput[10] = {};
// Camera observers are disposable, in-memory rendering copies. They never run
// scripts, walking, input, co-op ticks or room entry/exit code.
static bool cameraSnapshot = false, cameraDirty = false;
static int cameraMode = 0, cameraActor = 0, cameraRoom = 0, cameraX = 160, cameraSequence = 0;
static int cameraDrawnRoom = 0, cameraDrawnX = 160, cameraDrawnWidth = 320;
static bool cameraLightKnown[256] = {};
static int cameraLights[256] = {};
static void cameraLightSync(Common::Serializer &s) {
    for (bool &known : cameraLightKnown) s.syncAsByte(known);
    for (int &flags : cameraLights) s.syncAsSint32LE(flags);
}

bool ScummEngine::appbaseSpectatorReplay() const { return observerRole == 2 || observerRole == 3; }
int ScummEngine::appbaseSpectatorWait(int original) const {
    if (observerRole == 3) return 16;
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
    if (observerInside || _game.version != 2 || (_game.id != GID_MANIAC && _game.id != GID_ZAK)) return;
    if (observerRole != 3 && _setupIsComplete && _currentRoom > 0) {
        cameraLightKnown[_currentRoom] = true;cameraLights[_currentRoom] = getCurrentLights();
    }
    // Serialize only at an outer VM boundary, including when paused. Camera
    // checkpoints have their own callback and never enter the network tape.
    if ((cameraSnapshot || observerRole == 1 && observerCheckpoint) && _setupIsComplete && !_saveLoadFlag && !g_gui.isActive()) {
        Common::MemoryWriteStreamDynamic save(DisposeAfterUse::YES), context(DisposeAfterUse::YES);
        if (saveState(&save)) {
            context.writeUint32BE(MKTAG('A','B','S','P'));context.writeUint32LE(1);
            Common::Serializer s(nullptr, &context);appbaseSpectatorContext(s);
            if (observerRole == 1 && observerCheckpoint) {
                EM_ASM({ Module.scummSpectatorCheckpoint?.($0, HEAPU8.slice($1, $1+$2), HEAPU8.slice($3, $3+$4)); }, observerFrame, save.getData(), (int)save.size(), context.getData(), (int)context.size());
                observerCheckpoint = false;observerReset = false;
            }
            if (cameraSnapshot) {
                cameraLightSync(s);
                EM_ASM({ Module.scummCameraSnapshot?.(HEAPU8.slice($0, $0+$1), HEAPU8.slice($2, $2+$3)); }, save.getData(), (int)save.size(), context.getData(), (int)context.size());
                cameraSnapshot = false;
            }
        }
    }
}
void ScummEngine::appbaseCameraRender() {
    if (observerRole != 3 || !cameraDirty || !_setupIsComplete || cameraActor < 1 || cameraActor >= _numActors ||
        (!appbasePartyActor(cameraActor) && _appbaseExtensions.info(2, cameraActor) != 1)) return;
    Actor *target = _actors[cameraActor];
    const int room = cameraMode == 2 ? target->_room : cameraRoom;
    if (room < 1 || (_game.id == GID_MANIAC && room >= 54)) return;
    const bool changed = room != _currentRoom;
    if (changed) {
        _currentRoom = room;_roomResource = room >= 128 ? _resourceMapper[room & 127] : room;
        ensureResourceLoaded(rtRoom, room);
        clearDrawQueues();clearRoomObjects();setupRoomSubBlocks();resetRoomSubBlocks();
        initBGBuffers(_roomHeight);resetRoomObjects();
        memset(gfxUsageBits, 0, sizeof(gfxUsageBits));
        if (VAR_CURRENT_LIGHTS != 0xff) VAR(VAR_CURRENT_LIGHTS) = cameraLightKnown[room] ? cameraLights[room] : 11;
    }
    for (int i = 1; i < _numActors; ++i) {
        Actor *a = _actors[i];const bool visible = a->_room == room && a->_costume > 0;
        if (visible && !a->_visible) {
            ensureResourceLoaded(rtCostume, a->_costume);
            const int frame = a->_frame;
            a->_cost.reset();a->startAnimActor(a->_initFrame);a->startAnimActor(frame > 0 ? frame : a->_standFrame);
        }
        a->_visible = visible;a->_needRedraw = visible;a->_needBgReset = visible;
    }
    camera._mode = kNormalCameraMode;
    camera._cur.x = MAX(_screenWidth / 2, MIN(cameraMode == 2 ? int(target->getPos().x) : cameraX, _roomWidth - _screenWidth / 2)) & ~7;
    camera._dest = camera._cur;cameraMoved();
    _fullRedraw = true;_bgNeedsRedraw = true;
    updateObjectStates();scummLoop_handleDrawing();scummLoop_handleActors();
    _fullRedraw = false;updatePalette();drawDirtyScreenParts();camera._last = camera._cur;
    cameraDrawnRoom = _currentRoom;cameraDrawnX = camera._cur.x;cameraDrawnWidth = _roomWidth;
    cameraDirty = false;++cameraSequence;
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
    if(_saveLoadFlag==2&&_saveLoadSlot==254){scummLoop_handleSaveLoad();if(observerRole==3)appbaseCameraRender();return true;}
    if(observerRole==3){appbaseCameraRender();return true;}
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
    if (observerRole != 2 && observerRole != 3) return;
    Common::File file;
    if (!file.open(Common::FSNode(Common::Path("/appbase-spectator/context.bin")))) { observerFailure=4;return; }
    if(file.size()<8||file.size()>1048576||file.readUint32BE()!=MKTAG('A','B','S','P')||file.readUint32LE()!=1){observerFailure=4;return;}
    Common::Serializer s(&file,nullptr);appbaseSpectatorContext(s);
    if (observerRole == 3) {cameraLightSync(s);cameraDirty = true;}
    if(file.err()||file.pos()!=file.size())observerFailure=4;
    observerFrame=EM_ASM_INT({return Module.scummSpectatorBaseFrame||0;});
    observerInside=false;observerReset=false;
    EM_ASM({Module.scummSpectatorRestored?.($0);},observerFrame);
}
extern "C" {
EMSCRIPTEN_KEEPALIVE int scumm_camera_version(){return 1;}
EMSCRIPTEN_KEEPALIVE int scumm_camera_snapshot(){if(observerRole==2||observerRole==3||!g_scumm)return 0;cameraSnapshot=true;return 1;}
EMSCRIPTEN_KEEPALIVE int scumm_camera_view(int mode,int actor,int room,int x){
    if(observerRole!=3||!g_scumm||mode<1||mode>2||actor<1||actor>=g_scumm->appbaseCoopGet(0,0,3)||
       (!g_scumm->appbasePartyActor(actor)&&g_scumm->appbaseExtensionsInfo(2,actor)!=1)||room<1||room>255||x<0||x>8192)return 0;
    cameraMode=mode;cameraActor=actor;cameraRoom=room;cameraX=x;cameraDirty=true;return 1;
}
EMSCRIPTEN_KEEPALIVE int scumm_camera_get(int field){
    if(observerRole!=3||!g_scumm)return -1;
    return field==0?cameraSequence:field==1?cameraDrawnRoom:field==2?cameraDrawnX:field==3?cameraDrawnWidth:field==4?cameraActor:cameraMode;
}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_version(){return 1;}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_mode(int role){
    if(role<0||role>3||g_scumm&&(g_scumm->_game.version!=2||(g_scumm->_game.id!=GID_MANIAC&&g_scumm->_game.id!=GID_ZAK)))return 0;
    observerRole=role;observerFrame=observerFailure=0;observerInside=observerReset=observerCheckpoint=false;return 1;
}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_get(int field){return field==0?observerFrame:field==1?observerFailure:field==2?int(observerInside):observerRole;}
EMSCRIPTEN_KEEPALIVE int scumm_spectator_checkpoint(){if(observerRole!=1)return 0;observerCheckpoint=true;return 1;}
}
} // namespace Scumm
#endif
