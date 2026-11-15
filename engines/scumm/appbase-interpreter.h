// Provider-owned portable scheduler. GPL-3.0-or-later.
// No SCUMM types, global state, filesystem, networking, or language parsing.
#ifndef APPBASE_CODE_INTERPRETER_V1_H
#define APPBASE_CODE_INTERPRETER_V1_H
#include <stdint.h>
namespace AppbaseCode {
enum Operation { Jump=1, Branch=2, Wait=3, Walk=4, Say=5, Open=6, Close=7, Take=8, Give=9, Use=10 };
enum Result { Pending=0, Complete=1, Blocked=2, Failed=3, Waiting=4 };
enum Status { Running=0, Finished=1, Stopped=2, Error=3 };
struct Instruction {
    uint32_t op=0,line=0,actor=0,target=0,kind=0,other=0,value=0,mask=0,jump=0;
};
// Fixed, bounded save representation. No wall clock or JS timer survives a save.
struct State {
    uint32_t pc=0,remaining=0,age=0,status=Running,error=0;
    uint32_t action[8]={};
};
template<class Adapter> class Interpreter {
public:
    // Adapter action() must be idempotent for its persisted action[] progress.
    // The caller ticks every world frame, independently of the player's room.
    static void tick(const Instruction *code,uint32_t count,State &state,Adapter &adapter,uint32_t milliseconds) {
        if(state.status!=Running)return;
        if(state.pc>=count){state.status=Finished;return;}
        // A non-yielding infinite loop consumes at most 32 instructions per tick.
        // Even completed actions yield for one world frame, preserving fairness.
        for(unsigned budget=0;budget<32&&state.pc<count;++budget){
            const Instruction &i=code[state.pc];
            if(i.op<Jump||i.op>Use){fail(state,1);return;}
            if(i.op==Jump){if(i.jump>count){fail(state,1);return;}state.pc=i.jump;continue;}
            if(i.op==Branch){if(i.jump>count){fail(state,1);return;}state.pc=adapter.test(i)?state.pc+1:i.jump;continue;}
            if(i.op==Wait){
                if(!state.remaining)state.remaining=i.value;
                if(milliseconds<state.remaining){state.remaining-=milliseconds;return;}
                state.remaining=0;++state.pc;return;
            }
            Result result=adapter.action(i,state.action,milliseconds);
            if(result==Complete){++state.pc;state.age=0;for(unsigned j=0;j<8;++j)state.action[j]=0;return;}
            if(result==Failed){fail(state,2);return;}
            // A known external gate (e.g. a closed door) may wait indefinitely.
            // It is checked once per game tick and never consumes a busy loop.
            if(result==Waiting){state.age=0;return;}
            // A blocked route/verb is retried, then stopped with a source line.
            // Never skip an action or continue the script after failure.
            state.age=milliseconds>=120000-state.age?120000:state.age+milliseconds;
            if(state.age>=120000)fail(state,result==Blocked?3:4);
            return;
        }
        if(state.pc>=count)state.status=Finished;
    }
    static bool valid(const Instruction *code,uint32_t count,const State &s) {
        if(s.pc>count||s.status>Error||s.error>4||s.age>120000||s.remaining>3600000)return false;
        if(s.status!=Error&&s.error)return false;
        if(s.remaining&&(s.pc>=count||code[s.pc].op!=Wait||s.remaining>code[s.pc].value))return false;
        return true;
    }
    static void stop(State &s){s.status=Stopped;}
private:
    static void fail(State &s,uint32_t reason){s.status=Error;s.error=reason;}
};

struct Edge { uint32_t from=0,to=0,exit=0,entry=0,x=0,y=0,gate=0,script=0; };
// Breadth-first shortest route using the current game state. At each completed
// edge the caller plans again, so changed doors and scripts are respected.
template<class Adapter> int nextEdge(const Edge *edges,unsigned count,unsigned from,unsigned to,Adapter &adapter) {
    if(!from||from>255||!to||to>255||count>1024)return -1;
    unsigned queue[256]={},first[256]={};bool seen[256]={};unsigned head=0,tail=0;
    queue[tail++]=from;seen[from]=true;
    while(head<tail){unsigned room=queue[head++];
        for(unsigned e=0;e<count;++e){const Edge &edge=edges[e];
            if(edge.from!=room||!edge.to||edge.to>255||seen[edge.to]||!adapter.accessible(edge))continue;
            first[edge.to]=room==from?e:first[room];
            if(edge.to==to)return static_cast<int>(first[edge.to]);
            seen[edge.to]=true;queue[tail++]=edge.to;
        }
    }
    return -1;
}
}
#endif
