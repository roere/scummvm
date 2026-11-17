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
#include "scumm/charset.h"
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
static uint32 cameraWorldFrame = 0, cameraPublishedFrame = ~0u;
static bool cameraStreaming = false, cameraForcePublish = false;
static int cameraSourceRoom = 0, cameraTalking = 0;
static int cameraStageTop = 0, cameraStageHeight = 0;
static byte cameraInbox[262144];
// Transient presentation state, deliberately outside the game save format.
struct CameraSpeech {
    byte room=0, native=0;
    uint32 remaining=0, sequence=0;
    byte text[256]={};
};
static Common::Array<CameraSpeech> cameraSpeech;
static uint32 cameraSpeechSequence=0;
static int cameraCaptionActor=-1;
int ScummEngine::appbaseCameraSpeech(int actor,const byte *text,bool native,int resumeMs) {
    if(observerRole==3||_game.version!=2||(_game.id!=GID_MANIAC&&_game.id!=GID_ZAK))return 0;
    const int index=actor==255?0:actor;
    if(index<0||index>=_numActors||!text)return 0;
    cameraSpeech.resize(_numActors);CameraSpeech &line=cameraSpeech[index];
    if(resumeMs>0&&line.remaining&&line.room==_actors[index]->_room)return line.remaining;
    line=CameraSpeech();line.room=index?_actors[index]->_room:_currentRoom;line.native=native;line.sequence=++cameraSpeechSequence;
    unsigned count=0;
    for(unsigned i=0;text[i]&&i<1023&&count<sizeof(line.text)-1;++i){
        const byte ch=text[i];
        if(ch==255||ch==254){
            const byte code=text[++i];if(!code)break;
            if(code==1||code==8)line.text[count++]=' ';
            else if(code>3)i+=2;
        }else if(ch==13)line.text[count++]=' ';
        else if(ch>=32&&ch!='@')line.text[count++]=ch;
    }
    line.remaining=resumeMs>0?MAX(1,resumeMs*60/1000):CLIP<int>(60+count*_defaultTextSpeed,120,720);
    return line.remaining;
}
void ScummEngine::appbaseCameraSpeechTick(int delta){
    if(observerRole==3)return;
    for(unsigned i=0;i<cameraSpeech.size();++i){
        CameraSpeech &line=cameraSpeech[i];
        if(line.native){if(!_haveMsg||getTalkingActor()!=(i?int(i):255))line.remaining=0;}
        else line.remaining=delta>=int(line.remaining)?0:line.remaining-delta;
    }
}
void ScummEngine::appbaseCameraCaption(){
    VirtScreen &screen=_virtscr[kTextVirtScreen];
    if(!screen.getPixels(0,0)||screen.h<=0)return;
    memset(screen.getPixels(0,0),0,screen.pitch*screen.h);screen.setDirtyRange(0,screen.h);
    cameraCaptionActor=-1;uint32 latest=0;
    for(unsigned i=0;i<cameraSpeech.size();++i){
        const CameraSpeech &line=cameraSpeech[i];
        if(line.remaining&&line.room==_currentRoom&&line.sequence>=latest){latest=line.sequence;cameraCaptionActor=i;}
    }
    if(cameraCaptionActor<0)return;
    const CameraSpeech &line=cameraSpeech[cameraCaptionActor];
    byte text[256];memcpy(text,line.text,sizeof(text));text[255]=0;
    const auto saved=_string[0];
    _string[0].xpos=0;_string[0].ypos=screen.topline-_screenTop;_string[0].right=_screenWidth-1;
    _string[0].center=false;_string[0].color=cameraCaptionActor?_actors[cameraCaptionActor]->_talkColor:15;
    _charset->addLinebreaks(0,text,0,_screenWidth);
    byte wrapped[270];unsigned count=0;
    for(unsigned i=0;text[i]&&count<sizeof(wrapped)-3;++i){
        if(text[i]==13){wrapped[count++]=255;wrapped[count++]=1;}
        else wrapped[count++]=text[i];
    }
    wrapped[count]=0;drawString(0,wrapped);_string[0]=saved;
}
struct CameraAnimation {
    Common::Point position;
    uint32 tick = 0;
    int room = 0, resource = 0, facing = 0;
    bool walking = false, talking = false;
};
static Common::Array<CameraAnimation> cameraAnimations;
static int cameraDrawnAnimation = 0, cameraDrawnFacing = 0, cameraDrawnWalking = 0;
static void cameraLightSync(Common::Serializer &s) {
    for (bool &known : cameraLightKnown) s.syncAsByte(known);
    for (int &flags : cameraLights) s.syncAsSint32LE(flags);
}

