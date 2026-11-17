/* ScummVM AppBase local co-op experiment. GPL-3.0-or-later.
 * Two independent room/script contexts; only the explicitly bridged state
 * is shared. This is not a claim of whole-game script determinism. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "scumm/appbase-party-profile.h"
#include "common/array.h"
#include "common/serializer.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v2.h"
#include "scumm/actor.h"
#include "scumm/object.h"
#include "scumm/resource.h"
#include "scumm/sound.h"
#include "scumm/music.h"
#include "scumm/verbs.h"

namespace Scumm {
struct CoopEvent { int v[8]; };
static Common::Array<CoopEvent> incoming;
static int localActor = 0, remoteActor = 0, ticks = 0;
// 0: idle, 1: waiting for canonical grant, 2: rejected (skip resumed opcode).
static byte pendingPickup[1024] = {};
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
static int lastRoom = -1, lastMoving = 0, lastExtensionFlags = -1;
static bool lightKnown[256] = {};
static int roomLight[256] = {};
static bool mansionEntrySent = false,fridgeHungerSent=false,zakVictorySent=false;
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
static int peerRole=0,peerFrame=0,peerAck=0,peerFailed=0,peerMusic=0,peerTextSpeed=0,peerLeadLimit=60;
static bool peerInFrame=false;
// A passive seat may request only visible native character-menu verbs. The
// controller executes their original input script in its normal replay frame;
// neither a second menu renderer nor a second character-ID mapping is needed.
static int partyMenuQueued=0,partyMenuPending=0,partyMenuAge=0;
static int partyHoverX=-1,partyHoverY=-1;
static bool partyMenuLocked=false,partyMenuLocalAllowed=true;
static void partyMenuDone(int actor) {
    partyMenuQueued=partyMenuPending=partyMenuAge=0;
    EM_ASM({if(Module.mmPartyMenuDone)Module.mmPartyMenuDone($0);},actor);
}
int ScummEngine::appbasePartyMenu(int operation,int a,int b) {
    // Local presentation coordinates never enter the replicated mouse state.
    if(operation==3){partyHoverX=a;partyHoverY=b;return 1;}
    if(operation==5){partyMenuLocked=a!=0;partyMenuLocalAllowed=b!=0;return 1;}
    if(operation==4){
        if(!peerRole || _appbaseExtensions.info(11,0)<=0)return -1;
        if(partyMenuLocked && !partyMenuLocalAllowed)return 0;
        if(peerRole!=2)return -1;
        const int verb=appbasePartyMenu(0,partyHoverX,partyHoverY);
        return verb?getVerbSlot(verb,0):0;
    }
    if(operation==2) {
        if(!_setupIsComplete || _appbaseExtensions.info(11,0)<=0)return 0;
        redrawVerbs();return 1; // Presentation only; no original script writes.
    }
    if(_game.id!=GID_ZAK || _game.version!=2 || !_setupIsComplete ||
       (!peerRole && !localActor) || _appbaseExtensions.info(11,0)<=0 || preparatoryFrozen || frozen ||
       peerFailed || appbaseCoopGet(0,0,28)!=1)return 0;
    int verb=a;
    if(operation==0) {
        if(a<0 || a>=_screenWidth || b<0 || b>=_screenHeight)return 0;
        const int slot=findVerbAtPos(a,b);if(!slot)return 0;verb=_verbs[slot].verbid;
    } else if(operation==1) {
        if(localActor || peerRole!=1 || partyMenuQueued || partyMenuPending)return 0;
    } else return 0;
    // Zak V2 reserves 45 for SWITCH and the following verbs for its character
    // menu. Only an actually visible verb may be dispatched, including mods.
    if(verb<45 || verb>59 || !appbaseCoopVerb(verb))return 0;
    const int slot=getVerbSlot(verb,0);
    if(!slot || _verbs[slot].curmode!=1 || _verbs[slot].saveid ||
       _verbs[slot].curRect.top<0 || _verbs[slot].curRect.top>=_screenHeight)return 0;
    if(operation==0)return verb;
    partyMenuQueued=verb;return 1;
}
static Common::Array<CoopEvent> peerQueue,peerRecord,peerInputs,peerSounds,peerRandom,peerChat;
static CoopEvent peerEnd,peerInput;
static int peerKeyFlags=0;
// Independent party leases need the same exact VM restore as replicated peers.
// Entering the original save room would run room-exit scripts during a handoff.
bool appbasePeerPreservesContext() { return peerRole!=0 || localActor!=0 || preparatoryFrozen; }
bool appbaseCoopRunsOriginalScript(int script) {
    // Original scripts 9/84 sample and consume the four heroes' air; 82/146
    // apply damage and warnings. Their clothing globals belong to the original
    // player's VM, not the independent extension player's room context.
    // Keep their slots intact so leaving co-op can resume ordinary simulation.
    const bool extensionReplica=g_scumm && g_scumm->_game.id==GID_ZAK &&
        g_scumm->_game.version==2 && localActor>0 && peerRole==0 && sceneStage==0 &&
        g_scumm->appbasePartyActor(localActor) && !appbasePartyOriginalActor(true,localActor) &&
        appbasePartyOriginalActor(true,remoteActor);
    return !extensionReplica || (script!=9 && script!=82 && script!=84 && script!=146);
}
static void peerRecordEvent(int kind,int a=0,int b=0,int c=0,int d=0,int e=0,int f=0) {
    CoopEvent record={{kind,peerFrame,a,b,c,d,e,f}};peerRecord.push_back(record);
}
int ScummEngine::appbasePeerWait(int quarterFrames) const {
    if(peerRole!=2 || preparatoryFrozen || frozen || peerFailed)return quarterFrames;
    // The controller already paced these original frames. Waiting for their
    // timers a second time leaves every network/browser stall permanently queued.
    for(uint i=0;i<peerQueue.size();++i)if(peerQueue[i].v[0]==27)return 0;
    return MIN(quarterFrames,4);
}
bool ScummEngine::appbasePeerTick(int &delta) {
    if(!peerRole)return false;
    if(preparatoryFrozen || frozen || peerFailed)return true;
    if(peerRole==1 && peerFrame-peerAck>=peerLeadLimit)return true;
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
            if(r.v[0]==28 && pendingSkipScript<0){pendingSkipScript=r.v[2];pendingSkipTicks=0;}
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
        if(_game.id==GID_ZAK && sceneStage>0 && pendingSkipScript>=0)peerRecordEvent(28,pendingSkipScript);
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
void ScummEngine::appbasePeerMusic() { if(VAR_MUSIC_TIMER==0xff)return;if(peerRole && peerInFrame)VAR(VAR_MUSIC_TIMER)=peerMusic;VAR(VAR_MUSIC_TIMER)=appbaseSpectatorValue(2,0,VAR(VAR_MUSIC_TIMER)); }
void ScummEngine::appbasePeerInput(int area,int value,int mode) { if(peerRole==1 && peerInFrame)peerRecordEvent(25,area,value,mode); }
bool ScummEngine::appbasePeerInputs() {
    if(peerRole==1 && peerInFrame && (partyMenuQueued || partyMenuPending)) {
        if(partyMenuQueued) {
            const int verb=partyMenuQueued;partyMenuQueued=0;
            // Readiness can change between the browser command and this tick.
            if(appbasePartyMenu(1,verb,0)!=1){partyMenuDone(0);return true;}
            partyMenuQueued=0;partyMenuPending=verb;partyMenuAge=0;
            runInputScript(kVerbClickArea,verb,1);
        }
        return true; // Do not mix controller input with another seat's request.
    }
    if(peerRole==1 && peerInFrame && partyMenuLocked)return true;
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
    if(peerRole==1 && partyMenuPending &&
       (++partyMenuAge>60 || (partyMenuPending==45 && partyMenuAge>=2)))partyMenuDone(0);
    // Check logical progress, not language-specific pixels or audio buffers.
    uint32 hash=2166136261u;
    const auto add=[&](int v){hash=(hash^uint32(v))*16777619u;};
    add(_currentRoom);add(VAR(VAR_EGO));add(_userPut);
    if(_game.id==GID_ZAK && sceneScript==126 && sceneStage>=1) {
        // Independent actors keep their own room scripts and inventories. The
        // same native replay protocol checks the common story execution only.
        add(sceneStage);add(sceneScript);add(_scummVars[96]);
        for(int id : {7,12}){Actor *a=_actors[id];add(a->_room);add(a->getRealPos().x);add(a->getRealPos().y);add(a->_frame);add(a->_costume);}
        for(int id=0;id<NUM_SCRIPT_SLOT;++id)if(vm.slot[id].number==126 && vm.slot[id].status!=ssDead){add(vm.slot[id].status);add(vm.slot[id].delay);add(vm.slot[id].offs);}
        for(int id : {155,328,329})add(getState(id));
    } else {
    for(int id=0;id<_numVariables;++id)if(id!=VAR_HAVE_MSG && id!=VAR_CHARCOUNT)add(_scummVars[id]);
    for(int id=0;id<NUM_SCRIPT_SLOT;++id){add(vm.slot[id].number);add(vm.slot[id].status);add(vm.slot[id].delay);}
    for(int id=1;id<_numGlobalObjects;++id){add(getOwner(id));add(getState(id));}
    for(int id=1;id<_numActors;++id){add(_actors[id]->_room);add(_actors[id]->getRealPos().x);add(_actors[id]->getRealPos().y);}
    if (_appbaseExtensions.info(12, 0) > 0) add(_appbaseExtensions.stateHash());
    }
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
// Native semantic notification: the first narrator print in original Zak149
// is the real congratulations, in both vetted English and German datasets.
// Keep the original epilogue running; no script, timer or hero is replaced.
void ScummEngine::appbaseCoopPrint() {
    if(_game.id!=GID_ZAK || _game.version!=2 || !localActor || applying ||
       zakVictorySent || peerRole || sceneStage || _currentScript==0xff ||
       vm.slot[_currentScript].where!=WIO_GLOBAL || vm.slot[_currentScript].number!=149 ||
       _actorToPrintStrFor!=255 || _currentRoom!=25 ||
       (localActor!=1 && localActor!=2) || _actors[1]->_room!=25 || _actors[2]->_room!=25)return;
    Actor *a=_actors[localActor];
    emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
    emit(38,149,25,localActor);
    zakVictorySent=true;
}
bool ScummEngine::appbaseRequestEncounter(unsigned index, unsigned room, unsigned actor, unsigned other, bool send) {
    if (!localActor) return false;
    if (send && !frozen && !preparatoryFrozen && !applying && (unsigned(localActor) == actor || unsigned(localActor) == other))
        emit(32, index + 1, room, actor, other);
    return true;
}
bool ScummEngine::appbaseRequestObjectReaction(unsigned index, unsigned room, unsigned actor, unsigned object, unsigned verb, unsigned before, unsigned after) {
	if (!localActor) return false;
	if (!frozen && !preparatoryFrozen && !applying && unsigned(localActor) == actor)
		emit(33, index + 1, room, actor, object, verb, before, after);
	return true; // Incoming canonical writes can never originate another request.
}
int ScummEngine::appbaseCoopRandom(int maximum,int value) {
    if(peerRole && peerInFrame) {
        if(peerRole==1){peerRecordEvent(31,maximum,value);return appbaseSpectatorValue(3,maximum,value);}
        if(peerRandom.empty() || peerRandom[0].v[2]!=maximum){peerFailed=4;return appbaseSpectatorValue(3,maximum,value);}
        const int result=peerRandom[0].v[3];peerRandom.remove_at(0);return appbaseSpectatorValue(3,maximum,result);
    }
    // Replay only the already accepted original high-score graphic. Other
    // random calls, including the first real choice, retain their normal RNG.
    return appbaseSpectatorValue(3,maximum, applying && arcadeReplay>=0 && maximum==3 && _currentScript!=0xff &&
        vm.slot[_currentScript].where==WIO_GLOBAL && vm.slot[_currentScript].number==102
        ? arcadeReplay : value);
}
bool ScummEngine::appbasePartyActor(int id) const {
    return appbasePartyOriginalActor(_game.id==GID_ZAK,id) || _appbaseExtensions.partyActor(id);
}
bool ScummEngine::appbasePartyActorAvailable(int id) {
    if(!appbasePartyActor(id))return false;
    if(appbaseCoopGet(8,id,0)==1)return true;
    // The original shared actor already belongs to both seats before the
    // other original heroes unlock. Only explicit extension parties may
    // split that actor into an independent seat at this stage.
    return _game.id==GID_ZAK && id==1 && _appbaseExtensions.info(11,0)>0 &&
        appbaseCoopGet(0,0,28)==1 && appbaseCoopGet(1,id,0)>0 && appbaseCoopGet(1,id,5)>0;
}
bool ScummEngine::appbaseCoopActive() const { return localActor != 0; }
int ScummEngine::appbaseCoopActor() const { return localActor; }
int ScummEngine::appbaseManiacPartySelect(int actor) {
    // Additional figures have independent byte-sized inventory owners. Keep
    // Dave and the two original selected kids, including their menu variables.
    if (_game.id != GID_MANIAC || _game.version != 2 || !_setupIsComplete ||
        peerRole || (localActor && (frozen || applying || sceneStage || VAR(VAR_EGO)!=localActor)) || isPaused() || _appbaseExtensions.info(11, 0) == 0 ||
        _currentRoom <= 0 || _currentRoom == 45 || _userPut <= 0 ||
        vm.cutSceneStackPointer || !appbaseTextCanSave()) return 0;
    bool selected = false, dave = false;
    for (int i = 47; i <= 49; ++i) {
        const int kid = _scummVars[i];
        if (kid < 1 || kid > 7) return 0;
        for (int j = 47; j < i; ++j) if (_scummVars[j] == kid) return 0;
        selected |= actor == kid;
        dave |= kid == 3;
    }
    if (!dave) return 0;
    const int previous = VAR(VAR_EGO);
    if (previous <= 0 || previous >= _numActors || _actors[previous]->_room != _currentRoom) return 0;
    const unsigned held = _appbaseExtensions.maniacActorBit(2952, previous);
    if (_scummVars[held >> 4] & (1 << (held & 15))) return 0;
    if (actor == -1) return 1; // Read-only availability query for the host UI.
    if ((!selected && !_appbaseExtensions.partyActor(actor)) || actor <= 0 ||
        actor >= _numActors || _actors[actor]->_room <= 0 || _actors[actor]->_costume <= 0) return 0;
    const unsigned targetHeld = _appbaseExtensions.maniacActorBit(2952, actor);
    if (_scummVars[targetHeld >> 4] & (1 << (targetHeld & 15))) return 0;
    if (localActor && (actor == remoteActor || !EM_ASM_INT({return Module.mmPartySelectable ? (Module.mmPartySelectable($0)?1:0) : 1;},actor))) return 0;
    if (previous == actor) return 1;
    if (localActor) {
        _actors[previous]->stopActorMoving();Actor *a=_actors[previous];
        emit(1,previous,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
        emit(6,actor,0);return 1; // The authenticated MM session grants the lease.
    }
    _actors[previous]->stopActorMoving();
    _sentenceNum = 0;
    stopScript(2); // Enhanced V2's original sentence script.
    VAR(VAR_EGO) = actor;
    actorFollowCamera(actor);
    runScript(164, 0, 0, nullptr); // Restore original action verbs after New kid.
    runInventoryScript(1);
    // A placed figure can first appear in a room without using its entry
    // door. Start the same original guards used for ordinary room arrivals;
    // let their original pursuit and capture scripts decide what happens.
    if (_actors[10]->_room == _currentRoom && !isScriptRunning(36) && !isScriptRunning(153))
        runScript(_currentRoom == 7 ? 153 : 36, 0, 0, nullptr);
    if (_actors[11]->_room == _currentRoom && !isScriptRunning(91) && !isScriptRunning(88) && !isScriptRunning(89))
        runScript(91, 0, 0, nullptr);
    _fullRedraw = true;
    return 1;
}
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
    if (_game.id==GID_ZAK && verb>=46 && verb<=59) {
        const int actor=_appbaseExtensions.partyVerbActor(verb);
        if(actor==0)return false;
        if(actor>0) {
            // drawVerb, keyboard dispatch and mouse hit testing already share
            // this predicate. Reuse native unlocks and the server's seat plan.
            if(!const_cast<ScummEngine *>(this)->appbasePartyActorAvailable(actor))return false;
            return EM_ASM_INT({return Module.mmPartySelectable ? (Module.mmPartySelectable($0)?1:0) : 1;},actor)!=0;
        }
    }
	return _game.id!=GID_MANIAC || !localActor || verb<20 || verb>22 || _scummVars[47+verb-20]!=remoteActor;
}
int ScummEngine::appbaseCoopSelect(int actor) {
    // During replicated input both engines retain the current EGO. Only the
    // controller publishes the selection intent to the common seat authority.
    if(!localActor && peerRole && _appbaseExtensions.info(11,0)>0 && _userPut>0 &&
       _currentScript!=0xff && vm.slot[_currentScript].where==WIO_GLOBAL && vm.slot[_currentScript].number==2 &&
       VAR(VAR_ACTIVE_VERB)==45 && VAR(VAR_ACTIVE_OBJECT2)==actor) {
        // An original menu can display heroes before the party unlocks them.
        // Their EGO write must not bypass the authenticated roster/lease.
        if(!appbasePartyActor(actor) || !appbasePartyActorAvailable(actor)) {
            if(peerRole==1 && partyMenuPending>45)partyMenuDone(0);
            return VAR(VAR_EGO);
        }
        if(peerRole==1 && partyMenuPending>45)partyMenuDone(actor);
        return VAR(VAR_EGO);
    }
	if (!localActor || sceneStage==2) return actor;
    if(_game.id==GID_ZAK) {
        // Zak's original SWITCH sentence (script 2), distinct from temporary
        // EGO writes used by original movies and animal-control puzzles.
        const bool selection=!applying && _userPut>0 && _currentScript!=0xff &&
            vm.slot[_currentScript].where==WIO_GLOBAL && vm.slot[_currentScript].number==2 &&
            VAR(VAR_ACTIVE_VERB)==45 && VAR(VAR_ACTIVE_OBJECT2)==actor;
        if(!selection)return actor;
        if(!appbasePartyActor(actor) || !appbasePartyActorAvailable(actor))
            return _appbaseExtensions.info(11,0)>0?localActor:actor;
        if(actor!=localActor && (actor!=remoteActor || _appbaseExtensions.active())) {
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
	if (actor!=localActor && actor!=remoteActor && appbasePartyActor(actor) &&
		(actor==_scummVars[47] || actor==_scummVars[48] || actor==_scummVars[49])) {
		Actor *a=_actors[localActor];a->stopActorMoving();
		emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
		emit(6,actor,0);
	}
	return localActor; // only the authenticated server grants the new controller
}
int ScummEngine::appbaseCoopPickup(int obj) {
    if (!localActor) return 0;
    if(sceneStage==2 && localActor!=sceneActor)return 0;
    if(obj<=0 || obj>=1024)return 0;
    if(pendingPickup[obj]==2){pendingPickup[obj]=0;return -1;}
    if(pendingPickup[obj]>=3)return 1;
    if(pendingPickup[obj]==1)return sceneStage==2?0:1;
    if(whereIsObject(obj)==WIO_INVENTORY)return -1;
    pendingPickup[obj]=1;
    const int ego=VAR(VAR_EGO);
    const bool animal=_game.id==GID_ZAK && ego>=5 && ego<=6 && _actors[ego]->_room==_currentRoom;
    // The authenticated hero stays behind during the original animal puzzle.
    // Carry the animal's original data room separately from the seat's pose.
    emit(2,obj,_game.id==GID_ZAK?_actors[localActor]->_room:_roomResource,localActor,
         animal?_currentRoom:0,animal?ego:0);
    if(sceneStage==2)return 0;
	return 1; // suspend this opcode until its sequenced response arrives
}
void ScummEngine::appbaseCoopTape(int var,int value) {
    if(_game.id!=GID_MANIAC)return;
    // Hunk-O-Matic29 sets this exact learned bit through setBitVar. Other
    // bits of184 are capture flags and must never travel with learned strength.
    if(localActor>=1 && localActor<=7 && !applying && var==184 && _roomResource==25 && sceneStage==0 &&
       _currentScript!=0xff && vm.slot[_currentScript].where==WIO_GLOBAL &&
       vm.slot[_currentScript].number==29 && (value&(1<<localActor)))
        emit(20,184,_actors[localActor]->_room,1<<localActor,localActor,0,0);
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
int ScummEngine::appbaseCoopTransfer(int object,int owner) {
	if (!localActor || applying || object<=0 || object>=_numGlobalObjects) return false;
    // States 3/4/5 share the existing serialized acknowledgement slots:
    // awaiting/accepted/rejected original setOwner from internal storage.
    if(object<1024 && pendingPickup[object]>=3) {
        if(pendingPickup[object]==3)return 2;
        pendingPickup[object]=0;return 1;
    }
	int from=getOwner(object);
    // Original V2 scripts can store inventory with an NPC, as well as owner14.
    // Playable heroes and the original animal carriers keep their transfer path.
    const auto storedOwner=[&](int value) {
        return value>0 && value<OF_OWNER_ROOM && !appbasePartyActor(value) &&
            !(_game.id==GID_ZAK && (value==5 || value==6));
    };
    const auto removedOwner=[&](int value) {
        return value==0 || value==OF_OWNER_ROOM || storedOwner(value);
    };
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
       removedOwner(owner)){
        pendingPickup[object]=false;consume();return false;
    }
    if(sceneStage==2){
        if(localActor==sceneActor && from==localActor && removedOwner(owner))consume();
        return false;
    }
    // Original animal control disables SWITCH (global199), but still permits
    // giving an item back to the hero standing in the same original room.
    if(animal && from==animal && appbasePartyActor(owner) && _actors[owner]->_room==_currentRoom) {
        Actor *a=_actors[localActor];
        emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
        emit(5,object,a->_room,localActor,owner,animal,_currentRoom);
        return true;
    }
    if(animal && from==animal && removedOwner(owner)) {
        consume();return false;
    }
    // Original V2 stores records, tapes and confiscated items with internal
    // owners or NPCs. Taking them back is an original inventory ownership
    // opcode, not pickupObject; route that through the same canonical grant.
    if(storedOwner(from) && owner==localActor && object<1024){
        pendingPickup[object]=3;
        // Explicit previous owner distinguishes setOwner from pickupObject.
        // It must preserve flags and resource provenance, including records
        // that have an owner but never appear in the visible inventory.
        emit(2,object,_game.id==GID_ZAK?_actors[localActor]->_room:_roomResource,localActor,0,0,from);
        return 2;
    }
	if (from==localActor && (owner==remoteActor || (appbasePartyActor(owner) && owner!=localActor &&
            ((_game.id==GID_ZAK && appbaseCoopGet(8,owner,0)==1) ||
             (_game.id==GID_MANIAC && (owner==_scummVars[47] || owner==_scummVars[48] || owner==_scummVars[49])))))) {
		Actor *a=_actors[localActor];
		emit(1,localActor,a->_room,a->getRealPos().x,a->getRealPos().y,a->getFacing(),a->_frame,a->_costume);
		emit(5,object,a->_room,localActor,owner);
		return true; // only confirmed ownership changes the inventories
	}
    if (from==localActor && removedOwner(owner)) {
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
    if((_game.id!=GID_MANIAC && _game.id!=GID_ZAK) || !localActor || _currentScript==0xff || vm.slot[_currentScript].where!=WIO_GLOBAL) return false;
    const int script=vm.slot[_currentScript].number;
    const int offset=_scriptPointer-_scriptOrgPointer-1;
    // A second original scene may reach its entry in the same VM frame as
    // preparation/completion of the server-approved scene. Yield only that
    // script; never freeze the approved scene or execute an unrelated cutscene
    // inside its shared barrier. It will request its own turn after release.
    // Original police victory calls 63 inside 126. This one vetted child
    // belongs to the same barrier; serializing it behind its endless parent
    // would prevent the real congratulations from ever executing.
    if(_game.id==GID_MANIAC && sceneStage==2 && sceneScript==126 && script==63 && offset==32 && _scummVars[162]==3)return false;
    if(sceneStage!=0 && script!=sceneScript)return true;
    if(sceneStage==3 || (sceneStage==1 && !sceneReleased))return true;
    if(sceneReleased && script==sceneScript){sceneStage=2;return false;}
    if(sceneStage==2) return false;
    const bool known=_game.id==GID_ZAK ? (script==126 && offset==8) : (script==1 && offset==32) || (script==100 && offset==4) || (script==101 && offset==54) || (script==105 && offset==42) || (script==112 && offset==4) || (script==119 && offset==4) || (script==120 && offset==4) || (script==126 && offset==12) || (script==130 && offset==5) || (script==131 && offset==10) || (script==140 && offset==4) || (script==142 && offset==4) || (script==145 && offset==4) || (script==148 && offset==8) || (script==151 && offset==50) || (script==155 && offset==4) || (script==157 && offset==4) || (script==163 && offset==10) || (script==175 && offset==4) || (script==23 && offset==5) || (script==27 && offset==45) || (script==35 && offset==20) || (script==37 && offset==4) || (script==42 && offset==8) || (script==43 && offset==16) || (script==47 && offset==5) || (script==48 && offset==27) || (script==49 && offset==237) || (script==61 && offset==5) || (script==62 && offset==27) || (script==63 && offset==32) || (script==87 && offset==35) || (script==88 && offset==283) || (script==89 && offset==4) || (script==9 && offset==4) || (script==95 && offset==7) || (script==97 && offset==8) || (script==99 && offset==10);
    if(!known) return false;
    frozen=true;
    emit(12,script,offset,script==37?_scummVars[73]:0);
    return true;
}
void ScummEngine::appbaseCoopSceneEnd() {
    if(sceneStage!=2 || _currentScript==0xff || vm.slot[_currentScript].number!=sceneScript)return;
    sceneStage=3;frozen=true;
    // The replicated final frame must keep one common EGO/room until both
    // engines acknowledge completion. Restore each local view after release.
    if(_game.id!=GID_ZAK || !peerRole){VAR(VAR_EGO)=localActor;actorFollowCamera(localActor);}
}
int ScummEngine::appbaseCoopSceneCommand(int operation,int script,int offset,int actor) {
    if(!localActor)return 0;
    if(operation==0)return sceneStage;
    if(operation==1){
        if(script<1 || script>=_numGlobalScripts || offset<4 || offset>65535 || actor<1 || !appbasePartyActor(actor))return 0;
        if(_game.id==GID_ZAK && (script!=126 || offset!=8))return 0;
        const byte *code=getResourceAddress(rtScript,script);
        if(!code || offset>=getResourceSize(rtScript,script) || code[offset]!=0x40)return 0;
        CoopEvent event={{17,script,offset,actor,0,0,0,0}};incoming.push_back(event);return 1;
    }
    if(operation==2 && (sceneStage==1 || sceneStage==2)){sceneReleased=true;frozen=false;return 1;}
    if(operation==3 && sceneStage==3){sceneStage=0;sceneScript=0;sceneActor=0;sceneReleased=false;
        if(_game.id==GID_ZAK){
            // A room change can yield (SDL/Asyncify). Run the existing local
            // actor repair in the next native tick, never inside a JS export.
            CoopEvent e={{5,0,0,0,0,0,0,0}};incoming.push_back(e);
        }return 1;}
    return 0;
}
bool appbaseCoopRoomLight(int room, int &flags) {
    if (!localActor || room <= 0 || room >= 256 || !lightKnown[room]) return false;
    flags = roomLight[room];return true;
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
    if(kind==12 && _game.id==GID_MANIAC && _appbaseExtensions.partyActor(id)) {
        int flags=0;for(int i=0;i<5;++i){const unsigned bit=_appbaseExtensions.maniacActorBit(2936+8*i,id);if(_scummVars[bit>>4]&(1<<(bit&15)))flags|=1<<i;}return flags;
    }
    if(kind==11){
        if(id>0 && id<1024)return pendingPickup[id];
        int count=0;for(byte state : pendingPickup)if(state==1 || state>=3)++count;return count;
    }
    // Original Zak SWITCH availability: scripts 4/19 gate the verb with
    // global 199 and state04 of object 169; per-actor bit 1470 disables a kid.
    if(kind==10 && _game.id==GID_ZAK)return _setupIsComplete && appbaseInitialFramePresented() && _numActors>1 && _scummVars && _currentRoom>0 && _roomResource>0 && _currentRoom!=50 && !isScriptRunning(7) && appbaseTextCanSave()?1:0;
    if(kind==8 && _appbaseExtensions.partyActor(id))return _setupIsComplete && id<_numActors && _actors[id]->_room>0 && _actors[id]->_costume>0?1:0;
    if(kind==8 && _game.id==GID_ZAK && appbasePartyOriginalActor(true, id)){
        if(!_setupIsComplete || id>=_numActors || !_scummVars)return 0;
        const int bit=1470+id;
        return _scummVars[199]!=0 && (getState(169)&kObjectStateLocked)!=0 &&
            (_scummVars[bit>>4]&(1<<(bit&15)))==0 && _actors[id]->_room>0 ? 1:0;
    }
    if(kind==9 && _game.id==GID_ZAK && id>0 && id<=255)return id>127?_resourceMapper[id&127]:id;
    // Read-only UI inventory order for browser acceptance tests. Never mutate
    // the original object table or expose a way to grant an item.
    if(kind==7 && id>=1 && (id<=7 || _appbaseExtensions.partyActor(id))){
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
        case 29:return chatSpeech.size();case 30:return chatDeliveryStatus;case 31:return 1; // MM extension multiplayer feature.
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
        if(v[0]==39 && _game.id==GID_MANIAC && _appbaseExtensions.partyActor(v[1]) && v[3]>=0 && v[3]<=31) {
            for(int i=0;i<5;++i){const unsigned bit=_appbaseExtensions.maniacActorBit(2936+8*i,v[1]);const int mask=1<<(bit&15);if(v[3]&(1<<i))_scummVars[bit>>4]|=mask;else _scummVars[bit>>4]&=~mask;}
        }
        if(v[0]==36 && v[1]>0 && v[1]<1024 && (pendingPickup[v[1]]==1 || pendingPickup[v[1]]==3)) {
            // Ownership and exact canonical state were queued before this ack.
            // Rejected requests also release the original script without retrying.
            pendingPickup[v[1]]=pendingPickup[v[1]]==3?(v[3]==v[5]?4:5):(v[3]==v[5]?0:2);
        }
        if(v[0]==15 && v[1]>=(_game.id==GID_ZAK?7:8) && v[1]<_numActors && v[2]>=0 && v[2]<(_game.id==GID_ZAK?256:54)){
            Actor *actor=_actors[v[1]];actor->stopActorMoving();
            if(actor->_costume!=v[7])actor->setActorCostume(v[7]);
            actor->putActor(v[3],v[4],v[2]);actor->setDirection(v[5]);
            if(actor->_costume)actor->startAnimActor(v[6]);
        }
        if(v[0]==16 && _game.id==GID_ZAK && v[1]==96)_scummVars[96]=static_cast<int16>(v[3]);
        if(v[0]==16 && _game.id==GID_MANIAC && (v[1]==73 || v[1]==86 || v[1]==87 || v[1]==88 || v[1]==89 || v[1]==90 || v[1]==91 || v[1]==94 || v[1]==97 || v[1]==98 || v[1]==99 || v[1]==101 || v[1]==109 || v[1]==110 || v[1]==112 || v[1]==113 || v[1]==115 || v[1]==116 || v[1]==118 || v[1]==119 || v[1]==125 || v[1]==126 || v[1]==130 || v[1]==138 || v[1]==143 || v[1]==144 || v[1]==157 || v[1]==161 || v[1]==162 || v[1]==164 || v[1]==167 || v[1]==169 || v[1]==175))_scummVars[v[1]]=static_cast<int16>(v[3]);
        if(v[0]==16 && _game.id==GID_MANIAC && (v[1]==174 || v[1]==193) && _scummVars[v[1]]!=v[3]) {
            _scummVars[v[1]]=static_cast<int16>(v[3]);refreshPlant=true;
        }
        // Merge only learned training bits; retain all original capture bits
        // in this same word. Repeated canonical snapshots are idempotent.
        if(v[0]==16 && _game.id==GID_MANIAC && v[1]==184 && (v[3]&~254)==0)
            _scummVars[184]|=v[3];
        if(v[0]==17){
            if(_game.id==GID_MANIAC){
            // These original preludes execute before the synchronized cutscene.
            // Stop their timers/chases in the second context as well.
            if(v[1]==151)stopScript(91);
            // The source has already completed48's search and49's chase.
            // A delayed peer prelude must not overwrite transferred target112.
            if(v[1]==49){stopScript(48);stopScript(174);}
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
            }
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
            stopTalk();if(_game.id==GID_MANIAC)actorFollowCamera(sceneActor);
            if(_game.id==GID_MANIAC && v[1]==37){stopScript(152);stopScript(153);stopScript(36);stopScript(39);stopScript(40);}
        }
		if(v[0] == 11 && appbasePartyActor(v[1]) && appbasePartyActor(v[3]) && v[1]!=v[3]) conflictSpeech.push_back(incoming[i]);
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
		if(v[0] == 9 && appbasePartyActor(v[1]) && appbasePartyActor(v[7]) && v[1]!=v[7]) {
			bool changed=localActor!=v[1];
			if(changed) _actors[localActor]->stopActorMoving();
			if(remoteActor!=v[7]) _actors[remoteActor]->stopActorMoving();
			localActor=v[1]; remoteActor=v[7]; predicting=false;lastExtensionFlags=-1;
			if(changed) {
				Actor *a=_actors[localActor];a->stopActorMoving();a->putActor(v[3],v[4],v[2]);a->setDirection(v[5]);
				VAR(VAR_EGO)=localActor;if(_game.id==GID_ZAK)runScript(19,0,0,nullptr);actorFollowCamera(localActor);
                if(_game.id==GID_MANIAC && _appbaseExtensions.info(11,0)>0){_sentenceNum=0;stopScript(2);runScript(164,0,0,nullptr);runInventoryScript(1);}
                // Canonical selection runs before scummLoop records oldEgo.
                // Use the original EGO-change inventory refresh here too.
                handleMouseOver(true);
				lastRoom=-1;lastMoving=0;
			}
			redrawVerbs();
		}
        if(v[0] == 7 && v[1]>0 && v[1]<_numGlobalObjects && (v[3]>=0 && (v[3]<=OF_OWNER_ROOM || _appbaseExtensions.partyActor(v[3])))) {
            if(getOwner(v[1])!=v[3]) setOwnerOf(v[1],v[3]);
            if(!appbasePartyActor(v[3]))runInventoryScript(1);_fullRedraw=true;
        }
        if(v[0] == 14 && v[1]>=1 && v[1]<=255 && v[2]==_currentRoom && _actors[localActor]->_room==_currentRoom) {
            if(v[3]==1) { if(!_sound->isSoundRunning(v[1]))_sound->addSoundToQueue(v[1]); }
            else if(v[3]==0)_sound->stopSound(v[1]);
        }
		if(v[0] == 5) { // initialize/fix local player; each engine keeps its room context
            const bool changed=VAR(VAR_EGO)!=localActor;
			VAR(VAR_EGO)=localActor;
            if(_game.id==GID_ZAK)runScript(19,0,0,nullptr);
			actorFollowCamera(localActor);
            if(changed)handleMouseOver(true);
		}
		if(v[0] == 1 && v[1] == remoteActor && v[2] > 0 && v[2] < (_game.id==GID_ZAK?256:54)) {
			Actor *a=_actors[remoteActor];
            // Costume is part of every native actor pose, independent of which
            // original or extension script caused the change. Only accept an
            // installed resource; a sparse/missing costume cannot crash a peer.
            const bool costumePresent=v[7]==0 || (v[7]>0 && v[7]<_numCostumes &&
                (_appbaseExtensions.info(6,v[7])>0 || _res->_types[rtCostume][v[7]]._roomoffs!=RES_INVALID_OFFSET));
            const bool costumeChanged=costumePresent && a->_costume!=v[7];
            if(costumeChanged)a->setActorCostume(v[7]);
			bool animate=predicting && remoteMove.v[2]==v[2] && v[2]==_currentRoom;
			if (costumeChanged || !animate || a->_room!=v[2] || ABS(a->getRealPos().x-v[3])>4 || ABS(a->getRealPos().y-v[4])>8) {
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
			if(v[0] == 2 && (v[3] == localActor || v[3] == remoteActor || appbasePartyActor(v[3]) ||
                (_game.id==GID_ZAK && (v[3]==5 || v[3]==6)))) {
				bool present=false;
				for(int j=0;j<_numInventory;++j)if(_inventory[j] == obj)present=true;
				// For old bundles verify the object exists before using the normal importer.
				if(!present && appbaseObjectResource(obj)){addObjectToInventory(obj,0);present=true;}
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
        if(_game.id==GID_MANIAC && _appbaseExtensions.partyActor(localActor)) {
            const int flags=appbaseCoopGet(12,localActor,0);
            if(flags!=lastExtensionFlags){lastExtensionFlags=flags;emit(39,localActor,_actors[localActor]->_room,flags);}
        }
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
	if(frozen || (sceneStage==0 && appbaseCoopGet(11,0,0)>0)){clearClickedStatus();_keyPressed.reset();_mouseAndKeyboardStat=0;}
	return frozen;
}
const char *ScummEngine::appbaseCoopName(int id) {
    if(id<=0 || id>=_numActors) return "";
    const byte *name=_actors[id]->getActorName();
    return name ? reinterpret_cast<const char *>(name) : "";
}
// The observer checkpoint includes transient co-op context. It is never
// accepted from a spectator by a player or a player synchronization endpoint.
void appbaseCoopObserverSync(Common::Serializer &s) {
    for(int *v : {&localActor,&remoteActor,&ticks,&arcadeReplay,&selectionInputSlot,&sceneStage,&sceneScript,&sceneActor,&lastRoom,&lastMoving,&lastExtensionFlags,&conflictStage,&chatLength,&chatDeliveryStatus,&pendingSkipScript,&pendingSkipTicks,&peerRole,&peerFrame,&peerAck,&peerFailed,&peerMusic,&peerTextSpeed,&peerLeadLimit,&partyMenuQueued,&partyMenuPending,&partyMenuAge,&partyHoverX,&partyHoverY,&peerKeyFlags})s.syncAsSint32LE(*v);
    for(bool *v : {&refreshPlant,&refreshArcade,&sceneReleased,&applying,&frozen,&predicting,&mansionEntrySent,&fridgeHungerSent,&preparatoryFrozen,&pendingIntroStart,&introAutoHold,&peerInFrame,&partyMenuLocked,&partyMenuLocalAllowed})s.syncAsByte(*v);
    for(byte &v : pendingPickup)s.syncAsByte(v);
    for(bool &v : lightKnown)s.syncAsByte(v);
    for(int &v : roomLight)s.syncAsSint32LE(v);
    for(CoopEvent *event : {&remoteMove,&peerEnd,&peerInput})for(int &v : event->v)s.syncAsSint32LE(v);
    for(Common::Array<CoopEvent> *list : {&incoming,&conflictSpeech,&peerQueue,&peerRecord,&peerInputs,&peerSounds,&peerRandom,&peerChat}){
        uint32 size=list->size();s.syncAsUint32LE(size);if(size>4096)error("Observer co-op queue exceeds bound");
        if(s.isLoading())list->resize(size);for(CoopEvent &event : *list)for(int &v : event.v)s.syncAsSint32LE(v);
    }
    uint32 size=chatSpeech.size();s.syncAsUint32LE(size);if(size>8)error("Observer speech queue exceeds bound");
    if(s.isLoading())chatSpeech.resize(size);
    for(ChatSpeech &line : chatSpeech){s.syncAsSint32LE(line.actor);s.syncAsSint32LE(line.room);s.syncAsSint32LE(line.age);s.syncBytes(line.text,161);}
    s.syncBytes(chatText,161);
}
extern "C" {
// Narrow string bridge: ordered printable bytes, no pointers or escape codes.
EMSCRIPTEN_KEEPALIVE int mm_shared_mode(int role,int acknowledgement) {
    // Optional bounded pipeline window; the normal shared game keeps 60.
    if(g_scumm && g_scumm->_game.id==GID_ZAK && role==3){if(!peerRole || acknowledgement<1 || acknowledgement>60)return 0;peerLeadLimit=acknowledgement;return 1;}
    if(!g_scumm || g_scumm->_game.id!=GID_ZAK || role<0 || role>2 || acknowledgement<0)return 0;
    if(role!=peerRole){partyMenuLocked=false;partyMenuLocalAllowed=true;partyHoverX=partyHoverY=-1;partyMenuQueued=partyMenuPending=partyMenuAge=0;peerRole=role;peerLeadLimit=60;peerFrame=peerAck=peerFailed=0;peerInFrame=false;peerQueue.clear();peerChat.clear();peerRecord.clear();chatSpeech.clear();}
    peerAck=MAX(peerAck,acknowledgement);return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_shared_menu(int operation,int a,int b) {
    return g_scumm?g_scumm->appbasePartyMenu(operation,a,b):0;
}
EMSCRIPTEN_KEEPALIVE int mm_shared_reset() {
    partyMenuLocked=false;partyMenuLocalAllowed=true;partyHoverX=partyHoverY=-1;
    partyMenuQueued=partyMenuPending=partyMenuAge=0;
    localActor=remoteActor=0; predicting=false; incoming.clear(); frozen=true; preparatoryFrozen=true;
    sceneStage=0;sceneScript=0;sceneReleased=false;lastRoom=-1;lastMoving=0;
    memset(pendingPickup,0,sizeof(pendingPickup));
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
EMSCRIPTEN_KEEPALIVE int scumm_native_objects_version() { return 1; }
EMSCRIPTEN_KEEPALIVE int scumm_item_owner(int id,int owner) { return !g_scumm?0:(owner<0?g_scumm->appbaseItem(2,id):g_scumm->appbaseItem(3,id,owner)); }
EMSCRIPTEN_KEEPALIVE int scumm_item_wear(int id,int worn) { return g_scumm?g_scumm->appbaseItem(9,id,worn):0; }
EMSCRIPTEN_KEEPALIVE int scumm_item_id(int index) { return g_scumm?g_scumm->appbaseItem(5,index):0; }
EMSCRIPTEN_KEEPALIVE int mm_coop_version() { return 8; }
EMSCRIPTEN_KEEPALIVE int scumm_pickup_order_version() { return 3; }
EMSCRIPTEN_KEEPALIVE int scumm_maniac_party_version() { return 1; }
EMSCRIPTEN_KEEPALIVE int scumm_maniac_party_select(int actor) { return g_scumm && actor > 0 ? g_scumm->appbaseManiacPartySelect(actor) : 0; }
EMSCRIPTEN_KEEPALIVE int scumm_maniac_party_ready() { return g_scumm ? g_scumm->appbaseManiacPartySelect(-1) : 0; }
EMSCRIPTEN_KEEPALIVE int scumm_game_id() { return !g_scumm?0:(g_scumm->_game.id==GID_MANIAC?1:(g_scumm->_game.id==GID_ZAK?2:0)); }
EMSCRIPTEN_KEEPALIVE int mm_coop_prepare(int operation) { if(!g_scumm && operation==1){preparatoryFrozen=true;return 1;}return g_scumm?g_scumm->appbaseCoopPrepare(operation):0; }
EMSCRIPTEN_KEEPALIVE void mm_coop_seed(unsigned int seed) { if(g_scumm) g_scumm->appbaseCoopSeed(seed); }
EMSCRIPTEN_KEEPALIVE int mm_coop_skip(int script) {
	if(!g_scumm || (g_scumm->_game.id!=GID_MANIAC && g_scumm->_game.id!=GID_ZAK) || g_scumm->_game.version!=2 || script<0 || script>178) return 0;
    if(g_scumm->_game.id==GID_ZAK && ((sceneStage!=1 && sceneStage!=2) || script!=126))return 0;
	pendingSkipScript=script;pendingSkipTicks=0;return 1;
}
EMSCRIPTEN_KEEPALIVE int mm_coop_get(int kind,int id,int field) {
	return g_scumm ? g_scumm->appbaseCoopGet(kind,id,field) : -1;
}
EMSCRIPTEN_KEEPALIVE const char *mm_coop_actor_name(int id) {
    return g_scumm ? g_scumm->appbaseCoopName(id) : "";
}
EMSCRIPTEN_KEEPALIVE int mm_coop_enable(int actor,int peer) {
    if(!g_scumm)return 0;

	if((g_scumm->_game.id != GID_MANIAC && g_scumm->_game.id != GID_ZAK) || g_scumm->_game.version != 2 || actor<=0 || peer<=0 || (g_scumm->_game.id==GID_ZAK && (!g_scumm->appbasePartyActor(actor) || !g_scumm->appbasePartyActor(peer) || !g_scumm->appbasePartyActorAvailable(actor) || !g_scumm->appbasePartyActorAvailable(peer))) || actor==peer || actor>=g_scumm->appbaseCoopGet(0,0,3) || peer>=g_scumm->appbaseCoopGet(0,0,3) || g_scumm->appbaseCoopGet(1,actor,0)<=0) return 0;
	memset(pendingPickup,0,sizeof(pendingPickup));
	selectionInputSlot=-1;localActor=actor; remoteActor=peer; frozen=true; predicting=false; lastRoom=-1; lastMoving=0;lastExtensionFlags=-1;
	mansionEntrySent=g_scumm->appbaseCoopGet(4,138,0)!=0;fridgeHungerSent=g_scumm->appbaseCoopGet(4,144,0)!=0;
	conflictSpeech.clear();conflictStage=0;chatSpeech.clear();chatLength=0;
	preparatoryFrozen=false;pendingIntroStart=false;introAutoHold=false;pendingSkipScript=-1;
	CoopEvent e={{5,0,0,0,0,0,0,0}};incoming.push_back(e);
    refreshPlant=false;refreshArcade=g_scumm->_game.id==GID_MANIAC;arcadeReplay=-1;zakVictorySent=false;
    // A legacy shared save may predate canonical plant state. Publish the
    // actual restored globals once; the server merges both seats monotonically.
    const int room=g_scumm->appbaseCoopGet(1,actor,0);
    if(g_scumm->_game.id==GID_MANIAC){
    int trainingMask=0;
    for(int field=9;field<=11;++field){
        const int kid=g_scumm->appbaseCoopGet(0,0,field);
        if(kid>=1 && kid<=7)trainingMask|=1<<kid;
    }
    emit(20,184,room,g_scumm->appbaseCoopGet(4,184,0)&trainingMask,actor,1,0);
    const int growth=g_scumm->appbaseCoopGet(4,174,0);
    emit(20,174,room,growth==255?255:MIN(growth,2),actor,1,0);
    emit(20,193,room,g_scumm->appbaseCoopGet(4,193,0)?1:0,actor,1,0);
    }
	return 1;
}
EMSCRIPTEN_KEEPALIVE void mm_coop_freeze(int value) { frozen=value!=0;if(!localActor)preparatoryFrozen=value!=0; }
EMSCRIPTEN_KEEPALIVE int mm_coop_receive(int kind,int id,int room,int a,int b,int c,int d,int e) {
    if(kind==36 && (id<=0 || id>=1024 || room<0 || room>255 || a<0 || (a>15 && !(g_scumm && g_scumm->appbasePartyActor(a))) || b<0 || b>15 ||
        (c!=localActor && !(g_scumm && g_scumm->_game.id==GID_ZAK && (c==5 || c==6))) || d || e))return 0;
    if(kind==21 && (id!=10001 || room!=0 || a<1 || a>7 || b || c || d || e))return 0;
	if(g_scumm && g_scumm->_game.id==GID_ZAK && (kind==10 || kind==11 || kind==21 ||
        (kind==15 && id!=7 && id!=12) || (kind==16 && id!=96)))return 0;
	if(!localActor || incoming.size()>=256 || ((kind<1 || kind>4) && kind!=7 && kind!=8 && kind!=9 && kind!=10 && kind!=11 && kind!=14 && kind!=15 && kind!=16 && kind!=21 && kind!=36 && kind!=39)) return 0;
	CoopEvent event={{kind,id,room,a,b,c,d,e}};incoming.push_back(event);return 1;
}
}
} // namespace Scumm
#endif
