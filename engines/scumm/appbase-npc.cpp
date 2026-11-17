/* Native action/state boundary. Language and scheduling belong to the provider. */
#include "scumm/appbase-npc.h"
#include "scumm/scumm.h"
#include "scumm/actor.h"
#include "scumm/resource.h"
#include "common/file.h"
#include "common/textconsole.h"
namespace Scumm {
namespace {
unsigned num(Common::JSONValue *v,const char *key,unsigned max=0xffffffffu){
    if(!v||!v->isObject()||!v->hasChild(key)||!v->child(key)->isIntegerNumber()||v->child(key)->asIntegerNumber()<0||v->child(key)->asIntegerNumber()>max)error("Invalid NPC number %s",key);
    return v->child(key)->asIntegerNumber();
}
const Common::JSONArray &list(Common::JSONValue *v,const char *key){
    if(!v||!v->isObject()||!v->hasChild(key)||!v->child(key)->isArray())error("Invalid NPC list %s",key);
    return v->child(key)->asArray();
}
uint32 hashState(const AppbaseCode::State &s,uint32 hash){
    for(uint32 n:{s.pc,s.remaining,s.age,s.status,s.error})hash=(hash^n)*16777619u;
    for(uint32 n:s.action)hash=(hash^n)*16777619u;
    return hash;
}
}
void AppbaseNpc::configure(ScummEngine &vm,Common::JSONValue *manifest){
    if(!manifest->hasChild("npcScripts"))return;
    if(num(manifest,"schemaVersion")<7||(vm._game.id!=GID_ZAK&&vm._game.id!=GID_MANIAC))error("NPC scripts require a supported V2 game and schema 7");
    const unsigned originalActors=vm._game.id==GID_MANIAC?25:13;
    for(Common::JSONValue *item:list(manifest,"npcScripts")){
        if(_programs.size()>=8)error("Too many NPC programs");
        Program p;p.actor=num(item,"actor",94);
        if(!vm._appbaseExtensions.isActor(p.actor)||p.actor<originalActors||vm._appbaseExtensions.partyActor(p.actor))error("NPC script may only own an added non-playable actor");
        for(const Program &other:_programs)if(other.actor==p.actor)error("Conflicting NPC behavior");
        const auto &code=list(item,"code");if(code.empty()||code.size()>1536)error("Invalid NPC program size");
        for(Common::JSONValue *entry:code){
            AppbaseCode::Instruction i;
            i.op=num(entry,"op",10);i.line=num(entry,"line",1000);i.actor=num(entry,"actor",94);i.target=num(entry,"target",65535);i.kind=num(entry,"kind",8);i.other=num(entry,"other",65535);i.value=num(entry,"value",3600000);i.mask=num(entry,"mask",15);i.jump=num(entry,"jump",code.size());
            if(!i.op||!i.line||(i.actor&&i.actor!=p.actor))error("Invalid NPC instruction/owner");
            if(i.op==AppbaseCode::Jump&&i.jump>code.size())error("Invalid NPC jump");
            if(i.op==AppbaseCode::Wait&&!i.value)error("Invalid NPC wait");
            if(i.op>AppbaseCode::Close)error("Unsupported native NPC action");
            if(i.op==AppbaseCode::Open||i.op==AppbaseCode::Close){
                if(i.actor!=p.actor||i.kind!=2||!i.target||i.target>=(unsigned)vm._numGlobalObjects||i.other||i.value||i.mask||i.jump)error("Invalid NPC object verb");
                _objectActions=true;
            }
            if(i.op==AppbaseCode::Walk&&(!i.target||i.kind<1||i.kind>3||i.actor!=p.actor||(i.kind==1&&!vm._appbaseExtensions.isActor(i.target))||(i.kind==2&&i.target>=(unsigned)vm._numGlobalObjects)||(i.kind==3&&i.target>=(unsigned)vm._numRooms)))error("Invalid NPC walk target");
            if(i.op==AppbaseCode::Branch&&(i.kind<1||i.kind>5||!i.target||(i.kind==1&&(!i.other||!vm._appbaseExtensions.isActor(i.other)||i.target>=(unsigned)vm._numRooms))||(i.kind>=2&&i.target>=(unsigned)vm._numGlobalObjects)||(i.kind==5&&(!i.other||i.other>=(unsigned)vm._numGlobalObjects))))error("Invalid NPC condition");
            Common::String text;
            if(entry->hasChild("text")){
                if(!entry->child("text")->isString())error("Invalid NPC speech");text=entry->child("text")->asString();
                if(text.empty()||text.size()>160)error("Invalid NPC speech size");
                for(char c:text)if((unsigned char)c<32||(unsigned char)c>126)error("NPC speech must cross native ASCII boundary");
            }
            p.code.push_back(i);p.text.push_back(text);
        }
        _programs.push_back(p);
    }
    if(_programs.empty())error("Empty NPC scripts");
    for(Common::JSONValue *entry:list(manifest,"npcRoutes")){
        if(_edges.size()>=1024)error("Too many NPC routes");
        AppbaseCode::Edge e;e.from=num(entry,"from",255);e.to=num(entry,"to",255);e.exit=num(entry,"exit",65535);e.entry=num(entry,"entry",65535);e.x=num(entry,"x",32767);e.y=num(entry,"y",32767);e.gate=num(entry,"gate",65535);e.script=num(entry,"script",255);_edges.push_back(e);
    }
    if(vm._game.id==GID_MANIAC){
        // The original door handlers are the authority for this first MM
        // route. Do not infer passages through halls from room numbers.
        if(_edges.size()!=2)error("Invalid Maniac NPC routes");
        for(unsigned n=0;n<2;++n){
            const AppbaseCode::Edge &e=_edges[n];
            if(e.from!=(n?5u:3u)||e.to!=(n?3u:5u)||e.exit!=(n?135u:126u)||e.entry!=(n?126u:135u)||e.gate!=e.exit||e.x||e.y||e.script)error("Invalid Maniac NPC door passage");
        }
    }
    // Read original room coordinates without loading the room into the scene,
    // running entry scripts, moving EGO, or changing the player's camera.
    for(unsigned room=1;room<(unsigned)vm._numRooms;++room){
        const auto &resource=vm._res->_types[rtRoom][room];
        if(resource._roomoffs==RES_INVALID_OFFSET||!Common::File::exists(vm.generateFilename(room)))continue;
        const byte *raw=vm.getResourceAddress(rtRoom,room);unsigned bytes=vm.getResourceSize(rtRoom,room);
        if(!raw||bytes<28)continue;unsigned count=raw[20];if(28+count*4>bytes)error("Invalid NPC room objects");
        const unsigned offset=raw[21], boxes=offset<bytes?raw[offset]:0;
        if(boxes && offset+1+boxes*8+boxes*(boxes+1)<=bytes){
            Navigation &nav=_navigation[room];
            for(unsigned i=0;i<1+boxes*8;++i)nav.boxes.push_back(raw[offset+i]);
            for(unsigned i=0;i<boxes*(boxes+1);++i)nav.matrix.push_back(raw[offset+1+boxes*8+i]);
            for(unsigned i=0;i<boxes;++i)if(unsigned(nav.matrix[i])+boxes>nav.matrix.size()-boxes)error("Invalid NPC walk matrix");
        }
        unsigned ids[256]={};Object objects[256];
        for(unsigned n=0;n<count;++n){
            unsigned off=READ_LE_UINT16(raw+28+count*2+n*2);
            if(off+16>bytes)error("Invalid NPC object header");const byte *b=raw+off;
            ids[n+1]=READ_LE_UINT16(b+4);objects[n+1]={room,unsigned(b[11]),unsigned(b[12]&31)*4,b[10],unsigned(b[8]&128?8:0),off};
        }
        for(unsigned n=1;n<=count;++n){Object o=objects[n];o.parent=o.parent<=count?ids[o.parent]:0;_objects[ids[n]]=o;}
    }
    for(const auto &e:_edges)if(!_objects.contains(e.exit)||!_objects.contains(e.entry)||_objects[e.exit].room!=e.from||_objects[e.entry].room!=e.to)error("NPC route doesn't match original objects");
}
byte *AppbaseNpc::walkMatrix(unsigned index){
    if(!_walkingRoom||!_navigation.contains(_walkingRoom))return nullptr;
    Navigation &nav=_navigation[_walkingRoom];
    return index==2?nav.boxes.data():index==1?nav.matrix.data():nullptr;
}
bool AppbaseNpc::owns(const ScummEngine &vm,unsigned slot) const {
    return _objectActions&&slot<80&&_contexts[slot].program&&_contexts[slot].program<=_programs.size()&&vm.vm.slot[slot].status!=ssDead;
}
AppbaseNpc::Context AppbaseNpc::context(const ScummEngine &vm) const {
    if(_starting.program)return _starting;
    return owns(vm,vm._currentScript)?_contexts[vm._currentScript]:_chain;
}
bool AppbaseNpc::active(const ScummEngine &vm) const {return context(vm).program!=0;}
void AppbaseNpc::clearSlot(unsigned slot){
    if(slot>=80)return;
    _contexts[slot]=Context();
    for(Context &c:_contexts)if(c.parent==slot+1)c.parent=0;
}
void AppbaseNpc::scriptContext(ScummEngine &vm,unsigned slot,int object,int verb){
    if(slot>=80)return;
    Context c=context(vm);_chain=Context();
    if(c.program&&owns(vm,vm._currentScript))c.parent=vm._currentScript+1;
    if(c.program&&object){
        c.object=object;c.verb=verb;
        if(_objects.contains(object))c.room=_objects[object].room;
    }
    _contexts[slot]=c;
}
void AppbaseNpc::chainContext(unsigned slot){_chain=slot<80?_contexts[slot]:Context();}
unsigned AppbaseNpc::scriptRoom(const ScummEngine &vm,unsigned slot) const {return owns(vm,slot)?_contexts[slot].room:0;}
bool AppbaseNpc::objectCode(ScummEngine &vm,unsigned object,unsigned verb,uint32 &base,int &entry){
    Context c=context(vm);if(!c.program)return false;
    base=0;entry=0;
    if(!_objects.contains(object)||!verb||verb>=250)return true;
    const Object &o=_objects[object];const unsigned actor=_programs[c.program-1].actor;
    if(vm.getOwner(object)!=actor&&(vm.getOwner(object)!=vm.OF_OWNER_ROOM||o.room!=vm._actors[actor]->_room))return true;
    const byte *raw=vm.getResourceAddress(rtRoom,o.room);unsigned bytes=vm.getResourceSize(rtRoom,o.room);
    if(!raw||o.offset+16>bytes)return true;
    const unsigned size=READ_LE_UINT16(raw+o.offset);
    if(size<16||o.offset+size>bytes)return true;
    for(unsigned n=15;n+1<size&&raw[o.offset+n];n+=2){
        if(raw[o.offset+n]==verb||raw[o.offset+n]==255){
            base=o.offset;entry=raw[o.offset+n+1];
            if(entry>=int(size))entry=0;
            return true;
        }
    }
    return true;
}
AppbaseNpc::ScriptScope::ScriptScope(AppbaseNpc &host,ScummEngine &engine):vm(engine),enabled(host.owns(vm,vm._currentScript)){
    if(!enabled)return;
    const Context c=host._contexts[vm._currentScript];
    const unsigned keys[]={vm.VAR_EGO,vm.VAR_ACTIVE_VERB,vm.VAR_ACTIVE_OBJECT1,vm.VAR_ACTIVE_OBJECT2,vm.VAR_ROOM,vm.VAR_ROOM_RESOURCE};
    for(unsigned n=0;n<6;++n)saved[n]=vm._scummVars[keys[n]];
    vm._scummVars[vm.VAR_EGO]=host._programs[c.program-1].actor;
    vm._scummVars[vm.VAR_ACTIVE_VERB]=c.verb;vm._scummVars[vm.VAR_ACTIVE_OBJECT1]=c.object;vm._scummVars[vm.VAR_ACTIVE_OBJECT2]=c.other;
    const unsigned room=vm._actors[host._programs[c.program-1].actor]->_room;
    vm._scummVars[vm.VAR_ROOM]=room;vm._scummVars[vm.VAR_ROOM_RESOURCE]=room;
}
AppbaseNpc::ScriptScope::~ScriptScope(){
    if(!enabled)return;
    const unsigned keys[]={vm.VAR_EGO,vm.VAR_ACTIVE_VERB,vm.VAR_ACTIVE_OBJECT1,vm.VAR_ACTIVE_OBJECT2,vm.VAR_ROOM,vm.VAR_ROOM_RESOURCE};
    for(unsigned n=0;n<6;++n)vm._scummVars[keys[n]]=saved[n];
}
void AppbaseNpc::reject(ScummEngine &vm){
    const Context c=context(vm);if(!c.program)return;
    Program &p=_programs[c.program-1];p.state.status=AppbaseCode::Error;p.state.error=2;
    warning("NPC-Skript, Figur %u, Zeile %u: Originalaktion erfordert eine nicht unterstuetzte Szenen- oder Steuerungsaktion",p.actor,p.state.pc<p.code.size()?p.code[p.state.pc].line:0);
    for(unsigned n=0;n<80;++n)if(_contexts[n].program==c.program){vm.vm.slot[n].status=ssDead;_contexts[n]=Context();}
    vm._actors[p.actor]->stopActorMoving();vm._actors[p.actor]->startAnimActor(vm._actors[p.actor]->_standFrame);vm._currentScript=0xff;
}
bool AppbaseNpc::beforeOpcode(ScummEngine &vm,unsigned op){
    if(!owns(vm,vm._currentScript))return true;
    // An object command may alter its world objects, but may not seize the
    // player's room, camera, menus, save slot, or cutscene controller.
    if((op&0x3f)==0x24||(op&0x7f)==0x72||(op&0x7f)==0x12||(op&0x7f)==0x32||(op&0x7f)==0x52||
       (op&0x7f)==0x40||op==0x58||(op&0x7f)==0x22||(op&0x7f)==0x60||(op&0x7f)==0x7a){reject(vm);return false;}
    const Context c=_contexts[vm._currentScript];
    const bool offscreen=vm._actors[_programs[c.program-1].actor]->_room!=vm._currentRoom;
    unsigned speaker=_programs[c.program-1].actor;
    bool silent=op==0xd8&&offscreen;
    if((op&0x7f)==0x14){
        speaker=vm.fetchScriptByte();if(op&128)speaker=vm.readVar(speaker);
        silent=speaker==255?offscreen:(speaker<(unsigned)vm._numActors&&vm._actors[speaker]->_room!=vm._currentRoom);
        if(!silent)--vm._scriptPointer;
    }
    if(silent){
        // actorTalk's offscreen early return occurs after converting into the
        // shared text buffer. Consume the original packed string here instead,
        // preserving any dialogue that is currently visible to the player.
        const Common::String key=vm.appbaseTextKey();
        byte text[512]={},expanded[512];unsigned count=0;
        for(byte packed;(packed=vm.fetchScriptByte())!=0;){
            const byte ch=packed&127;const bool space=packed&128;
            if(count+5>=sizeof(text)){reject(vm);return false;}
            if(ch<8){text[count++]=255;text[count++]=ch;if(ch>3){text[count++]=vm.fetchScriptByte();text[count++]=0;}}
            else text[count++]=ch;
            if(space)text[count++]=' ';
        }
        text[count]=0;vm.convertMessageToString(vm.appbaseText(text,key,0),expanded,sizeof(expanded));
        vm.appbaseCameraSpeech(speaker==255?_programs[c.program-1].actor:speaker,expanded,false);
        return false;
    }
    if(offscreen&&((op&0x7f)==0x1c||(op&0x7f)==0x3c)){vm.fetchScriptByte();return false;}
    bool visibleOnly=(op&0x1f)==0x01||(op&0x3f)==0x0d||(op&0x3f)==0x0e||(op&0x1f)==0x1e||
        (op&0x3f)==0x36||(op&0x7f)==0x2d||(op&0x7f)==0x6d||(op&0x3f)==0x33||(op&0x7f)==0x70;
    if(offscreen&&(op&0x7f)==0x42){
        unsigned script=vm.fetchScriptByte();--vm._scriptPointer;
        if(op&128)script=vm.readVar(script);
        visibleOnly=script>=(unsigned)vm._numGlobalScripts;
    }
    if(offscreen&&visibleOnly){--vm._scriptPointer;vm.updateScriptPtr();vm._currentScript=0xff;return false;}
    return true;
}
bool AppbaseNpc::sentence(ScummEngine &vm,unsigned verb,unsigned object,unsigned other,unsigned mode){
    if(!active(vm))return false;
    if(mode==2)return true;
    if(mode>2||verb>=250||!object){reject(vm);return true;}
    const Context before=_starting;_starting=context(vm);_starting.object=object;_starting.verb=verb;_starting.other=other;
    uint32 base;int entry;objectCode(vm,object,verb,base,entry);
    if(entry)vm.runObjectScript(object,verb,false,false,nullptr);
    else reject(vm);
    _starting=before;return true;
}
bool AppbaseNpc::waitingForSentence(const ScummEngine &vm) const {
    Context c=context(vm);
    for(unsigned n=0;n<80;++n)if(n!=vm._currentScript&&owns(vm,n)&&_contexts[n].program==c.program&&
        (vm.vm.slot[n].where==WIO_ROOM||vm.vm.slot[n].where==WIO_INVENTORY)){
        unsigned parent=_contexts[n].parent;
        for(unsigned depth=0;parent&&parent<=80&&depth<80;++depth){
            if(parent==vm._currentScript+1)return true;
            parent=_contexts[parent-1].parent;
        }
    }
    return false;
}
struct AppbaseNpc::Adapter {
    AppbaseNpc &host;ScummEngine &vm;Program &program;bool ignoreGates;
    bool accessible(const AppbaseCode::Edge &e){return (ignoreGates||((!e.gate||(vm.getState(e.gate)&8))&&(!e.script||!vm.isScriptRunning(e.script))))&&!vm._appbaseExtensions.roomRefusal(program.actor,e.to);}
    bool visible(unsigned object,unsigned room){
        if(!host._objects.contains(object)||host._objects[object].room!=room||vm.getOwner(object)!=vm.OF_OWNER_ROOM)return false;
        unsigned n=object;
        for(unsigned depth=0;depth<64;++depth){const Object &o=host._objects[n];if(!o.parent)return true;if(!host._objects.contains(o.parent)||(vm.getState(o.parent)&8)!=o.parentState)return false;n=o.parent;}
        return false;
    }
    bool test(const AppbaseCode::Instruction &i){
        if(i.kind==1)return i.other<(unsigned)vm._numActors&&vm._appbaseExtensions.isActor(i.other)&&vm._actors[i.other]->_room==i.target;
        if(i.kind==2)return vm.getOwner(i.target)==program.actor;
        if(i.kind==3)return (vm.getState(i.target)&i.mask)==i.value;
        if(i.kind==4)return visible(i.target,vm._actors[program.actor]->_room);
        if(i.kind==5)return host._objects.contains(i.target)&&host._objects[i.target].parent==i.other&&vm.getOwner(i.target)==vm.OF_OWNER_ROOM;
        return false;
    }
    AppbaseCode::Result move(Actor *actor,unsigned x,unsigned y,uint32 *progress,uint32 ms){
        if(actor->_room==vm._currentRoom){
            // If the player entered during an offscreen leg, start the native
            // walker from the saved logical position and respect walkboxes.
            if(progress[7]!=1){actor->startWalkActor(x,y,-1);progress[7]=1;return AppbaseCode::Pending;}
            if(actor->_moving)return AppbaseCode::Pending;
            // A failed native walk must not silently teleport through a wall.
            Common::Point p=actor->getRealPos();
            if(ABS(p.x-int(x))>3||ABS(p.y-int(y))>12){progress[7]=0;return AppbaseCode::Blocked;}
            return AppbaseCode::Complete;
        }
        if(!host._navigation.contains(actor->_room))return AppbaseCode::Blocked;
        host._walkingActor=program.actor;host._walkingRoom=actor->_room;
        const int result=actor->appbaseNpcWalk(x,y,progress[7]!=2||!actor->_moving);
        host._walkingActor=host._walkingRoom=0;progress[7]=2;
        return result>0?AppbaseCode::Complete:result<0?AppbaseCode::Blocked:AppbaseCode::Pending;
    }
    AppbaseCode::Result action(const AppbaseCode::Instruction &i,uint32 *progress,uint32 ms){
        Actor *actor=vm._actors[program.actor];
        if(i.op==AppbaseCode::Say){
            if(actor->_room!=vm._currentRoom){
                byte text[512];vm.convertMessageToString((const byte *)program.text[program.state.pc].c_str(),text,sizeof(text));
                const int duration=vm.appbaseCameraSpeech(program.actor,text,false,progress[0]?progress[1]:0);
                if(!progress[0]){progress[1]=duration*1000/60;progress[0]=1;}
                if(ms<progress[1]){progress[1]-=ms;return AppbaseCode::Pending;}
                return AppbaseCode::Complete;
            }
            if(!progress[0]){if(vm._haveMsg)return AppbaseCode::Blocked;vm._actorToPrintStrFor=program.actor;vm.actorTalk((const byte *)program.text[program.state.pc].c_str());progress[0]=1;return AppbaseCode::Pending;}
            return vm._haveMsg?AppbaseCode::Pending:AppbaseCode::Complete;
        }
        if(i.op==AppbaseCode::Open||i.op==AppbaseCode::Close){
            if(progress[0]){
                // Original handlers can finish before a helper they started.
                // Keep the action at its source line until that whole call
                // finishes; an error must not be attributed to the next line.
                for(unsigned slot=0;slot<80;++slot)if(host.owns(vm,slot)&&host._contexts[slot].program==unsigned(&program-host._programs.data())+1)
                    return actor->_room==vm._currentRoom?AppbaseCode::Pending:AppbaseCode::Waiting;
                return program.state.status==AppbaseCode::Error?AppbaseCode::Failed:AppbaseCode::Complete;
            }
            if(!host._objects.contains(i.target))return AppbaseCode::Failed;
            if(vm.getOwner(i.target)!=program.actor){
                if(!visible(i.target,actor->_room))return AppbaseCode::Blocked;
                const Object &o=host._objects[i.target];
                AppbaseCode::Result moved=move(actor,o.x,o.y,progress,ms);if(moved!=AppbaseCode::Complete)return moved;
            }
            for(unsigned n=0;n<80;++n)if(vm.vm.slot[n].status!=ssDead&&vm.vm.slot[n].number==i.target&&vm.vm.slot[n].where==WIO_ROOM)return AppbaseCode::Waiting;
            unsigned slot=1;while(slot<80&&vm.vm.slot[slot].status!=ssDead)++slot;if(slot==80)return AppbaseCode::Waiting;
            Context c;c.program=unsigned(&program-host._programs.data())+1;c.object=i.target;c.verb=i.op==AppbaseCode::Open?1:2;c.room=host._objects[i.target].room;
            host._starting=c;uint32 base;int entry;host.objectCode(vm,i.target,c.verb,base,entry);
            if(!entry){host._starting=Context();return AppbaseCode::Failed;}
            host.clearSlot(slot);progress[0]=1;progress[6]=slot+1;
            vm.runObjectScript(i.target,c.verb,false,false,nullptr,slot);host._starting=Context();
            return program.state.status==AppbaseCode::Error?AppbaseCode::Failed:AppbaseCode::Pending;
        }
        if(i.op!=AppbaseCode::Walk)return AppbaseCode::Failed;
        unsigned destination=i.target;
        if(i.kind==1){
            if(destination>=(unsigned)vm._numActors||!vm._appbaseExtensions.isActor(destination))return AppbaseCode::Failed;
            Actor *target=vm._actors[destination];if(actor->_room!=target->_room)return AppbaseCode::Blocked;
            Common::Point p=target->getRealPos();return move(actor,p.x,p.y,progress,ms);
        }
        if(i.kind==2){
            if(!visible(destination,actor->_room))return AppbaseCode::Blocked;
            const Object &o=host._objects[destination];return move(actor,o.x,o.y,progress,ms);
        }
        if(actor->_room==destination)return AppbaseCode::Complete;
        if(!progress[0]){
            int edge=AppbaseCode::nextEdge(host._edges.data(),host._edges.size(),actor->_room,destination,*this);
            if(edge<0){ignoreGates=true;const int possible=AppbaseCode::nextEdge(host._edges.data(),host._edges.size(),actor->_room,destination,*this);ignoreGates=false;return possible<0?AppbaseCode::Failed:AppbaseCode::Waiting;}progress[0]=edge+1;progress[7]=0;
        }
        const auto &edge=host._edges[progress[0]-1];
        if(actor->_room!=edge.from||!accessible(edge)){progress[0]=progress[7]=0;return AppbaseCode::Waiting;}
        const Object &exit=host._objects[edge.exit];
        AppbaseCode::Result moved=move(actor,exit.x,exit.y,progress,ms);if(moved!=AppbaseCode::Complete)return moved;
        const Object &entry=host._objects[edge.entry];
        actor->stopActorMoving();actor->putActor(edge.x?edge.x:entry.x,edge.y?edge.y:entry.y,edge.to);
        progress[0]=progress[7]=0;return AppbaseCode::Pending;
    }
};
void AppbaseNpc::tick(ScummEngine &vm,unsigned delta){
    if(!delta||_programs.empty()||!vm._setupIsComplete||!vm._currentRoom||vm._saveLoadFlag||vm.vm.cutSceneStackPointer||vm._userPut<=0)return;
    if(vm._game.id==GID_MANIAC&&vm._currentRoom==45)return;
    if(_navigation.contains(vm._currentRoom)){
        Navigation &nav=_navigation[vm._currentRoom];
        const byte *boxes=vm.getResourceAddress(rtMatrix,2);
        if(boxes&&vm.getResourceSize(rtMatrix,2)==nav.boxes.size())memcpy(nav.boxes.data(),boxes,nav.boxes.size());
        const byte *matrix=vm.getResourceAddress(rtMatrix,1);
        if(matrix&&vm.getResourceSize(rtMatrix,1)==nav.matrix.size())memcpy(nav.matrix.data(),matrix,nav.matrix.size());
    }
    unsigned ms=(delta*1000+30)/60;
    for(Program &p:_programs){
        const unsigned before=p.state.status;Adapter adapter{*this,vm,p,false};
        AppbaseCode::Interpreter<Adapter>::tick(p.code.data(),p.code.size(),p.state,adapter,ms);
        if(before==AppbaseCode::Running&&p.state.status==AppbaseCode::Error){
            for(unsigned slot=0;slot<80;++slot)if(_contexts[slot].program==unsigned(&p-_programs.data())+1){vm.vm.slot[slot].status=ssDead;clearSlot(slot);}
            vm._actors[p.actor]->stopActorMoving();vm._actors[p.actor]->startAnimActor(vm._actors[p.actor]->_standFrame);
            const char *messages[]={"", "Ungueltige Programmposition", "Kein freigegebener Weg oder keine unterstuetzte Aktion", "Das Ziel bleibt blockiert", "Die Aktion wurde nicht innerhalb von 120 Sekunden abgeschlossen"};
            warning("NPC-Skript, Figur %u, Zeile %u: %s",p.actor,p.state.pc<p.code.size()?p.code[p.state.pc].line:0,messages[p.state.error]);
        }
    }
}
int AppbaseNpc::info(int kind,int index) const {
    if(kind==0)return _programs.size();if(index<0||index>=(int)_programs.size())return -1;const Program &p=_programs[index];
    switch(kind){case 1:return p.state.status;case 2:return p.state.pc<p.code.size()?p.code[p.state.pc].line:0;case 3:return p.state.pc;case 4:return p.state.remaining;case 5:return p.state.error;case 6:return p.actor;default:return -1;}
}
void AppbaseNpc::save(Common::WriteStream &out) const {
    if(_programs.empty())return;uint32 hash=2166136261u;
    for(const Program &p:_programs){const auto &s=p.state;for(uint32 n:{s.pc,s.remaining,s.age,s.status,s.error})out.writeUint32LE(n);for(uint32 n:s.action)out.writeUint32LE(n);hash=hashState(s,hash);}
    if(_objectActions)for(const Context &c:_contexts)for(uint32 n:{c.program,c.object,c.verb,c.other,c.room,c.parent}){out.writeUint32LE(n);hash=(hash^n)*16777619u;}
    out.writeUint32BE(MKTAG('A','B','N','P'));out.writeUint32LE(_objectActions?2:1);out.writeUint32LE(_programs.size());out.writeUint32LE(hash);
}
bool AppbaseNpc::load(Common::SeekableReadStream &in,int64 end,bool apply){
    if(_programs.empty())return true;if(end<(int64)size())return false;
    const int64 pos=in.pos();in.seek(end-size());Common::Array<AppbaseCode::State> states;uint32 hash=2166136261u;bool valid=true;
    for(const Program &p:_programs){AppbaseCode::State s;s.pc=in.readUint32LE();s.remaining=in.readUint32LE();s.age=in.readUint32LE();s.status=in.readUint32LE();s.error=in.readUint32LE();for(uint32 &n:s.action)n=in.readUint32LE();states.push_back(s);hash=hashState(s,hash);
        const bool verb=s.pc<p.code.size()&&(p.code[s.pc].op==AppbaseCode::Open||p.code[s.pc].op==AppbaseCode::Close);
        if(!AppbaseCode::Interpreter<Adapter>::valid(p.code.data(),p.code.size(),s)||s.action[0]>(verb?1:_edges.size())||s.action[7]>2||s.action[6]>(verb?80:66))valid=false;
    }
    Context contexts[80];
    if(_objectActions)for(Context &c:contexts){
        c.program=in.readUint32LE();c.object=in.readUint32LE();c.verb=in.readUint32LE();c.other=in.readUint32LE();c.room=in.readUint32LE();c.parent=in.readUint32LE();
        for(uint32 n:{c.program,c.object,c.verb,c.other,c.room,c.parent})hash=(hash^n)*16777619u;
        if(c.program>_programs.size()||c.parent>80||(!c.program&&(c.object||c.verb||c.other||c.room||c.parent))||
           (c.program&&(!_objects.contains(c.object)||!c.verb||c.verb>=250||c.other>65535||c.room!=_objects[c.object].room)))valid=false;
    }
    if(_objectActions)for(unsigned slot=0;slot<80;++slot){unsigned parent=contexts[slot].parent;for(unsigned depth=0;parent&&parent<=80;++depth){if(parent==slot+1||depth>=80){valid=false;break;}parent=contexts[parent-1].parent;}}
    if(in.readUint32BE()!=MKTAG('A','B','N','P')||in.readUint32LE()!=(_objectActions?2u:1u)||in.readUint32LE()!=_programs.size()||in.readUint32LE()!=hash)valid=false;
    in.seek(pos);if(valid&&apply){for(unsigned i=0;i<states.size();++i)_programs[i].state=states[i];if(_objectActions)for(unsigned i=0;i<80;++i)_contexts[i]=contexts[i];_starting=_chain=Context();}return valid;
}
}
