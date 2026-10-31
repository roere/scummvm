/* ScummVM AppBase local co-op experiment. GPL-3.0-or-later.
 * Two independent room/script contexts; only the explicitly bridged state
 * is shared. This is not a claim of whole-game script determinism. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "common/array.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v2.h"
#include "scumm/actor.h"
#include "scumm/object.h"
#include "scumm/resource.h"
#include "scumm/sound.h"
#include "scumm/music.h"

namespace Scumm {
struct CoopEvent { int v[8]; };
static Common::Array<CoopEvent> incoming;
static int localActor = 0, remoteActor = 0, ticks = 0;
static bool pendingPickup[1024] = {};
static bool refreshPlant = false, refreshArcade = false;
static int arcadeReplay = -1;
static int selectionInputSlot = -1;
static int sceneStage=0,sceneScript=0,sceneActor=0;
static bool sceneReleased=false;
// Scope a character request to an actual mouse/key dispatch, never a story VM
// write or a continuation of an older script after a cutscene.
void appbaseCoopInput(int area, int value) {
	selectionInputSlot = area==1 && value>=20 && value<=22 ? value-20 :
		(area==4 && value>=1 && value<=3 ? value-1 : -1);
}
static bool applying = false, frozen = true;
static CoopEvent remoteMove;
static bool predicting = false;
static int lastRoom = -1, lastMoving = 0;
static bool lightKnown[256] = {};
static int roomLight[256] = {};
static bool mansionEntrySent = false,fridgeHungerSent=false;
static Common::Array<CoopEvent> conflictSpeech;
static int conflictStage=0;
struct ChatSpeech { int actor,room,age; byte text[161]; };
static Common::Array<ChatSpeech> chatSpeech;
static byte chatText[161] = {};
static int chatLength=0,chatDeliveryStatus=0;
static bool preparatoryFrozen=false,pendingIntroStart=false,introAutoHold=false;
static int pendingSkipScript=-1,pendingSkipTicks=0;
// Native shared-character replication. Participants execute original scripts;
// only bounded input/timer/RNG records travel through the normal party exchange.
static int peerRole=0,peerFrame=0,peerAck=0,peerFailed=0,peerMusic=0,peerTextSpeed=0;
static bool peerInFrame=false;
static Common::Array<CoopEvent> peerQueue,peerRecord,peerInputs,peerSounds,peerRandom,peerChat;
static CoopEvent peerEnd,peerInput;
static int peerKeyFlags=0;
static void peerRecordEvent(int kind,int a=0,int b=0,int c=0,int d=0,int e=0,int f=0) {
    CoopEvent record={{kind,peerFrame,a,b,c,d,e,f}};peerRecord.push_back(record);
}
bool ScummEngine::appbasePeerTick(int &delta) {
    if(!peerRole)return false;
    if(preparatoryFrozen || frozen || peerFailed)return true;
    if(peerRole==1 && peerFrame-peerAck>=60)return true;
    peerInputs.clear();peerSounds.clear();peerRandom.clear();
    if(peerRole==2) {
        int end=-1;
        for(uint i=0;i<peerQueue.size();++i)if(peerQueue[i].v[0]==27){end=i;break;}
        if(end<0)return true;
        const CoopEvent start=peerQueue[0],clock=peerQueue[1];
        if(start.v[0]!=23 || clock.v[0]!=24 || start.v[1]!=peerFrame+1 || clock.v[1]!=start.v[1]){peerFailed=1;return true;}
        peerInput=start;peerKeyFlags=clock.v[5];
        peerFrame=start.v[1];delta=start.v[2];_mouse.x=start.v[3];_mouse.y=start.v[4];
        _leftBtnPressed=start.v[5]&15;_rightBtnPressed=(start.v[5]>>4)&15;
        _keyPressed=Common::KeyState(static_cast<Common::KeyCode>(start.v[6]),start.v[7],clock.v[5]);
        _rnd.setSeed(uint32(clock.v[2])|(uint32(clock.v[3])<<16));peerMusic=clock.v[4];peerTextSpeed=clock.v[7];static_cast<ScummEngine_v2 *>(this)->appbasePeerInventory(clock.v[6]);
        for(int i=2;i<end;++i) {
            const CoopEvent &r=peerQueue[i];
            if(r.v[0]==25)peerInputs.push_back(r);
            if(r.v[0]==30)peerSounds.push_back(r);
            if(r.v[0]==31)peerRandom.push_back(r);
            if(r.v[0]==26 && !EM_ASM_INT({return Module.mmReplaySpeech ? Module.mmReplaySpeech($0) : 0;},r.v[2]))peerFailed=5;
        }
        peerEnd=peerQueue[end];for(int i=0;i<=end;++i)peerQueue.remove_at(0);
    } else {
        ++peerFrame;peerRecord.clear();
        peerMusic=_musicEngine?_musicEngine->getMusicTimer()*getTimerFrequency()/240.0:0;
        const uint32 seed=_rnd.getSeed();
        const bool localUiKey=_keyPressed.ascii=='+' || _keyPressed.ascii=='-' || _keyPressed.ascii=='[' || _keyPressed.ascii==']' || (_keyPressed.flags&(Common::KBD_CTRL|Common::KBD_ALT|Common::KBD_META));
        peerRecordEvent(23,delta,MAX(0,int(_mouse.x)),MAX(0,int(_mouse.y)),(_leftBtnPressed&15)|((_rightBtnPressed&15)<<4),localUiKey?0:_keyPressed.keycode,localUiKey?0:_keyPressed.ascii);
        peerRecordEvent(24,seed&65535,seed>>16,peerMusic,_keyPressed.flags,static_cast<ScummEngine_v2 *>(this)->appbasePeerInventory(-1),0);
        for(uint i=0;i<peerChat.size();++i){CoopEvent r=peerChat[i];r.v[1]=peerFrame;peerRecord.push_back(r);}peerChat.clear();
    }
    peerInFrame=true;return false;
}
void ScummEngine::appbasePeerBeforeInput() {
    if(!peerRole || !peerInFrame)return;
    // Display/dialog preparation can poll events. Capture the input actually
    // consumed by processInput, rather than an earlier sample of this frame.
    if(peerRole==1) {
        int *v=peerRecord[0].v;
        const bool ui=_keyPressed.ascii=='+' || _keyPressed.ascii=='-' || _keyPressed.ascii=='[' || _keyPressed.ascii==']' || (_keyPressed.flags&(Common::KBD_CTRL|Common::KBD_ALT|Common::KBD_META));
        v[3]=MAX(0,int(_mouse.x));v[4]=MAX(0,int(_mouse.y));v[5]=(_leftBtnPressed&15)|((_rightBtnPressed&15)<<4);v[6]=ui?0:_keyPressed.keycode;v[7]=ui?0:_keyPressed.ascii;peerRecord[1].v[5]=_keyPressed.flags;
    } else {
        _mouse.x=peerInput.v[3];_mouse.y=peerInput.v[4];_leftBtnPressed=peerInput.v[5]&15;_rightBtnPressed=(peerInput.v[5]>>4)&15;
        _keyPressed=Common::KeyState(static_cast<Common::KeyCode>(peerInput.v[6]),peerInput.v[7],peerKeyFlags);
    }
}
void ScummEngine::appbasePeerAfterInput() {
    if(!peerRole || !peerInFrame)return;
    // Native settings dialogs belong to the controller's UI. Replicate their
    // completed text-speed setting without opening an unattended peer dialog.
    if(peerRole==1){setTalkSpeed(_defaultTextSpeed);peerRecord[1].v[7]=_defaultTextSpeed;}
    else {_defaultTextSpeed=peerTextSpeed;setTalkSpeed(peerTextSpeed);if(VAR_CHARINC!=0xff)VAR(VAR_CHARINC)=9-peerTextSpeed;}
}
void ScummEngine::appbasePeerMusic() { if(peerRole && peerInFrame && VAR_MUSIC_TIMER!=0xff)VAR(VAR_MUSIC_TIMER)=peerMusic; }
void ScummEngine::appbasePeerInput(int area,int value,int mode) { if(peerRole==1 && peerInFrame)peerRecordEvent(25,area,value,mode); }
bool ScummEngine::appbasePeerInputs() {
    if(peerRole!=2 || !peerInFrame)return false;
    for(uint i=0;i<peerInputs.size();++i)runInputScript(peerInputs[i].v[2],peerInputs[i].v[3],peerInputs[i].v[4]);
    return true;
}
int ScummEngine::appbasePeerSound(int sound,int value) {
    if(!peerInFrame || !peerRole)return value;
    if(peerRole==1){peerRecordEvent(30,sound,value);return value;}
    if(peerSounds.empty() || peerSounds[0].v[2]!=sound){peerFailed=2;return value;}
    const int result=peerSounds[0].v[3];peerSounds.remove_at(0);return result;
}
void ScummEngine::appbasePeerEnd() {
    if(!peerRole || !peerInFrame)return;
    peerInFrame=false;
    // Check logical progress, not language-specific pixels or audio buffers.
    uint32 hash=2166136261u;
    const auto add=[&](int v){hash=(hash^uint32(v))*16777619u;};
    add(_currentRoom);add(VAR(VAR_EGO));add(_userPut);
    for(int id=0;id<_numVariables;++id)if(id!=VAR_HAVE_MSG && id!=VAR_CHARCOUNT)add(_scummVars[id]);
    for(int id=0;id<NUM_SCRIPT_SLOT;++id){add(vm.slot[id].number);add(vm.slot[id].status);add(vm.slot[id].delay);}
    for(int id=1;id<_numGlobalObjects;++id){add(getOwner(id));add(getState(id));}
    for(int id=1;id<_numActors;++id){add(_actors[id]->_room);add(_actors[id]->getRealPos().x);add(_actors[id]->getRealPos().y);}
    if(peerRole==1) {
        peerRecordEvent(27,hash&65535,hash>>16);
        for(uint i=0;i<peerRecord.size();++i){const int *v=peerRecord[i].v;EM_ASM({if(Module.mmSharedEvent)Module.mmSharedEvent([$0,$1,$2,$3,$4,$5,$6,$7]);},v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);}
    } else if(peerEnd.v[2]!=int(hash&65535) || peerEnd.v[3]!=int(hash>>16))peerFailed=3;
    EM_ASM({Module.mmSharedProgress={};Module.mmSharedProgress.frame=$0;Module.mmSharedProgress.checksum=$1>>>0;Module.mmSharedProgress.failed=$2;},peerFrame,hash,peerFailed);
}
static bool presentationObject(int obj) { return (obj>=260 && obj<=265) || obj==395 || obj==403 || (obj>=541 && obj<=553); }
static void emit(int kind, int id, int room, int a=0, int b=0, int c=0, int d=0, int e=0) {
	if (!localActor || applying) return;
	EM_ASM({ if (Module.mmCoopEvent) Module.mmCoopEvent([$0,$1,$2,$3,$4,$5,$6,$7]); },kind,id,room,a,b,c,d,e);
}
int ScummEngine::appbaseCoopRandom(int maximum,int value) {
    if(peerRole && peerInFrame) {
        if(peerRole==1){peerRecordEvent(31,maximum,value);return value;}
        if(peerRandom.empty() || peerRandom[0].v[2]!=maximum){peerFailed=4;return value;}
        const int result=peerRandom[0].v[3];peerRandom.remove_at(0);return result;
    }
    // Replay only the already accepted original high-score graphic. Other
    // random calls, including the first real choice, retain their normal RNG.
    return applying && arcadeReplay>=0 && maximum==3 && _currentScript!=0xff &&
        vm.slot[_currentScript].where==WIO_GLOBAL && vm.slot[_currentScript].number==102
        ? arcadeReplay : value;
}
bool ScummEngine::appbaseCoopManualGive(int recipient) {
    if(!localActor)return false;
    Actor *a=_actors[localActor];
    emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
    emit(21,10001,a->_room,localActor,recipient);
    return true; // Ownership changes only after the canonical server reply.
}
bool ScummEngine::appbaseCoopItemGive(int item,int recipient) {
    if(!localActor)return false;
    if(_game.id==GID_MANIAC && item==10001)return appbaseCoopManualGive(recipient);
    Actor *a=_actors[localActor];
    emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
    emit(22,item,a->_room,localActor,recipient);return true;
}
bool ScummEngine::appbaseCoopActive() const { return localActor != 0; }
int ScummEngine::appbaseCoopActor() const { return localActor; }
int ScummEngine::appbaseCoopPrepare(int operation) {
	if((_game.id!=GID_MANIAC && _game.id!=GID_ZAK) || _game.version!=2) return 0;
	if(operation==0 || operation==1) { preparatoryFrozen=operation==1;frozen=operation==1;return 1; }
	if(operation==2 && !localActor && _currentRoom==45 && _scummVars[111]>=3) { pendingIntroStart=true;introAutoHold=true;return 1; }
	// Join during the original intro: resume its held VM context, without START.
	if(operation==3 && !localActor && (isScriptRunning(120) || isScriptRunning(131))) { preparatoryFrozen=false;frozen=false;introAutoHold=true;return 1; }
	return 0;
}
void ScummEngine::appbaseCoopSeed(uint32 seed) { _rnd.setSeed(seed); }
bool ScummEngine::appbaseCoopVerb(int verb) const {
	return _game.id!=GID_MANIAC || !localActor || verb<20 || verb>22 || _scummVars[47+verb-20]!=remoteActor;
}
int ScummEngine::appbaseCoopSelect(int actor) {
	if (!localActor || sceneStage==2) return actor;
    if(_game.id==GID_ZAK) {
        // Zak's original SWITCH sentence (script 2), distinct from temporary
        // EGO writes used by original movies and animal-control puzzles.
        const bool selection=!applying && _userPut>0 && _currentScript!=0xff &&
            vm.slot[_currentScript].where==WIO_GLOBAL && vm.slot[_currentScript].number==2 &&
            VAR(VAR_ACTIVE_VERB)==45 && VAR(VAR_ACTIVE_OBJECT2)==actor && actor>=1 && actor<=4 && appbaseCoopGet(8,actor,0)==1;
        if(!selection)return actor;
        if(actor!=localActor && actor!=remoteActor) {
            Actor *a=_actors[localActor];a->stopActorMoving();
            emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
            emit(6,actor,0);
        }
        return localActor;
    }
	const bool explicitSelection = !applying && _userPut>0 && selectionInputSlot>=0 &&
		actor==_scummVars[47+selectionInputSlot] && _currentScript!=0xff &&
		vm.slot[_currentScript].where==WIO_GLOBAL && vm.slot[_currentScript].number==4;
	selectionInputSlot=-1;
	if (!explicitSelection) return localActor;
	if (actor!=localActor && actor!=remoteActor && actor>=1 && actor<=7 &&
		(actor==_scummVars[47] || actor==_scummVars[48] || actor==_scummVars[49])) {
		Actor *a=_actors[localActor];a->stopActorMoving();
		emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
		emit(6,actor,0);
	}
	return localActor; // only the authenticated server grants the new controller
}
bool ScummEngine::appbaseCoopPickup(int obj) {
    if (!localActor) return false;
    if(sceneStage==2 && localActor!=sceneActor)return false;
    if(obj>0 && obj<1024)pendingPickup[obj]=true;
    const int ego=VAR(VAR_EGO);
    const bool animal=_game.id==GID_ZAK && ego>=5 && ego<=6 && _actors[ego]->_room==_currentRoom;
    // The authenticated hero stays behind during the original animal puzzle.
    // Carry the animal's original data room separately from the seat's pose.
    emit(2,obj,_game.id==GID_ZAK?_actors[localActor]->_room:_roomResource,localActor,
         animal?_currentRoom:0,animal?ego:0);
    if(sceneStage==2)return false;
	return true; // inventory is changed only after the sequencer accepts it
}
void ScummEngine::appbaseCoopTape(int var,int value) {
    if(_game.id!=GID_MANIAC)return;
    // Plant growth and sedation also control the original climbing walkbox.
    // Report only original watering/Pepsi writes, not arbitrary scene globals.
    if(localActor && !applying && _roomResource==27 &&
       (sceneStage!=2 || localActor==sceneActor) && VAR(VAR_ACTIVE_OBJECT1)==304) {
        const int item=VAR(VAR_ACTIVE_OBJECT2);
        if((var==174 && (item==123 || item==132)) || (var==193 && item==93))
            emit(20,var,_actors[localActor]->_room,var==174?(value==255?255:MIN(value,2)):(value?1:0),localActor,0,item);
    }
    // The original cassette contents are global variable 157, not the tape's
    // owner or display flags. Report real recording writes, including a repeat
    // recording of the same song, only from the executing room-17 source.
    if(localActor && !applying && var==157 && _roomResource==17 &&
       (sceneStage!=2 || localActor==sceneActor))
        emit(19,var,_roomResource,value,localActor);
}
void ScummEngine::appbaseCoopState(int obj,int state) {
	if (localActor && !applying && (sceneStage!=2 || localActor==sceneActor) && (_game.id!=GID_MANIAC || !presentationObject(obj)) && _objectStateTable[obj] != state)
		emit(3,obj,_game.id==GID_ZAK?_currentRoom:(_roomResource>0?_roomResource:_actors[localActor]->_room),state);
}
bool ScummEngine::appbaseCoopWalk(int actor,int room,int x,int y,int dir) {
	if (!localActor || applying || sceneStage==2) return true;
	if (actor == remoteActor) return false; // only the owning engine issues movement
	if (actor != localActor || room <= 0) return true;
	Actor *a=_actors[actor];
	emit(1,actor,room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
	emit(4,actor,room,x,y,dir < 0 ? 360 : dir);
	return true;
}
bool ScummEngine::appbaseCoopTransfer(int object,int owner) {
	if (!localActor || applying || object<=0 || object>=_numGlobalObjects) return false;
	int from=getOwner(object);
    const int ego=VAR(VAR_EGO);
    const int candidate=ego>=5 && ego<=6?ego:from;
    const int animal=_game.id==GID_ZAK && candidate>=5 && candidate<=6 &&
        getOwner(55)==localActor?candidate:0;
    auto consume=[&](){
        int usage=0,before=0,drank=0;
        if(_game.id==GID_MANIAC && owner==0 && VAR(VAR_ACTIVE_OBJECT2)==14 && _currentScript!=0xff && vm.slot[_currentScript].where==WIO_GLOBAL){
            const int script=vm.slot[_currentScript].number;
            if(script==103 || script==104){
                usage=script==103?(_scummVars[123]==198?3:1):2;
                before=script==103?MAX(0,int(_scummVars[98])-1):int(_scummVars[98]);
                drank=_scummVars[99]?1:0;
            }
        }
        emit(7,object,_game.id==GID_ZAK?_actors[localActor]->_room:_roomResource,localActor,owner,
             animal?animal:usage,animal?_currentRoom:before,drank);
    };
    // pickupObject can be followed immediately by setOwner in one original
    // script (radio batteries are stored inside the picked-up radio). The
    // canonical grant is still in flight, so preserve its following ownership
    // opcode in the same ordered transaction rather than undoing it on reply.
    if(object<1024 && pendingPickup[object] && from!=localActor &&
       (owner==0 || owner==14 || owner==OF_OWNER_ROOM)){
        pendingPickup[object]=false;consume();return false;
    }
    if(sceneStage==2){
        if(localActor==sceneActor && from==localActor && (owner==0 || owner==14 || owner==OF_OWNER_ROOM))consume();
        return false;
    }
    // Original animal control disables SWITCH (global199), but still permits
    // giving an item back to the hero standing in the same original room.
    if(animal && from==animal && owner>=1 && owner<=4 && _actors[owner]->_room==_currentRoom) {
        Actor *a=_actors[localActor];
        emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
        emit(5,object,a->_room,localActor,owner,animal,_currentRoom);
        return true;
    }
    if(animal && from==animal && (owner==0 || owner==14 || owner==OF_OWNER_ROOM)) {
        consume();return false;
    }
    // Original v2 uses owner 14 to store a record on the Victrola or a tape
    // inside the recorder. Taking it back is an original inventory ownership
    // opcode, not pickupObject; route that through the same canonical grant.
    if(from==14 && owner==localActor){emit(2,object,_game.id==GID_ZAK?_actors[localActor]->_room:_roomResource,localActor);return true;}
	if (from==localActor && (owner==remoteActor || (_game.id==GID_ZAK && owner>=1 && owner<=4 && owner!=localActor && appbaseCoopGet(8,owner,0)==1))) {
		Actor *a=_actors[localActor];
		emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
		emit(5,object,a->_room,localActor,owner);
		return true; // only confirmed ownership changes the inventories
	}
    if (from==localActor && (owner==0 || owner==14 || owner==OF_OWNER_ROOM)) {
        consume();
        // Keep the original script semantics (owner 0 can end its inventory code).
        return false;
    }
    return from==remoteActor && owner!=remoteActor;
}
void ScummEngine::appbaseCoopSound(int sound,int operation) {
    if (!localActor || applying || sound<1 || sound>255 || _currentRoom<=0 ||
        _actors[localActor]->_room!=_currentRoom || sceneStage!=0) return;
    emit(14,sound,_currentRoom,operation);
}
bool ScummEngine::appbaseCoopSceneRunning() const { return sceneStage==2; }
bool ScummEngine::appbaseCoopSceneBegin() {
    if(_game.id!=GID_MANIAC || !localActor || _currentScript==0xff || vm.slot[_currentScript].where!=WIO_GLOBAL) return false;
    const int script=vm.slot[_currentScript].number;
    const int offset=_scriptPointer-_scriptOrgPointer-1;
    // A second original scene may reach its entry in the same VM frame as
    // preparation/completion of the server-approved scene. Yield only that
    // script; never freeze the approved scene or execute an unrelated cutscene
    // inside its shared barrier. It will request its own turn after release.
    // Original police victory calls 63 inside 126. This one vetted child
    // belongs to the same barrier; serializing it behind its endless parent
    // would prevent the real congratulations from ever executing.
    if(sceneStage==2 && sceneScript==126 && script==63 && offset==32 && _scummVars[162]==3)return false;
    if(sceneStage!=0 && script!=sceneScript)return true;
    if(sceneStage==3 || (sceneStage==1 && !sceneReleased))return true;
    if(sceneReleased && script==sceneScript){sceneStage=2;return false;}
    if(sceneStage==2) return false;
    const bool known=(script==1 && offset==32) || (script==100 && offset==4) || (script==101 && offset==54) || (script==105 && offset==42) || (script==112 && offset==4) || (script==119 && offset==4) || (script==120 && offset==4) || (script==126 && offset==12) || (script==130 && offset==5) || (script==131 && offset==10) || (script==140 && offset==4) || (script==142 && offset==4) || (script==145 && offset==4) || (script==148 && offset==8) || (script==151 && offset==50) || (script==155 && offset==4) || (script==157 && offset==4) || (script==163 && offset==10) || (script==175 && offset==4) || (script==23 && offset==5) || (script==27 && offset==45) || (script==35 && offset==20) || (script==37 && offset==4) || (script==42 && offset==8) || (script==43 && offset==16) || (script==47 && offset==5) || (script==48 && offset==27) || (script==49 && offset==237) || (script==61 && offset==5) || (script==62 && offset==27) || (script==63 && offset==32) || (script==87 && offset==35) || (script==88 && offset==283) || (script==89 && offset==4) || (script==9 && offset==4) || (script==95 && offset==7) || (script==97 && offset==8) || (script==99 && offset==10);
    if(!known) return false;
    frozen=true;
    emit(12,script,offset,script==37?_scummVars[73]:0);
    return true;
}
void ScummEngine::appbaseCoopSceneEnd() {
    if(sceneStage!=2 || _currentScript==0xff || vm.slot[_currentScript].number!=sceneScript)return;
    sceneStage=3;frozen=true;
    VAR(VAR_EGO)=localActor;actorFollowCamera(localActor);
}
int ScummEngine::appbaseCoopSceneCommand(int operation,int script,int offset,int actor) {
    if(!localActor)return 0;
    if(operation==0)return sceneStage;
    if(operation==1){
        if(script<1 || script>=_numGlobalScripts || offset<4 || offset>65535 || actor<1 || actor>7)return 0;
        const byte *code=getResourceAddress(rtScript,script);
        if(!code || offset>=getResourceSize(rtScript,script) || code[offset]!=0x40)return 0;
        CoopEvent event={{17,script,offset,actor,0,0,0,0}};incoming.push_back(event);return 1;
    }
    if(operation==2 && (sceneStage==1 || sceneStage==2)){sceneReleased=true;frozen=false;return 1;}
    if(operation==3 && sceneStage==3){sceneStage=0;sceneScript=0;sceneActor=0;sceneReleased=false;return 1;}
    return 0;
}
int ScummEngine::appbaseCoopLight(int flags) {
	if (!localActor || applying || sceneStage!=0 || _currentRoom<=0 || _currentRoom>=(_game.id==GID_ZAK?256:54)) return flags;
	Actor *a=_actors[localActor];
	// Story scenes can load a different room while the controlled kid stays put.
	// They must not publish a fictional player pose or that room's light default.
	if(a->_room!=_currentRoom) return flags;
	bool entering=_currentRoom!=lastRoom;
	if (entering && lightKnown[_currentRoom]) return roomLight[_currentRoom];
	emit(1,localActor,_currentRoom,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
	emit(8,_currentRoom,flags,entering?1:0);
	return flags;
}
extern bool appbaseInitialFramePresented();
int ScummEngine::appbaseCoopGet(int kind,int id,int field) {
	if ((_game.id != GID_MANIAC && _game.id != GID_ZAK) || _game.version != 2) return -1;
    // Browser callbacks may run before native setup has allocated variables.
    if(!_setupIsComplete)return 0;
    // Original Zak SWITCH availability: scripts 4/19 gate the verb with
    // global 199 and state04 of object 169; per-actor bit 1470 disables a kid.
    if(kind==10 && _game.id==GID_ZAK)return _setupIsComplete && appbaseInitialFramePresented() && _numActors>1 && _scummVars && _currentRoom>0 && _roomResource>0 && _currentRoom!=50 && !isScriptRunning(7) && appbaseTextCanSave()?1:0;
    if(kind==8 && _game.id==GID_ZAK && id>=1 && id<=4){
        if(!_setupIsComplete || id>=_numActors || !_scummVars)return 0;
        const int bit=1470+id;
        return _scummVars[199]!=0 && (getState(169)&kObjectStateLocked)!=0 &&
            (_scummVars[bit>>4]&(1<<(bit&15)))==0 && _actors[id]->_room>0 ? 1:0;
    }
    if(kind==9 && _game.id==GID_ZAK && id>0 && id<=255)return id>127?_resourceMapper[id&127]:id;
    // Read-only UI inventory order for browser acceptance tests. Never mutate
    // the original object table or expose a way to grant an item.
    if(kind==7 && id>=1 && id<=7){
        const int extra=appbaseItem(0,id);
        if(field==0)return getInventoryCount(id)+extra;
        if(field<1)return 0;
        if(field<=extra)return appbaseItem(1,id,field);
        return findInventory(id,field-extra);
    }
	if (kind == 0) {
		switch(field) {
		case 0:return _currentRoom; case 1:return VAR(VAR_EGO);
		case 2:return _numGlobalObjects; case 3:return _numActors;
		case 4:return OF_OWNER_ROOM; case 5:return camera._cur.x;
		case 6:return localActor; case 7:return ticks; case 8:return getCurrentLights();
		case 9:return _scummVars[47]; case 10:return _scummVars[48]; case 11:return _scummVars[49];
		case 12:return remoteActor;
		case 13:return _haveMsg; case 14:return _numVariables; case 15:return NUM_SCRIPT_SLOT;
		case 16:return conflictStage; case 17:return conflictSpeech.size();
		case 18:return conflictSpeech.empty()?0:conflictSpeech[0].v[1];
		case 19:return conflictSpeech.empty()?0:conflictSpeech[0].v[3];
		case 20:return preparatoryFrozen;case 21:return _userPut;
		case 22:return vm.cutScenePtr[0]?vm.slot[vm.cutSceneScript[0]].number:0;
		case 23:return static_cast<int>(_rnd.getSeed());
		case 24:return _game.id==GID_MANIAC && !localActor?(isScriptRunning(120)?120:(isScriptRunning(131)?131:0)):0;
		case 25:return sceneScript;
        case 26:return appbaseManualOwner(0);
        case 27:return _roomResource;
        case 28:return _userPut>0 && !vm.cutSceneStackPointer;
        case 29:return chatSpeech.size();case 30:return chatDeliveryStatus;
		}
	}
	if (kind == 1 && id > 0 && id < _numActors) {
		Actor *a=_actors[id];
		switch(field) {
		case 0:return a->_room; case 1:return a->getRealPos().x;
		case 2:return a->getRealPos().y; case 3:return a->getFacing();
		case 4:return a->_frame; case 5:return a->_costume; case 6:return a->_moving;
		}
	}
	if (kind == 2 && id > 0 && id < _numGlobalObjects) {
		if(field == 0)return getOwner(id);
		if(field == 1)return getState(id);
		if(field == 2)return _game.id==GID_MANIAC && presentationObject(id)?1:0;
	}
	if (kind == 3 && id > 0 && id < _numLocalObjects) {
		const ObjectData &o=_objs[id];
		if(field == 0)return o.obj_nr;
		if(field == 1)return o.x_pos;
		if(field == 2)return o.y_pos;
		if(field == 3)return o.width;
		if(field == 4)return o.height;
	}
	if(kind==6 && id>=1 && id<=255) return _sound->isSoundRunning(id)?1:0;
	if(kind==4 && id>=0 && id<_numVariables && field==0) return _scummVars[id];
	if(kind==5 && id>=0 && id<NUM_SCRIPT_SLOT) {
		if(field==0) return vm.slot[id].number;
		if(field==1) return vm.slot[id].where;
		if(field==2) return vm.slot[id].status;
		if(field==3) return vm.slot[id].delay;
	}
	return -1;
}
bool ScummEngine::appbaseCoopTick() {
    const auto chatTick=[&]() {
    if(chatSpeech.empty())return;
    ChatSpeech &line=chatSpeech[0];++line.age;
    // A new cutscene or loss of control discards queued user speech.
    if(line.age>600 || !appbaseCoopGet(0,0,28) || _currentRoom!=line.room || _actors[line.actor]->_room!=line.room) { chatDeliveryStatus=line.age>600?5:(!appbaseCoopGet(0,0,28)?3:4);chatSpeech.remove_at(0);return; }
    if(!_haveMsg && conflictSpeech.empty()) {
        _actorToPrintStrFor=line.actor;
        EM_ASM({ Module.mmChatSpeaking=true; });
        actorTalk(line.text);chatDeliveryStatus=2;
        EM_ASM({ Module.mmChatSpeaking=false; });
        chatSpeech.remove_at(0);
    }
    };
    appbaseManual(2);
	if(pendingSkipScript>=0) {
		if(vm.cutScenePtr[0] && (pendingSkipScript==0 || vm.slot[vm.cutSceneScript[0]].number==pendingSkipScript)) { abortCutscene();pendingSkipScript=-1; }
		else if(++pendingSkipTicks>120) pendingSkipScript=-1;
	}
	if (!localActor) {
        if(_game.id==GID_ZAK && !preparatoryFrozen)chatTick();
		if(pendingIntroStart) { pendingIntroStart=false;preparatoryFrozen=false;doSentence(9,403,0); }
		const bool introComplete=_game.id==GID_MANIAC && introAutoHold && !pendingIntroStart && _actors[3]->_room==44 && _userPut>0 && !isScriptRunning(131);
		// The last line outlives script 131. Keep speech running until the
		// checkpoint is serializable; freezing it earlier deadlocks translated
		// speech against appbaseTextCanSave(). Ignore player input while draining.
		if(introComplete && !_haveMsg && canSaveGameStateCurrently()) preparatoryFrozen=true;
		if(preparatoryFrozen || introComplete){clearClickedStatus();_keyPressed.reset();_mouseAndKeyboardStat=0;}
		return preparatoryFrozen;
	}
	applying=true;
	for(uint i=0;i<incoming.size();++i) {
		const int *v=incoming[i].v;
        if(v[0]==21 && v[1]==10001 && v[3]>=1 && v[3]<=7)appbaseManualOwner(v[3]);
        if(v[0]==15 && v[1]>=8 && v[1]<_numActors && v[2]>=0 && v[2]<54){
            Actor *actor=_actors[v[1]];actor->stopActorMoving();
            if(actor->_costume!=v[7])actor->setActorCostume(v[7]);
            actor->putActor(v[3],v[4],v[2]);actor->setDirection(v[5]);
            if(actor->_costume)actor->startAnimActor(v[6]);
        }
        if(v[0]==16 && (v[1]==73 || v[1]==86 || v[1]==87 || v[1]==88 || v[1]==89 || v[1]==90 || v[1]==91 || v[1]==94 || v[1]==97 || v[1]==98 || v[1]==99 || v[1]==101 || v[1]==109 || v[1]==110 || v[1]==113 || v[1]==115 || v[1]==116 || v[1]==118 || v[1]==119 || v[1]==125 || v[1]==126 || v[1]==130 || v[1]==138 || v[1]==143 || v[1]==144 || v[1]==157 || v[1]==161 || v[1]==162 || v[1]==164 || v[1]==167 || v[1]==169 || v[1]==175))_scummVars[v[1]]=static_cast<int16>(v[3]);
        if(v[0]==16 && (v[1]==174 || v[1]==193) && _scummVars[v[1]]!=v[3]) {
            _scummVars[v[1]]=static_cast<int16>(v[3]);refreshPlant=true;
        }
        if(v[0]==17){
            // These original preludes execute before the synchronized cutscene.
            // Stop their timers/chases in the second context as well.
            if(v[1]==151)stopScript(91);
            if(v[1]==88 || v[1]==89){
                // Original script 91 chains into Ed's capture after stopping
                // his trip scripts. The peer VM must perform that same prelude:
                // its still-running search can otherwise overwrite victim 113
                // with 255 when the visitor leaves the hallway.
                stopScript(91);stopScript(87);stopScript(98);stopScript(97);stopScript(151);
                if(v[1]==89)stopScript(88);
            }
            // Original 126 sets mode 108=3 before its cutscene. Preserve that
            // prelude on both VMs so loadRoom(30) cannot start a second 125.
            if(v[1]==126){_scummVars[108]=3;stopScript(125);}
            if(v[1]==37){stopScript(152);stopScript(153);stopScript(36);stopScript(39);stopScript(40);}
            stopScript(v[1]);
            const int slot=getScriptSlot();ScriptSlot &script=vm.slot[slot];
            script.number=v[1];script.offs=v[2];script.status=ssRunning;script.where=WIO_GLOBAL;
            script.freezeResistant=false;script.recursive=false;script.freezeCount=0;script.delayFrameCount=0;script.cycle=1;
            initializeLocals(slot,nullptr);
            sceneStage=1;sceneScript=v[1];sceneActor=v[3];sceneReleased=false;frozen=true;
            VAR(VAR_EGO)=sceneActor;
            // Original v2 actorTalk deliberately omits speech by actors outside
            // the visible room. Both contexts must see the triggering kid's room
            // before the first line (e.g. Edna's "I got you!"). Otherwise the
            // second VM skips its waitForMessage and reaches the dungeon early.
            stopTalk();actorFollowCamera(sceneActor);
            if(v[1]==37){stopScript(152);stopScript(153);stopScript(36);stopScript(39);stopScript(40);}
        }
		if(v[0] == 11 && v[1]>=1 && v[1]<=7 && v[3]>=1 && v[3]<=7 && v[1]!=v[3]) conflictSpeech.push_back(incoming[i]);
		if(v[0] == 10 && v[1] == 1) {
			mansionEntrySent=true;
			if(_scummVars[138] == 0) {
				// Original Amiga v2 front-door script's once-only house-entry effects.
				// A second player entering later must not respawn Edna/reset her timer.
				_scummVars[138]=1;
				_actors[10]->putActor(62,56,7);_actors[10]->animateActor(251);
				putState(89,getState(89)|kObjectStateIntrinsic);
				runScript(148,0,0,nullptr);runScript(152,0,0,nullptr);
			}
		}
		if(v[0] == 9 && v[1]>=1 && v[1]<=(_game.id==GID_ZAK?4:7) && v[7]>=1 && v[7]<=(_game.id==GID_ZAK?4:7) && v[1]!=v[7]) {
			bool changed=localActor!=v[1];
			if(changed) _actors[localActor]->stopActorMoving();
			if(remoteActor!=v[7]) _actors[remoteActor]->stopActorMoving();
			localActor=v[1]; remoteActor=v[7]; predicting=false;
			if(changed) {
				Actor *a=_actors[localActor];a->stopActorMoving();a->putActor(v[3],v[4],v[2]);a->setDirection(v[5]);
				VAR(VAR_EGO)=localActor;if(_game.id==GID_ZAK)runScript(19,0,0,nullptr);actorFollowCamera(localActor);
				lastRoom=-1;lastMoving=0;
			}
			redrawVerbs();
		}
        if(v[0] == 7 && v[1]>0 && v[1]<_numGlobalObjects && (v[3]==0 || v[3]==14 || v[3]==OF_OWNER_ROOM)) {
            if(getOwner(v[1])!=v[3]) setOwnerOf(v[1],v[3]);
            runInventoryScript(1);_fullRedraw=true;
        }
        if(v[0] == 14 && v[1]>=1 && v[1]<=255 && v[2]==_currentRoom && _actors[localActor]->_room==_currentRoom) {
            if(v[3]==1) { if(!_sound->isSoundRunning(v[1]))_sound->addSoundToQueue(v[1]); }
            else if(v[3]==0)_sound->stopSound(v[1]);
        }
		if(v[0] == 5) { // initialize/fix local player; each engine keeps its room context
			VAR(VAR_EGO)=localActor;
            if(_game.id==GID_ZAK)runScript(19,0,0,nullptr);
			actorFollowCamera(localActor);
		}
		if(v[0] == 1 && v[1] == remoteActor && v[2] > 0 && v[2] < (_game.id==GID_ZAK?256:54)) {
			Actor *a=_actors[remoteActor];
			bool animate=predicting && remoteMove.v[2]==v[2] && v[2]==_currentRoom;
			if (!animate || a->_room!=v[2] || ABS(a->getRealPos().x-v[3])>4 || ABS(a->getRealPos().y-v[4])>8) {
				a->stopActorMoving();
				a->putActor(v[3],v[4],v[2]);
				a->setDirection(v[5]);
				if(v[6] >= 0 && v[6] <= 255 && a->_costume) a->startAnimActor(v[6]);
				if(animate) a->startWalkActor(remoteMove.v[3],remoteMove.v[4],remoteMove.v[5]==360?-1:remoteMove.v[5]);
			}
		}
		if(v[0] == 4 && v[1] == remoteActor && v[2] > 0 && v[2] < (_game.id==GID_ZAK?256:54)) {
			remoteMove=incoming[i]; predicting=true;
			Actor *a=_actors[remoteActor];
			// Walkboxes belong to the visible room. Other rooms use checkpoints.
			if(a->_room==v[2] && _currentRoom==v[2]) a->startWalkActor(v[3],v[4],v[5]==360?-1:v[5]);
		}
		if(v[0] == 8 && v[1]>0 && v[1]<(_game.id==GID_ZAK?256:54) && v[2]>=0 && v[2]<=255) {
			lightKnown[v[1]]=true; roomLight[v[1]]=v[2];
			if(_currentRoom==v[1]) { VAR(VAR_CURRENT_LIGHTS)=v[2]; _fullRedraw=true; }
		}
		if((v[0] == 2 || v[0] == 3) && v[1] > 0 && v[1] < _numGlobalObjects) {
			int obj=v[1];
			if(v[0] == 2 && (v[3] == localActor || v[3] == remoteActor ||
                (_game.id==GID_ZAK && (v[3]==5 || v[3]==6)))) {
                if(obj<1024)pendingPickup[obj]=false;
				bool present=false;
				for(int j=0;j<_numInventory;++j)if(_inventory[j] == obj)present=true;
				// For old bundles verify the object exists before using the normal importer.
				if(!present && v[2] > 0 && v[2] < (_game.id==GID_ZAK?256:54)) {
					const int resourceRoom=_game.id==GID_ZAK && v[2]>127?_resourceMapper[v[2]&127]:v[2];
                    const byte *room=getResourceAddress(rtRoom,resourceRoom);
					bool found=false;
					if(room) for(int j=0;j<room[20];++j) {
						uint offset=READ_LE_UINT16(room+28+2*room[20]+2*j);
						if(READ_LE_UINT16(room+offset+4) == obj)found=true;
					}
					if(found) { addObjectToInventory(obj,resourceRoom);present=true; }
				}
				if(present) {
					_objectOwnerTable[obj]=v[3];
					_objectStateTable[obj]|=kObjectStateIntrinsic|kObjectStateUntouchable;
					runInventoryScript(1);
				}
			} else if(v[0] == 3) {
				_objectStateTable[obj]=v[3];
                if(_game.id==GID_MANIAC && obj>=248 && obj<=251)refreshArcade=true;
			}
			markObjectRectAsDirty(obj);
			clearDrawObjectQueue();
			_fullRedraw=true;
		}
	}
	incoming.clear();
    // The shared graphic is authoritative, but independent VMs can choose
    // different digits in script 102. Reuse that unmodified original script
    // with its accepted graphic choice, never hard-code a code or open a door.
    // This also repairs legacy paired saves before a kid enters the keypad.
    if(refreshArcade && !isScriptRunning(102)) {
        int choice=-1,count=0;
        for(int id=248;id<=251;++id)if(getState(id)&kObjectStateIntrinsic){choice=id-248;++count;}
        if(count==1) {
            refreshArcade=false;arcadeReplay=choice;
            const uint32 seed=_rnd.getSeed();const int16 scratch=_scummVars[111];
            runScript(102,0,0,nullptr);
            _scummVars[111]=scratch;_rnd.setSeed(seed);arcadeReplay=-1;
        }
    }
	applying=false;
    // Reuse the original renderer/walkbox rules after a canonical update.
    // Merely copying actor 19's appearance leaves climbing blocked.
    if(refreshPlant && _currentRoom==27 && sceneStage==0) {
        refreshPlant=false;runScript(54,0,0,nullptr);
    }
	++ticks;
	Actor *peer=_actors[remoteActor];
	if(predicting && peer->_room==remoteMove.v[2] && !peer->_moving && peer->getRealPos().x==remoteMove.v[3] && peer->getRealPos().y==remoteMove.v[4]) predicting=false;
	if(!frozen) {
        chatTick();
		Actor *a=_actors[localActor];
		if(!conflictSpeech.empty() && !_haveMsg && !vm.cutSceneStackPointer && _userPut>0) {
			const int *v=conflictSpeech[0].v;
			if(conflictStage==2 || (_currentRoom!=v[2] && a->_room!=v[2])) { conflictSpeech.remove_at(0);conflictStage=0; }
			else if(_currentRoom==v[2]) {
				int speaker=conflictStage==0?v[1]:v[3];
				if(_actors[speaker]->_room==_currentRoom) {
					_actorToPrintStrFor=speaker;
					actorTalk(reinterpret_cast<const byte *>(conflictStage==0?"Sorry.":"Never mind."));
					++conflictStage;
				} else { conflictSpeech.remove_at(0);conflictStage=0; }
			}
		}
        if(_game.id==GID_MANIAC && !fridgeHungerSent && a->_room==7 && _scummVars[144]==1 && _scummVars[143]==2){
            emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
            emit(18,1,a->_room,localActor);fridgeHungerSent=true;
        }
		if(_game.id==GID_MANIAC && !mansionEntrySent && _scummVars[138]!=0 && a->_room==10) {
			emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
			emit(10,1,a->_room,localActor);mansionEntrySent=true;
		}
		if(ticks % 15 == 0 || a->_room!=lastRoom || (!a->_moving && lastMoving))
			emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
		lastRoom=a->_room; lastMoving=a->_moving;
	}
	if(frozen){clearClickedStatus();_keyPressed.reset();_mouseAndKeyboardStat=0;}
	return frozen;
}
const char *ScummEngine::appbaseCoopName(int id) {
    if(id<=0 || id>=_numActors) return "";
    const byte *name=_actors[id]->getActorName();
    return name ? reinterpret_cast<const char *>(name) : "";
}
extern "C" {
// Narrow string bridge: ordered printable bytes, no pointers or escape codes.
EMSCRIPTEN_KEEPALIVE int mm_shared_mode(int role,int acknowledgement) {
    if(!g_scumm || g_scumm->_game.id!=GID_ZAK || role<0 || role>2 || acknowledgement<0)return 0;
    if(role!=peerRole){peerRole=role;peerFrame=peerAck=peerFailed=0;peerInFrame=false;peerQueue.clear();peerChat.clear();peerRecord.clear();chatSpeech.clear();}
    peerAck=MAX(peerAck,acknowledgement);return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_shared_reset() {
    peerRole=peerFrame=peerAck=peerFailed=0;peerInFrame=false;peerQueue.clear();peerChat.clear();peerRecord.clear();chatSpeech.clear();return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_shared_receive(int kind,int frame,int a,int b,int c,int d,int e,int f) {
    if(peerRole!=2 || peerQueue.size()>=4096 || kind<23 || kind>31 || frame<=peerFrame)return 0;
    CoopEvent event={{kind,frame,a,b,c,d,e,f}};peerQueue.push_back(event);return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_chat_byte(int index,int value) {
    if(index==0)chatLength=0;
    if(index!=chatLength || index<0 || index>=160 || value<32 || value>126)return 0;
    chatText[chatLength++]=value;chatText[chatLength]=0;return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_chat_say(int actor,int room,int length) {
    if(!g_scumm || (!localActor?(g_scumm->_game.id!=GID_ZAK || actor!=g_scumm->appbaseCoopGet(0,0,1)):(actor!=localActor && actor!=remoteActor)) || g_scumm->appbaseCoopGet(0,0,28)!=1 || room<1 || room>(g_scumm->_game.id==GID_ZAK?255:53) || length!=chatLength || length<1 || length>160 || chatSpeech.size()>=8)return 0;
    if(peerRole==1){
        const int sequence=EM_ASM_INT({return Module.mmChatSequence || 0;});
        if(sequence<=0)return 0;
        CoopEvent record={{26,0,sequence,0,0,0,0,0}};peerChat.push_back(record);
    }
    ChatSpeech line;line.actor=actor;line.room=room;line.age=0;memcpy(line.text,chatText,length+1);chatSpeech.push_back(line);chatLength=0;chatDeliveryStatus=1;return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_coop_scene(int operation,int script,int offset,int actor) {
    return g_scumm?g_scumm->appbaseCoopSceneCommand(operation,script,offset,actor):0;
}
EMSCRIPTEN_KEEPALIVE int mm_manual_command(int operation) {
    return g_scumm && (operation==0 || operation==1)?g_scumm->appbaseManual(operation):0;
}
EMSCRIPTEN_KEEPALIVE int scumm_item_version() { return 1; }
EMSCRIPTEN_KEEPALIVE int scumm_item_owner(int id,int owner) { return !g_scumm?0:(owner<0?g_scumm->appbaseItem(2,id):g_scumm->appbaseItem(3,id,owner)); }
EMSCRIPTEN_KEEPALIVE int scumm_item_id(int index) { return g_scumm?g_scumm->appbaseItem(5,index):0; }
EMSCRIPTEN_KEEPALIVE int mm_coop_version() { return 8; }
EMSCRIPTEN_KEEPALIVE int scumm_game_id() { return !g_scumm?0:(g_scumm->_game.id==GID_MANIAC?1:(g_scumm->_game.id==GID_ZAK?2:0)); }
EMSCRIPTEN_KEEPALIVE int mm_coop_prepare(int operation) { if(!g_scumm && operation==1){preparatoryFrozen=true;return 1;}return g_scumm?g_scumm->appbaseCoopPrepare(operation):0; }
EMSCRIPTEN_KEEPALIVE void mm_coop_seed(unsigned int seed) { if(g_scumm) g_scumm->appbaseCoopSeed(seed); }
EMSCRIPTEN_KEEPALIVE int mm_coop_skip(int script) {
	if(!g_scumm || g_scumm->_game.id!=GID_MANIAC || g_scumm->_game.version!=2 || script<0 || script>178) return 0;
	pendingSkipScript=script;pendingSkipTicks=0;return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_coop_get(int kind,int id,int field) {
	return g_scumm ? g_scumm->appbaseCoopGet(kind,id,field) : -1;
}
EMSCRIPTEN_KEEPALIVE const char *mm_coop_actor_name(int id) {
    return g_scumm ? g_scumm->appbaseCoopName(id) : "";
}
EMSCRIPTEN_KEEPALIVE int mm_coop_enable(int actor,int peer) {
	if(!g_scumm || (g_scumm->_game.id != GID_MANIAC && g_scumm->_game.id != GID_ZAK) || g_scumm->_game.version != 2 || actor<=0 || peer<=0 || (g_scumm->_game.id==GID_ZAK && (actor>4 || peer>4 || g_scumm->appbaseCoopGet(8,actor,0)!=1 || g_scumm->appbaseCoopGet(8,peer,0)!=1)) || actor==peer || actor>=g_scumm->appbaseCoopGet(0,0,3) || peer>=g_scumm->appbaseCoopGet(0,0,3) || g_scumm->appbaseCoopGet(1,actor,0)<=0) return 0;
	memset(pendingPickup,0,sizeof(pendingPickup));
	selectionInputSlot=-1;localActor=actor; remoteActor=peer; frozen=true; predicting=false; lastRoom=-1; lastMoving=0;
	mansionEntrySent=g_scumm->appbaseCoopGet(4,138,0)!=0;fridgeHungerSent=g_scumm->appbaseCoopGet(4,144,0)!=0;
	conflictSpeech.clear();conflictStage=0;chatSpeech.clear();chatLength=0;
	preparatoryFrozen=false;pendingIntroStart=false;introAutoHold=false;pendingSkipScript=-1;
	CoopEvent e={{5,0,0,0,0,0,0,0}};incoming.push_back(e);
    refreshPlant=false;refreshArcade=g_scumm->_game.id==GID_MANIAC;arcadeReplay=-1;
    // A legacy shared save may predate canonical plant state. Publish the
    // actual restored globals once; the server merges both seats monotonically.
    const int room=g_scumm->appbaseCoopGet(1,actor,0);
    if(g_scumm->_game.id==GID_MANIAC){
    const int growth=g_scumm->appbaseCoopGet(4,174,0);
    emit(20,174,room,growth==255?255:MIN(growth,2),actor,1,0);
    emit(20,193,room,g_scumm->appbaseCoopGet(4,193,0)?1:0,actor,1,0);
    }
	return 1;
}
EMSCRIPTEN_KEEPALIVE void mm_coop_freeze(int value) { frozen=value!=0;if(!localActor)preparatoryFrozen=value!=0; }
EMSCRIPTEN_KEEPALIVE int mm_coop_receive(int kind,int id,int room,int a,int b,int c,int d,int e) {
    if(kind==21 && (id!=10001 || room!=0 || a<1 || a>7 || b || c || d || e))return 0;
	if(g_scumm && g_scumm->_game.id==GID_ZAK && (kind==10 || kind==11 || kind==15 || kind==16 || kind==21))return 0;
	if(!localActor || incoming.size()>=256 || ((kind<1 || kind>4) && kind!=7 && kind!=8 && kind!=9 && kind!=10 && kind!=11 && kind!=14 && kind!=15 && kind!=16 && kind!=21)) return 0;
	CoopEvent event={{kind,id,room,a,b,c,d,e}};incoming.push_back(event);return 1;
}
}
} // namespace Scumm
#endif