bool ScummEngine::appbaseCameraObserver() const { return observerRole == 3; }
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
    if (cameraForcePublish) appbaseCameraPublish();
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
                s.syncAsUint32LE(cameraWorldFrame);
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
    }
    if (VAR_CURRENT_LIGHTS != 0xff) VAR(VAR_CURRENT_LIGHTS) = cameraLightKnown[room] ? cameraLights[room] : 11;
    cameraAnimations.resize(_numActors);
    for (int i = 1; i < _numActors; ++i) {
        Actor *a = _actors[i];const bool visible = a->_room == room && a->_costume > 0;
        if (visible) {
            ensureResourceLoaded(rtCostume, a->_costume);
            CameraAnimation &pose = cameraAnimations[i];
            if (!a->_visible) {
                const bool same = pose.room == room && pose.resource == a->_costume;
                const Common::Point position = a->getRealPos();
                const int dx = position.x - pose.position.x, dy = position.y - pose.position.y;
                const bool walking = a->_moving != 0 || (same && (dx || dy));
                const bool talking = cameraTalking == i || (i<int(cameraSpeech.size())&&cameraSpeech[i].remaining);
                if (!same) {
                    a->_cost.reset();a->startAnimActor(a->_initFrame);
                    a->startAnimActor(talking ? a->_talkStartFrame : walking ? a->_walkFrame : a->_standFrame);
                } else if (walking != pose.walking || talking != pose.talking) {
                    a->startAnimActor(talking ? a->_talkStartFrame : walking ? a->_walkFrame : a->_standFrame);
                }
                if (same && (dx || dy)) a->setDirection(ABS(dx) >= ABS(dy) ? (dx > 0 ? 90 : 270) : (dy > 0 ? 180 : 0));
                // The decoder persists across compact updates. Never restore it
                // from stale offscreen save data or advance for paused redraws.
                if (same) for (uint32 tick = 0, count = MIN<uint32>(cameraWorldFrame - pose.tick, 120); tick < count; ++tick) a->animateCostume();
                pose.walking = walking;pose.talking = talking;
            } else pose.walking = a->_moving != 0;
            pose.position = a->getRealPos();pose.tick = cameraWorldFrame;
            pose.room = room;pose.resource = a->_costume;pose.facing = a->getFacing();
        }
        a->_visible = visible;a->_needRedraw = visible;a->_needBgReset = visible;
    }
    // Read-only diagnostics describe the cel that will actually be drawn.
    uint32 animation = 2166136261u;
    for (uint16 cel : target->_cost.curpos) animation = (animation ^ cel) * 16777619u;
    cameraDrawnAnimation = animation & 0x7fffffff;
    cameraDrawnFacing = target->getFacing();cameraDrawnWalking = cameraAnimations[cameraActor].walking;
    camera._mode = kNormalCameraMode;
    camera._cur.x = MAX(_screenWidth / 2, MIN(cameraMode == 2 ? int(target->getPos().x) : cameraX, _roomWidth - _screenWidth / 2)) & ~7;
    camera._dest = camera._cur;cameraMoved();
    _fullRedraw = true;_bgNeedsRedraw = true;
    updateObjectStates();scummLoop_handleDrawing();scummLoop_handleActors();
    appbaseCameraCaption();
    _fullRedraw = false;updatePalette();drawDirtyScreenParts();camera._last = camera._cur;
    // A completion signal denotes a presented SDL/WebGL frame, not just dirty
    // backend buffers. The compositor never observes an unfinished redraw.
    _system->updateScreen();
    cameraStageTop = _virtscr[kMainVirtScreen].topline - _screenTop;
    cameraStageHeight = _virtscr[kMainVirtScreen].h;
    cameraDrawnRoom = _currentRoom;cameraDrawnX = camera._cur.x;cameraDrawnWidth = _roomWidth;
    cameraDirty = false;++cameraSequence;
    EM_ASM({ Module.scummCameraPresented?.($0,$1,$2,$3,$4,$5,$6,$7); }, cameraSequence,cameraStageTop,cameraStageHeight,_screenWidth,_screenHeight,cameraSourceRoom,_currentRoom,cameraCaptionActor);

}
// Compact, versioned render state. No VM variables, script slots, inventory,
// random source, save metadata or resource teardown enter this steady path.
void ScummEngine::appbaseCameraState(Common::Serializer &s) {
    s.syncAsUint32LE(cameraWorldFrame);
    cameraSourceRoom = _currentRoom;s.syncAsSint32LE(cameraSourceRoom);
    cameraTalking = _haveMsg ? getTalkingActor() : 0;s.syncAsSint32LE(cameraTalking);
    cameraLightSync(s);
    s.syncBytes(_currentPalette, sizeof(_currentPalette));
    s.syncBytes(_roomPalette, sizeof(_roomPalette));
    s.syncBytes(_resourceMapper, sizeof(_resourceMapper));
    s.syncBytes(_objectStateTable, _numGlobalObjects);
    s.syncBytes(_objectOwnerTable, _numGlobalObjects);
    s.syncArray(_classData, _numGlobalObjects, Common::Serializer::Uint32LE);
    for (int i = 1; i < _numActors; ++i) _actors[i]->appbaseCameraSync(s);
    cameraSpeech.resize(_numActors);
    for(CameraSpeech &line:cameraSpeech){
        s.syncAsByte(line.room);s.syncAsByte(line.native);s.syncAsUint32LE(line.remaining);s.syncAsUint32LE(line.sequence);s.syncBytes(line.text,256);
        if(s.isLoading())line.text[255]=0;
    }
    if (s.isLoading()) setDirtyColors(0, 255);
}
void ScummEngine::appbaseCameraPublish() {
    if (!cameraStreaming || observerRole == 3 || !_setupIsComplete || _game.version != 2 ||
        _saveLoadFlag || _currentRoom < 1 || g_gui.isActive() ||
        (!cameraForcePublish && cameraPublishedFrame == cameraWorldFrame)) return;
    cameraLightKnown[_currentRoom] = true;cameraLights[_currentRoom] = getCurrentLights();
    Common::MemoryWriteStreamDynamic out(DisposeAfterUse::YES);
    out.writeUint32BE(MKTAG('A','B','C','V'));out.writeUint16LE(2);
    out.writeUint16LE(_numActors);out.writeUint16LE(_numGlobalObjects);
    Common::Serializer s(nullptr, &out);appbaseCameraState(s);
    cameraPublishedFrame = cameraWorldFrame;cameraForcePublish = false;
    EM_ASM({ Module.scummCameraFrame?.(HEAPU8.slice($0, $0+$1)); }, out.getData(), (int)out.size());
}
bool ScummEngine::appbaseCameraReceive(Common::SeekableReadStream &in) {
    if (in.size() < 1024 || in.size() > sizeof(cameraInbox) ||
        in.readUint32BE() != MKTAG('A','B','C','V') || in.readUint16LE() != 2 ||
        in.readUint16LE() != _numActors || in.readUint16LE() != _numGlobalObjects ||
        in.size() != 2454 + 6 * _numGlobalObjects + 698 * (_numActors - 1) + 266 * _numActors) return false;
    Common::Serializer s(&in, nullptr);appbaseCameraState(s);
    if (in.err() || in.pos() != in.size()) return false;
    cameraDirty = true;return true;
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
    if(observerRole==3){
        const int size = EM_ASM_INT({
            const frame = Module.scummCameraTakeFrame?.();
            if (!frame) return 0;
            if (!(frame instanceof Uint8Array) || frame.byteLength > $1) return -1;
            HEAPU8.set(frame, $0);return frame.byteLength;
        }, cameraInbox, sizeof(cameraInbox));
        if (size < 0) { observerFailure = 5;EM_ASM({ Module.scummCameraFailed?.(); });return true; }
        if (size > 0) {
            Common::MemoryReadStream in(cameraInbox, size);
            if (!appbaseCameraReceive(in)) { observerFailure = 5;EM_ASM({ Module.scummCameraFailed?.(); });return true; }
        }
        appbaseCameraRender();return true;
    }
    const int accepted = EM_ASM_INT({const row=Module.scummSpectatorTakeFrame?.($0);if(!row)return 0;if(!Array.isArray(row)||row.length!==10)return -1;for(let i=0;i<10;++i)HEAP32[($1>>2)+i]=row[i]|0;return 1;}, observerFrame+1, observerInput);
    if (accepted <= 0) { if (accepted < 0) observerFailure = 1;return true; }
    ++observerFrame;observerInside = true;delta = observerInput[0];_rnd.setSeed((uint32)observerInput[1]);
    _mouse.x=observerInput[2];_mouse.y=observerInput[3];_leftBtnPressed=observerInput[4];_rightBtnPressed=observerInput[5];
    _keyPressed=Common::KeyState(static_cast<Common::KeyCode>(observerInput[6]),observerInput[7],observerInput[8]);
    _defaultTextSpeed=observerInput[9];setTalkSpeed(_defaultTextSpeed);
    return false;
}
void ScummEngine::appbaseSpectatorInput() {
    // Called only after the peer/co-op hold checks, once per live world frame.
    if (observerRole != 3) ++cameraWorldFrame;
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
    if(observerRole!=3)cameraSpeech.clear();
    if (observerRole == 1) {
        observerReset=true;observerCheckpoint=true;observerFrame=0;
        EM_ASM({Module.scummSpectatorReset?.();});return;
    }
    if (observerRole != 2 && observerRole != 3) return;
    Common::File file;
    if (!file.open(Common::FSNode(Common::Path("/appbase-spectator/context.bin")))) { observerFailure=4;return; }
    if(file.size()<8||file.size()>1048576||file.readUint32BE()!=MKTAG('A','B','S','P')||file.readUint32LE()!=1){observerFailure=4;return;}
    Common::Serializer s(&file,nullptr);appbaseSpectatorContext(s);
    if (observerRole == 3) {cameraLightSync(s);s.syncAsUint32LE(cameraWorldFrame);cameraDirty = true;}
    if(file.err()||file.pos()!=file.size())observerFailure=4;
    observerFrame=EM_ASM_INT({return Module.scummSpectatorBaseFrame||0;});
    observerInside=false;observerReset=false;
    EM_ASM({Module.scummSpectatorRestored?.($0);},observerFrame);
}
extern "C" {
EMSCRIPTEN_KEEPALIVE int scumm_camera_version(){return 2;}
EMSCRIPTEN_KEEPALIVE int scumm_camera_stream(int enabled){
    if ((enabled != 0 && enabled != 1) || observerRole == 2 || observerRole == 3 || !g_scumm) return 0;
    cameraStreaming = enabled != 0;cameraForcePublish = cameraStreaming;return 1;
}
EMSCRIPTEN_KEEPALIVE int scumm_camera_snapshot(){if(observerRole==2||observerRole==3||!g_scumm)return 0;cameraSnapshot=true;return 1;}
EMSCRIPTEN_KEEPALIVE int scumm_camera_view(int mode,int actor,int room,int x){
    if(observerRole!=3||!g_scumm||mode<1||mode>2||actor<1||actor>=g_scumm->appbaseCoopGet(0,0,3)||
       (!g_scumm->appbasePartyActor(actor)&&g_scumm->appbaseExtensionsInfo(2,actor)!=1)||room<1||room>255||x<0||x>8192)return 0;
    cameraMode=mode;cameraActor=actor;cameraRoom=room;cameraX=x;cameraDirty=true;return 1;
}
EMSCRIPTEN_KEEPALIVE int scumm_camera_get(int field){
    if(observerRole!=3||!g_scumm)return -1;
    return field==0?cameraSequence:field==1?cameraDrawnRoom:field==2?cameraDrawnX:field==3?cameraDrawnWidth:field==4?cameraActor:field==5?cameraMode:field==6?cameraDrawnAnimation:field==7?cameraWorldFrame:field==8?cameraDrawnWalking:field==9?cameraDrawnFacing:field==10?cameraStageTop:field==11?cameraStageHeight:field==12?cameraCaptionActor:-1;
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
