/* Native SCUMM V2 extension inventory resources. GPL-3.0-or-later.
 * No per-item gameplay dispatcher: the original input, sentence, object,
 * inventory and opcode handlers execute the registered resource bytecode.
 */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "scumm/scumm_v2.h"
#include "scumm/resource.h"
#include "scumm/extension-validation.h"
#include "common/endian.h"

namespace Scumm {
void ScummEngine_v2::appbaseObjectsConfigure() {
    _appbaseObjects.clear();_appbaseOriginalObjects=_numGlobalObjects;
    if(_game.version!=2 || !(_game.features&GF_OLD_BUNDLE))return;
    const int count=EM_ASM_INT({return Module.scummObjects?.length||0;});
    if(count<0 || count>32 || _numGlobalObjects+count>1024)error("Invalid extension object count");
    if(!count)return;
    char identity[33]={};
    EM_ASM({const s=String(Module.scummObjectIdentity||'');if(/^[a-f0-9]{32}$/.test(s)){for(let i=0;i<32;++i)HEAPU8[$0+i]=s.charCodeAt(i);}},identity);
    _appbaseObjectIdentity=identity;if(_appbaseObjectIdentity.size()!=32)error("Missing native object identity");
    for(int i=0;i<count;++i){
        AppbaseObject o;
        o.id=EM_ASM_INT({return Module.scummObjects[$0].id;},i);
        o.catalogId=EM_ASM_INT({return Module.scummObjects[$0].catalogId;},i);
        o.initialOwner=EM_ASM_INT({return Module.scummObjects[$0].initialOwner;},i);
        o.document=EM_ASM_INT({return Module.scummObjects[$0].services.includes('document')?1:0;},i)!=0;
        const int size=EM_ASM_INT({return Module.scummObjects[$0].resource?.length||0;},i);
        if(o.id!=_appbaseOriginalObjects+i || o.catalogId<1 || o.initialOwner<1 || o.initialOwner==OF_OWNER_ROOM || o.initialOwner>=95 || size<18 || size>4096)error("Invalid native object binding");
        for(uint j=0;j<_appbaseObjects.size();++j)if(_appbaseObjects[j].catalogId==o.catalogId)error("Duplicate extension object key");
        o.bytes.resize(size);EM_ASM({HEAPU8.set(Module.scummObjects[$0].resource,$1);},i,o.bytes.data());
        if(READ_LE_UINT16(o.bytes.data())!=size || READ_LE_UINT16(o.bytes.data()+4)!=o.id)error("Native object resource does not match binding");
        const unsigned name=o.bytes[14];
        if(name<18 || name>=o.bytes.size() || o.bytes.back()!=0)error("Invalid native object name/header");
        unsigned p=15;
        while(p<name && o.bytes[p]){if(p+1>=name || o.bytes[p+1]<=name || o.bytes[p+1]>=size)error("Invalid native verb entry");o.entries.push_back(o.bytes[p+1]);p+=2;}
        if(p!=name-1 || o.bytes[p]!=0 || o.entries.empty())error("Malformed native object verb table");
        unsigned end=name;while(end<o.bytes.size() && o.bytes[end])++end;
        if(end>=o.bytes.size() || end-name>80 || o.entries[0]!=end+1)error("Invalid native object name extent");
        for(unsigned j=0;j<end-name;++j)o.name[j]=o.bytes[name+j];
        _appbaseObjects.push_back(o);
    }
    _numGlobalObjects+=count;
}

void ScummEngine_v2::appbaseItemsReset() {
    _appbaseGivingItem=0;
    if(_appbaseObjects.empty())return;
    bool actors[256]={};for(unsigned i=1;i<_numActors;++i)actors[i]=!_appbaseExtensions.active() || _appbaseExtensions.isActor(i);
    ExtensionV2Limits limits={_numActors,(unsigned)_numRooms,(unsigned)_numScripts,(unsigned)_numGlobalObjects,(unsigned)_numCostumes,(unsigned)_numVariables,OF_OWNER_ROOM,actors,(unsigned)_numSounds,nullptr,nullptr,nullptr,nullptr};
    for(uint i=0;i<_appbaseObjects.size();++i){
        AppbaseObject &o=_appbaseObjects[i];
        if(o.initialOwner>=_numActors || !actors[o.initialOwner])error("Extension object owner is not registered");
        for(uint j=0;j<o.entries.size();++j){
            const unsigned start=o.entries[j],end=j+1<o.entries.size()?o.entries[j+1]:o.bytes.size();
            if(end<=start)error("Unordered native object entries");
            Common::Array<byte> script;script.resize(4+end-start);WRITE_LE_UINT16(script.data(),script.size());script[2]=script[3]=0;
            memcpy(script.data()+4,o.bytes.data()+start,end-start);
            const char *why=nullptr;if(!validateExtensionV2(script.data(),script.size(),limits,&why))error("Invalid extension object script: %s",why);
        }
        addObjectToInventory(o.id,0);putOwner(o.id,o.initialOwner);putState(o.id,11);
    }
}

const byte *ScummEngine_v2::appbaseObjectResource(int id) const {
    for(uint i=0;i<_appbaseObjects.size();++i)if(_appbaseObjects[i].id==id)return _appbaseObjects[i].bytes.data();
    return nullptr;
}
bool ScummEngine_v2::appbaseObjectLocal(uint var) const {
    return var>=248 && var<256 && _currentScript!=0xff && vm.slot[_currentScript].where==WIO_INVENTORY && appbaseObjectResource(vm.slot[_currentScript].number);
}
const byte *ScummEngine_v2::appbaseItemName(int id) {
    for(uint i=0;i<_appbaseObjects.size();++i)if(_appbaseObjects[i].id==id){
        AppbaseObject &o=_appbaseObjects[i];
        EM_ASM({const item=Module.scummObjects.find(x=>x.id===$0);const s=item?.names?.[$2?'de':'en'];if(typeof s==='string'){let i=0;for(;i<80&&i<s.length;++i)HEAPU8[$1+i]=s.charCodeAt(i)<=255?s.charCodeAt(i):63;HEAPU8[$1+i]=0;}},id,o.name,appbaseGerman() || _language==Common::DE_DEU);
        return o.name;
    }
    return nullptr;
}
int ScummEngine_v2::appbaseItem(int op,int id,int value) {
    // Catalog-facing document APIs are aliases onto native ownership, never a
    // second inventory. Counts/positions always use the original engine list.
    if(op==0 || op==1)return 0;
    if(op==4)return _appbaseObjects.size();
    if(op==5)return id>=0 && id<(int)_appbaseObjects.size()?_appbaseObjects[id].catalogId:0;
    for(uint i=0;i<_appbaseObjects.size();++i){const AppbaseObject &o=_appbaseObjects[i];if(o.id!=id && o.catalogId!=id)continue;
        if(op==2)return getOwner(o.id);
        if(op==3 && value>=0 && value<_numActors && (value==0 || !_appbaseExtensions.active() || _appbaseExtensions.isActor(value))){putOwner(o.id,value);return 1;}
    }
    return 0;
}
void ScummEngine_v2::o2_extensionService() {
    if(_currentScript==0xff || vm.slot[_currentScript].where!=WIO_INVENTORY || !appbaseObjectResource(vm.slot[_currentScript].number)){o2_dummy();return;}
    const unsigned service=fetchScriptByte(),object=fetchScriptWord();
    if(_currentScript==0xff || vm.slot[_currentScript].where!=WIO_INVENTORY || vm.slot[_currentScript].number!=object)error("Host service outside registered object script");
    for(uint i=0;i<_appbaseObjects.size();++i){const AppbaseObject &o=_appbaseObjects[i];if(o.id!=object)continue;
        if(service!=1 || !o.document || getOwner(object)!=VAR(VAR_EGO))error("Object host service denied");
        EM_ASM({Module.scummObjectService?.('document',$0);},o.catalogId);return;
    }
    error("Unknown extension object host service");
}
void ScummEngine_v2::appbaseObjectsSave(Common::WriteStream &out) const {
    if(_appbaseObjects.empty())return;
    out.writeUint32BE(MKTAG('A','B','N','O'));out.writeUint32LE(1);out.writeUint32LE(_numGlobalObjects);out.write(_appbaseObjectIdentity.c_str(),32);
}
bool ScummEngine_v2::appbaseObjectsAcceptSave(Common::SeekableReadStream &in) const {
    if(_appbaseObjects.empty())return true;
    const int64 pos=in.pos();const int64 end=in.size()-(_appbaseExtensions.active()?44+_appbaseExtensions.stateTrailerSize():0);bool ok=false;
    if(end>=44){in.seek(end-44);const uint32 magic=in.readUint32BE(),version=in.readUint32LE(),objects=in.readUint32LE();char id[33]={};in.read(id,32);ok=magic==MKTAG('A','B','N','O') && version==1 && objects==(uint32)_numGlobalObjects && _appbaseObjectIdentity==id;}
    in.seek(pos);return ok;
}
} // namespace Scumm
#endif
