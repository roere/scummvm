/* Bounds validation only; native CostumeLoader/Renderer remain authoritative. */
#ifndef SCUMM_EXTENSION_COSTUME_VALIDATION_H
#define SCUMM_EXTENSION_COSTUME_VALIDATION_H
#include <stddef.h>
#include <stdint.h>
namespace Scumm {
inline bool validateExtensionCostumeV2(const unsigned char *p, size_t size, const char **why) {
 #define COST_BAD(msg) do { *why=msg; return false; } while(0)
 if (!p || size<43 || size>UINT16_MAX) COST_BAD("invalid costume size");
 auto word=[p,size](size_t o)->unsigned { return o+1<size ? (unsigned)(p[o]|p[o+1]<<8) : UINT32_MAX; };
 if(word(0)!=size || (p[5]&127)!=0x58 || p[6]>=16) COST_BAD("invalid V2 Amiga costume header");
 const unsigned commands=word(7), tableEnd=41+2*(p[4]+1);
 unsigned tables[16],firstTable=UINT32_MAX;
 for(unsigned l=0;l<16;++l){tables[l]=word(9+2*l);if(tables[l])if(tables[l]<firstTable)firstTable=tables[l];}
 if(tableEnd>commands || commands>=firstTable || firstTable>=size) COST_BAD("invalid costume tables");
 bool refs[16][128]={},any=false;
 for(unsigned a=0;a<=p[4];++a){
  unsigned o=word(41+2*a);if(!o)continue;
  if(o<tableEnd || o+2>commands) COST_BAD("invalid animation descriptor");
  const unsigned mask=word(o);o+=2;
  for(unsigned l=0;l<16;++l)if(mask&(0x8000>>l)){
   if(o>=commands) COST_BAD("truncated animation descriptor");
   const unsigned start=p[o++];if(start==255)continue;
   if(o>=commands) COST_BAD("truncated animation range");
   const unsigned flags=p[o++],end=start+(flags&127);
   if(commands+end>=firstTable) COST_BAD("animation range overlaps frame tables");
   if(p[commands+start]==0x79 || p[commands+start]==0x7a)continue;
   // increaseAnim skips raw counter commands 78/7c. A whole loop of
   // counters, or a held final counter, never returns to the engine.
   bool hasReturn=false;
   for(unsigned j=start;j<=end;++j)if(p[commands+j]!=0x78 && p[commands+j]!=0x7c)hasReturn=true;
   const bool heldCounter=(flags&128) && (p[commands+end]==0x78 || p[commands+end]==0x7c);
   if(start!=end && (!hasReturn || heldCounter)) COST_BAD("nonterminating animation sequence");
   for(unsigned j=start;j<=end;++j){
    const unsigned raw=p[commands+j],code=raw&127;
    if(code>0x7c) COST_BAD("unsupported animation command");
    // Raw counters are skipped during advancement, but the initial command
    // is drawn before advancement. Other masked codes reach drawLimb too.
    const bool counter=raw==0x78 || raw==0x7c;
    if(code!=0x7b && (!counter || j==start)){refs[l][code]=true;any=true;}
   }
  }
 }
 if(!any) COST_BAD("costume has no graphic references");
 unsigned firstCel=UINT32_MAX;
 for(unsigned l=0;l<16;++l)for(unsigned code=0;code<128;++code)if(refs[l][code]){
  const unsigned table=tables[l],o=table+2*code;
  if(!table || o+2>size) COST_BAD("invalid frame reference");
  const unsigned cel=word(o);if(!cel || cel>=size) COST_BAD("empty or invalid referenced cel");
  if(cel<firstCel)firstCel=cel;
 }
 if(firstCel<=firstTable || firstCel>=size) COST_BAD("invalid first cel");
 struct CelStarts { unsigned char *data; explicit CelStarts(size_t n):data(new unsigned char[n]()){} ~CelStarts(){delete[] data;} } cels(size);
 for(unsigned l=0;l<16;++l){
  const unsigned begin=tables[l];if(!begin)continue;
  if(begin<firstTable || begin>firstCel) COST_BAD("invalid limb table");
  unsigned end=firstCel;
  for(unsigned t:tables)if(t>begin && t<end)end=t;
  if((end-begin)&1) COST_BAD("unaligned limb table");
  for(unsigned code=0;code<128;++code)if(refs[l][code] && begin+2*code>=end) COST_BAD("reference outside limb table");
  for(unsigned o=begin;o<end;o+=2){
   const unsigned cel=word(o);if(!cel)continue;
   if(cel<firstCel || cel>=size) COST_BAD("invalid cel pointer");
   cels.data[cel]=1;
  }
 }
 for(unsigned begin=firstCel;begin<size;){
  if(!cels.data[begin]){++begin;continue;}
  unsigned limit=begin+1;while(limit<size && !cels.data[limit])++limit;
  if(begin+12>limit) COST_BAD("truncated cel header");
  const unsigned w=word(begin),h=word(begin+2);
  if(!w || w>320 || !h || h>200) COST_BAD("invalid cel dimensions");
  unsigned o=begin+12,pixels=0;
  while(pixels<w*h){
   if(o>=limit) COST_BAD("truncated cel RLE");
   unsigned n=p[o++]&15;
   if(!n){if(o>=limit) COST_BAD("truncated RLE run");n=p[o++];if(!n)n=256;}
   pixels+=n;
  }
  begin=limit;
 }
 return true;
 #undef COST_BAD
}
}
#endif

