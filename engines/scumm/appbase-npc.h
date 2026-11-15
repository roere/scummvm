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
    unsigned size() const {return _programs.empty()?0:16+52*_programs.size();}
    int info(int kind,int index) const;
private:
    struct Program { unsigned actor; Common::Array<AppbaseCode::Instruction> code; Common::Array<Common::String> text; AppbaseCode::State state; };
    struct Adapter;
    Common::Array<Program> _programs;
    Common::Array<AppbaseCode::Edge> _edges;
    struct Object { unsigned room,x,y,parent,parentState; };
    Common::HashMap<unsigned,Object> _objects;
};
}
#endif
