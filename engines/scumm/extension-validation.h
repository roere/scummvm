/* Optional SCUMM extension validation. GPL-3.0-or-later; see COPYING. */
#ifndef SCUMM_EXTENSION_VALIDATION_H
#define SCUMM_EXTENSION_VALIDATION_H
#include <stddef.h>
#include <stdint.h>

namespace Scumm {
struct ExtensionV2Limits {
 unsigned actors, rooms, scripts, objects, costumes, variables, ownerRoom;
 const bool *actorIds;
 unsigned sounds;
 const bool *roomIds, *scriptIds, *costumeIds, *soundIds;
};

// Parses a bounded subset of the existing V2 handlers; never executes bytecode.
// Variable operands are checked as variable references. Their runtime values,
// including V2's special indirect read variables 14..16, remain VM semantics.
inline bool validateExtensionV2(const unsigned char *p, size_t size, const ExtensionV2Limits &l, const char **why) {
 if (!p || size < 5 || size > UINT16_MAX || (size_t)(p[0] | (p[1] << 8)) != size) { *why="invalid v2 resource size"; return false; }
 unsigned char *boundary=new unsigned char[size]();
 unsigned *writes=new unsigned[size], *next=new unsigned[size](), *indegree=new unsigned[size](), *queue=new unsigned[size];
 int *targets=new int[size];
 for (size_t i=0;i<size;++i) {targets[i]=INT32_MIN;writes[i]=UINT32_MAX;}
 size_t pos=4,at=4; unsigned op=0,last=255; bool valid=true; const char *reason="invalid v2 bytecode";
 auto bad=[&](const char *s) { valid=false;reason=s; };
 auto byte=[&]() -> unsigned { if(pos>=size){bad("truncated v2 instruction");return 0;}return p[pos++]; };
 auto word=[&]() -> unsigned { unsigned a=byte(),b=byte();return a|(b<<8); };
 auto var=[&]() -> unsigned { unsigned v=byte();if(v>=l.variables)bad("invalid variable reference");return v; };
 auto result=[&]() { unsigned v=var();writes[at]=v; };
 auto ref=[&](unsigned n,unsigned limit,const bool *present,bool zero,const char *message) {
  if ((!zero&&!n)||n>=limit||(present&&!present[n]))bad(message);
 };
 enum Kind { Number, Actor, Object, Room, Script, Costume, Sound, Owner };
 auto arg=[&](unsigned mask,bool wide,Kind kind=Number) -> unsigned {
  if(op&mask){var();return UINT32_MAX;}
  unsigned n=wide?word():byte();
  switch(kind){
  case Actor:ref(n,l.actors,l.actorIds,false,"invalid actor reference");break;
  case Object:ref(n,l.objects,nullptr,true,"invalid object reference");break;
  case Room:ref(n,l.rooms,l.roomIds,true,"missing room resource");break;
  case Script:ref(n,l.scripts,l.scriptIds,false,"missing script resource");break;
  case Costume:ref(n,l.costumes,l.costumeIds,true,"missing costume resource");break;
  case Sound:ref(n,l.sounds,l.soundIds,true,"missing sound resource");break;
  case Owner:if(n&&n!=l.ownerRoom)ref(n,l.actors,l.actorIds,false,"invalid owner reference");break;
  default:break;
  }return n;
 };
 auto branch=[&]() { unsigned delta=word();targets[at]=(int)pos+(int16_t)delta; };
 auto text=[&](bool packed) {
  for(unsigned n=0;valid;++n){
   unsigned c=byte();if(!c)break;
   if(n>=4096){bad("v2 string too long");break;}
   if(packed && (c&127)>3 && (c&127)<8)byte();
  }
 };
 while(valid&&pos<size){
  at=pos;boundary[at]=1;op=byte();last=op;
  switch(op){
  case 0x00:case 0xa0:case 0x80:case 0xac:case 0x98:case 0x4c:break;
  case 0x01:case 0x1e:case 0xfe:arg(128,false,Actor);arg(64,false);arg(32,false);break;
  case 0x11:arg(128,false,Actor);arg(64,false);break;
  case 0x2d:arg(128,false,Actor);arg(64,false,Room);break;
  case 0x09:case 0xc9:arg(128,false,Actor);arg(64,true,Object);break;
  case 0x13:case 0x53:{
   arg(128,false,Actor);unsigned value=arg(64,false),sub=byte();
   if(sub==1 && value!=UINT32_MAX)ref(value,l.sounds,l.soundIds,true,"missing sound resource");
   else if(sub==4 && value!=UINT32_MAX)ref(value,l.costumes,l.costumeIds,true,"missing costume resource");
   else if(sub==5) {if(value!=UINT32_MAX&&value>15)bad("invalid talk color");}
   else if(sub==2) {if(byte()>15|| (value!=UINT32_MAX&&value>15))bad("invalid palette color");}
   else if(sub==3)text(false);
   else if(sub!=1&&sub!=4)bad("unsupported actor operation");
   break;
  }
  case 0x1a:case 0x9a:case 0x3a:case 0x5a:result();arg(128,true);break;
  case 0x2c:result();byte();break;
  case 0x46:result();break;
  case 0x18:branch();break;
  case 0x04:case 0x08:case 0x38:case 0x44:case 0x48:case 0x78:case 0x88:case 0xc8:var();arg(128,true);branch();break;
  case 0x28:case 0xa8:var();branch();break;
  case 0x0f:case 0x6f:case 0xff:arg(128,true,Object);branch();break;
  case 0x1d:case 0x9d:arg(128,true,Object);arg(64,false);branch();break;
  case 0x31:case 0xb1:{result();unsigned base=word(),index=arg(128,false);if(base>=l.variables*16||(index!=UINT32_MAX&&base+index>=l.variables*16))bad("invalid bit variable");break;}
  case 0x42:case 0x4a:arg(128,false,Script);break;
  case 0x62:{unsigned id=byte();if(id)ref(id,l.scripts,l.scriptIds,false,"missing script resource");break;}
  case 0x52:case 0xd2:arg(128,false,Actor);break;
  case 0x72:arg(128,false,Room);break;
  case 0x68:result();arg(128,false,Script);break;
  case 0x10:case 0x90:case 0x6c:case 0xec:result();arg(128,true,Object);break;
  case 0x71:case 0xf1:result();arg(128,false,Actor);break;
  case 0x34:case 0xf4:result();arg(128,true,Object);arg(64,true,Object);break;
  case 0x29:case 0xe9:arg(128,true,Object);arg(64,false,Owner);break;
  case 0x50:case 0xd0:arg(128,true,Object);break;
  case 0x0d:case 0xcd:arg(128,false,Actor);arg(64,false,Actor);byte();break;
  case 0x36:case 0xf6:arg(128,false,Actor);arg(64,true,Object);break;
  case 0x3b:case 0xbb:arg(128,false,Actor);break;
  case 0x15:case 0xd5:case 0x35:case 0xf5:result();arg(128,false);arg(64,false);break;
  case 0x19:case 0x59:case 0x79:case 0xf9:{
   unsigned verb=arg(128,false);if(verb==251||verb==252)break;
   arg(64,true,Object);arg(32,true,Object);if(byte()>2)bad("invalid sentence operation");break;
  }
  case 0x60:arg(128,true);break;
  case 0x70:case 0xf0:arg(128,false);byte();byte();break;
  case 0x7a:case 0xfa:{
   unsigned verb=byte();
   if(verb==0){unsigned slot=arg(128,false);if(slot!=UINT32_MAX&&slot>=15)bad("invalid verb slot");}
   else if(verb==255){byte();if(byte()>2)bad("invalid verb state");}
   else{byte();byte();unsigned slot=arg(128,false);if(slot!=UINT32_MAX&&slot>=15)bad("invalid verb slot");byte();text(false);}
   break;
  }
  case 0xd8:text(true);break;
  default:bad("unsupported v2 opcode or indirect reference");break;
  }
  next[at]=pos;
 }
 if(valid && last!=0x00 && last!=0xa0){bad("script has no final terminator");}
 // Check every branch before traversing the control-flow graph.
 for(size_t i=4;valid&&i<size;++i)if(boundary[i]){
  if((p[i]==0||p[i]==0xa0)&&i+1<size)bad("bytes after script terminator");
  if(targets[i]!=INT32_MIN && (targets[i]<4||targets[i]>=(int)size||!boundary[targets[i]]))bad("jump outside instruction boundaries");
 }
 // Acyclic backward branches (shared cleanup) are valid. Prove only the
 // original simple increment/test backedges; retain their fallthrough edges
 // when checking the rest of the CFG, so outer non-yielding cycles still fail.
 unsigned char *bounded=new unsigned char[size]();
 if(valid)for(unsigned i=4;i<size;++i)if(boundary[i]&&p[i]==0x44&&targets[i]>=4&&targets[i]<=(int)i&&p[i+2]<=254&&p[i+3]==0){
  unsigned v=p[i+1];bool increment=false,otherWrite=false,otherBackedge=false;
  for(int j=targets[i];j<(int)i;++j)if(boundary[j]){
   if(writes[j]==v){if(p[j]==0x46)increment=true;else otherWrite=true;}
   if(targets[j]!=INT32_MIN&&targets[j]<=(int)j)otherBackedge=true;
  }
  if(!increment||otherWrite||otherBackedge)continue;
  // Every path reaching the comparison from its target must increment v.
  unsigned char *reachable=new unsigned char[size]();reachable[targets[i]]=1;
  for(unsigned j=targets[i];j<i;++j)if(boundary[j]&&reachable[j]){
   if(p[j]==0x46&&writes[j]==v)continue;
   if(targets[j]>(int)j&&targets[j]<=(int)i)reachable[targets[j]]=1;
   if(p[j]!=0x18&&p[j]!=0x00&&p[j]!=0xa0&&p[j]!=0x4a&&p[j]!=0x98&&next[j]<=i)reachable[next[j]]=1;
  }
  bounded[i]=!reachable[i];delete[] reachable;
 }
 auto cuts=[&](unsigned i) {
  return p[i]==0x00||p[i]==0xa0||p[i]==0x4a||p[i]==0x98||(p[i]==0x62&&p[i+1]==0)||p[i]==0x80||p[i]==0x3b||p[i]==0xbb||p[i]==0x4c;
 };
 unsigned nodes=0,done=0,head=0,tail=0;
 if(valid){
  for(unsigned i=4;i<size;++i)if(boundary[i]){
   ++nodes;if(cuts(i))continue;
   if(targets[i]!=INT32_MIN&&!bounded[i])++indegree[targets[i]];
   if(p[i]!=0x18&&next[i]<size)++indegree[next[i]];
  }
  for(unsigned i=4;i<size;++i)if(boundary[i]&&!indegree[i])queue[tail++]=i;
  while(head<tail){
   unsigned i=queue[head++];++done;if(cuts(i))continue;
   if(targets[i]!=INT32_MIN&&!bounded[i]&&!--indegree[targets[i]])queue[tail++]=targets[i];
   if(p[i]!=0x18&&next[i]<size&&!--indegree[next[i]])queue[tail++]=next[i];
  }
  if(done!=nodes)bad("cycle without yield or bounded counter");
 }
 delete[] boundary;delete[] writes;delete[] targets;delete[] next;delete[] indegree;delete[] queue;delete[] bounded;
 if(!valid)*why=reason;
 return valid;
}
inline bool validExtensionActor(unsigned id, unsigned originalCount, unsigned capacityLimit, unsigned roomOwner) {
 return id >= originalCount && id < capacityLimit && id != roomOwner;
}
} // namespace Scumm
#endif
