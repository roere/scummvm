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
    if(num(manifest,"schemaVersion")<7||vm._game.id!=GID_ZAK)error("NPC scripts require Zak schema 7");
    for(Common::JSONValue *item:list(manifest,"npcScripts")){
        if(_programs.size()>=8)error("Too many NPC programs");
        Program p;p.actor=num(item,"actor",94);
        if(!vm._appbaseExtensions.isActor(p.actor)||p.actor<13||vm._appbaseExtensions.partyActor(p.actor))error("NPC script may only own an added non-playable actor");
        for(const Program &other:_programs)if(other.actor==p.actor)error("Conflicting NPC behavior");
        const auto &code=list(item,"code");if(code.empty()||code.size()>1536)error("Invalid NPC program size");
        for(Common::JSONValue *entry:code){
            AppbaseCode::Instruction i;
            i.op=num(entry,"op",10);i.line=num(entry,"line",1000);i.actor=num(entry,"actor",94);i.target=num(entry,"target",65535);i.kind=num(entry,"kind",8);i.other=num(entry,"other",65535);i.value=num(entry,"value",3600000);i.mask=num(entry,"mask",15);i.jump=num(entry,"jump",code.size());
            if(!i.op||!i.line||(i.actor&&i.actor!=p.actor))error("Invalid NPC instruction/owner");
            if(i.op==AppbaseCode::Jump&&i.jump>code.size())error("Invalid NPC jump");
            if(i.op==AppbaseCode::Wait&&!i.value)error("Invalid NPC wait");
            if(i.op>AppbaseCode::Say)error("Unsupported native NPC action");
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
    // Read original room coordinates without loading the room into the scene,
    // running entry scripts, moving EGO, or changing the player's camera.
    for(unsigned room=1;room<(unsigned)vm._numRooms;++room){
        const auto &resource=vm._res->_types[rtRoom][room];
        if(resource._roomoffs==RES_INVALID_OFFSET||!Common::File::exists(vm.generateFilename(room)))continue;
        const byte *raw=vm.getResourceAddress(rtRoom,room);unsigned bytes=vm.getResourceSize(rtRoom,room);
        if(!raw||bytes<28)continue;unsigned count=raw[20];if(28+count*4>bytes)error("Invalid NPC room objects");
        unsigned ids[256]={};Object objects[256];
        for(unsigned n=0;n<count;++n){
            unsigned off=READ_LE_UINT16(raw+28+count*2+n*2);
            if(off+16>bytes)error("Invalid NPC object header");const byte *b=raw+off;
            ids[n+1]=READ_LE_UINT16(b+4);objects[n+1]={room,unsigned(b[11]),unsigned(b[12]&31)*4,b[10],unsigned(b[8]&128?8:0)};
        }
        for(unsigned n=1;n<=count;++n){Object o=objects[n];o.parent=o.parent<=count?ids[o.parent]:0;_objects[ids[n]]=o;}
    }
    for(const auto &e:_edges)if(!_objects.contains(e.exit)||!_objects.contains(e.entry)||_objects[e.exit].room!=e.from||_objects[e.entry].room!=e.to)error("NPC route doesn't match original objects");
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
        progress[7]=2;actor->stopActorMoving();
        Common::Point pos=actor->getRealPos();
        // Like walkActors(), advance once per native world frame. V2 uses
        // 8x2-pixel coordinate units and one logical unit per axis.
        const unsigned frames=1;
        int dx=int(x)-pos.x,dy=int(y)-pos.y;
        pos.x+=CLIP<int>(dx,-int(frames),int(frames));pos.y+=CLIP<int>(dy,-int(frames),int(frames));
        actor->putActor(pos.x,pos.y,actor->_room);
        return pos.x==int(x)&&pos.y==int(y)?AppbaseCode::Complete:AppbaseCode::Pending;
    }
    AppbaseCode::Result action(const AppbaseCode::Instruction &i,uint32 *progress,uint32 ms){
        Actor *actor=vm._actors[program.actor];
        if(i.op==AppbaseCode::Say){
            if(actor->_room!=vm._currentRoom)return AppbaseCode::Complete;
            if(!progress[0]){if(vm._haveMsg)return AppbaseCode::Blocked;vm._actorToPrintStrFor=program.actor;vm.actorTalk((const byte *)program.text[program.state.pc].c_str());progress[0]=1;return AppbaseCode::Pending;}
            return vm._haveMsg?AppbaseCode::Pending:AppbaseCode::Complete;
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
    unsigned ms=(delta*1000+30)/60;
    for(Program &p:_programs){
        const unsigned before=p.state.status;Adapter adapter{*this,vm,p,false};
        AppbaseCode::Interpreter<Adapter>::tick(p.code.data(),p.code.size(),p.state,adapter,ms);
        if(before==AppbaseCode::Running&&p.state.status==AppbaseCode::Error){
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
    out.writeUint32BE(MKTAG('A','B','N','P'));out.writeUint32LE(1);out.writeUint32LE(_programs.size());out.writeUint32LE(hash);
}
bool AppbaseNpc::load(Common::SeekableReadStream &in,int64 end,bool apply){
    if(_programs.empty())return true;if(end<(int64)size())return false;
    const int64 pos=in.pos();in.seek(end-size());Common::Array<AppbaseCode::State> states;uint32 hash=2166136261u;bool valid=true;
    for(const Program &p:_programs){AppbaseCode::State s;s.pc=in.readUint32LE();s.remaining=in.readUint32LE();s.age=in.readUint32LE();s.status=in.readUint32LE();s.error=in.readUint32LE();for(uint32 &n:s.action)n=in.readUint32LE();states.push_back(s);hash=hashState(s,hash);
        if(!AppbaseCode::Interpreter<Adapter>::valid(p.code.data(),p.code.size(),s)||s.action[0]>_edges.size()||s.action[7]>2||s.action[6]>=67)valid=false;
    }
    if(in.readUint32BE()!=MKTAG('A','B','N','P')||in.readUint32LE()!=1||in.readUint32LE()!=_programs.size()||in.readUint32LE()!=hash)valid=false;
    in.seek(pos);if(valid&&apply)for(unsigned i=0;i<states.size();++i)_programs[i].state=states[i];return valid;
}
}
