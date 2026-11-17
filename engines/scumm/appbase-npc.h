/* SCUMM adapter for the provider-owned Code Interpreter. GPL-3.0-or-later. */
#ifndef SCUMM_APPBASE_NPC_H
#define SCUMM_APPBASE_NPC_H
#include "common/array.h"
#include "common/str.h"
#include "common/stream.h"
#include "common/formats/json.h"
#include "scumm/appbase-interpreter.h"
namespace Scumm {
class ScummEngine;
class AppbaseNpc {
public:
    void configure(ScummEngine &vm,Common::JSONValue *manifest);
    void tick(ScummEngine &vm,unsigned delta);
    void save(Common::WriteStream &out) const;
    bool load(Common::SeekableReadStream &in,int64 end,bool apply);
    unsigned size() const {return _programs.empty()?0:16+52*_programs.size()+(_objectActions?80*24:0);}
    int info(int kind,int index) const;
    // Original object scripts keep their own actor and sentence context across
    // yields. They never borrow the player's sentence queue or camera.
    bool owns(const ScummEngine &vm,unsigned slot) const;
    bool active(const ScummEngine &vm) const;
    void clearSlot(unsigned slot);
    void scriptContext(ScummEngine &vm,unsigned slot,int object,int verb);
    void chainContext(unsigned slot);
    bool objectCode(ScummEngine &vm,unsigned object,unsigned verb,uint32 &base,int &entry);
    unsigned scriptRoom(const ScummEngine &vm,unsigned slot) const;
    bool beforeOpcode(ScummEngine &vm,unsigned opcode);
    bool sentence(ScummEngine &vm,unsigned verb,unsigned object,unsigned other,unsigned mode);
    bool waitingForSentence(const ScummEngine &vm) const;
    bool walking(unsigned actor) const { return _walkingActor == actor; }
    byte *walkMatrix(unsigned index);
    struct ScriptScope {
        ScriptScope(AppbaseNpc &host,ScummEngine &vm);
        ~ScriptScope();
        ScummEngine &vm;bool enabled;unsigned saved[6];
    };
private:
    struct Program { unsigned actor; Common::Array<AppbaseCode::Instruction> code; Common::Array<Common::String> text; AppbaseCode::State state; };
    struct Adapter;
    Common::Array<Program> _programs;
    Common::Array<AppbaseCode::Edge> _edges;
    struct Object { unsigned room,x,y,parent,parentState,offset; };
    Common::HashMap<unsigned,Object> _objects;
    struct Context { uint32 program=0,object=0,verb=0,other=0,room=0,parent=0; };
    Context _contexts[80],_starting,_chain;
    bool _objectActions=false;
    struct Navigation { Common::Array<byte> boxes, matrix; };
    Common::HashMap<unsigned,Navigation> _navigation;
    unsigned _walkingActor=0, _walkingRoom=0;
    Context context(const ScummEngine &vm) const;
    void reject(ScummEngine &vm);
};
}
#endif
