from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence, Tuple

GEN_NAMES = {
    0:"startAddrsOffset",1:"endAddrsOffset",2:"startloopAddrsOffset",3:"endloopAddrsOffset",4:"startAddrsCoarseOffset",
    5:"modLfoToPitch",6:"vibLfoToPitch",7:"modEnvToPitch",8:"initialFilterFc",9:"initialFilterQ",
    10:"modLfoToFilterFc",11:"modEnvToFilterFc",12:"endAddrsCoarseOffset",13:"modLfoToVolume",
    15:"chorusEffectsSend",16:"reverbEffectsSend",17:"pan",21:"delayModLFO",22:"freqModLFO",23:"delayVibLFO",
    24:"freqVibLFO",25:"delayModEnv",26:"attackModEnv",27:"holdModEnv",28:"decayModEnv",29:"sustainModEnv",
    30:"releaseModEnv",31:"keynumToModEnvHold",32:"keynumToModEnvDecay",33:"delayVolEnv",34:"attackVolEnv",
    35:"holdVolEnv",36:"decayVolEnv",37:"sustainVolEnv",38:"releaseVolEnv",39:"keynumToVolEnvHold",
    40:"keynumToVolEnvDecay",41:"instrument",43:"keyRange",44:"velRange",45:"startloopAddrsCoarseOffset",
    46:"keynum",47:"velocity",48:"initialAttenuation",50:"endloopAddrsCoarseOffset",51:"coarseTune",
    52:"fineTune",53:"sampleID",54:"sampleModes",56:"scaleTuning",57:"exclusiveClass",58:"overridingRootKey",60:"endOper",
}
SAMPLE_TYPE_NAMES={1:"monoSample",2:"rightSample",4:"leftSample",8:"linkedSample"}


def align4(n:int)->int: return (n+3)&~3

def cstr(raw:bytes)->str: return raw.split(b"\0",1)[0].decode("latin-1",errors="replace").rstrip()

class Sf2Error(ValueError): pass

@dataclass(frozen=True)
class Chunk:
    id:str; size:int; offset:int; list_type:Optional[str]=None; children:Tuple["Chunk",...]=()
@dataclass(frozen=True)
class Bag: gen:int; mod:int
@dataclass(frozen=True)
class Gen:
    oper:int; amount:int
    @property
    def name(self): return GEN_NAMES.get(self.oper,f"oper_{self.oper}")
    def rng(self): return (self.amount&0xff,(self.amount>>8)&0xff)
@dataclass(frozen=True)
class Preset: name:str; program:int; bank:int; bag:int
@dataclass(frozen=True)
class Inst: name:str; bag:int
@dataclass(frozen=True)
class Sample:
    name:str; start:int; end:int; start_loop:int; end_loop:int; rate:int; root:int; correction:int; link:int; type:int
    @property
    def pcm_bytes(self): return max(0,self.end-self.start)*2
    @property
    def base_type(self): return self.type&0x7fff
    @property
    def is_rom(self): return bool(self.type&0x8000)

class Sf2:
    def __init__(self,path:Path):
        self.path=Path(path); self.data=self.path.read_bytes(); self.file_size=len(self.data); self.chunks=[]; self.index={}
        self.phdr=[]; self.pbag=[]; self.pmod=[]; self.pgen=[]; self.inst=[]; self.ibag=[]; self.imod=[]; self.igen=[]; self.shdr=[]
        self.smpl_bytes=0

    def parse(self):
        d=self.data
        if len(d)<12 or d[:4]!=b"RIFF": raise Sf2Error("not a RIFF file")
        if d[8:12]!=b"sfbk": raise Sf2Error("RIFF form is not sfbk")
        end=min(len(d),8+struct.unpack_from("<I",d,4)[0])
        self.chunks=self._chunks(12,end)
        for c in self.chunks:
            if c.id=="LIST" and c.list_type:
                for x in c.children: self.index[(c.list_type,x.id)]=x
        self._pdta(); self.smpl_bytes=self._chunk("sdta","smpl").size
        return self

    def _chunks(self,start:int,end:int):
        out=[]; off=start
        while off+8<=end:
            cid=self.data[off:off+4].decode("latin-1"); size=struct.unpack_from("<I",self.data,off+4)[0]; pos=off+8; stop=pos+size
            if stop>end or stop>len(self.data): raise Sf2Error(f"chunk {cid} exceeds parent/file bounds")
            if cid=="LIST":
                if size<4: raise Sf2Error("LIST chunk too small")
                typ=self.data[pos:pos+4].decode("latin-1"); out.append(Chunk(cid,size,pos,typ,tuple(self._chunks(pos+4,stop))))
            else: out.append(Chunk(cid,size,pos))
            off=stop+(size&1)
        return out

    def _chunk(self,group:str,cid:str):
        try: return self.index[(group,cid)]
        except KeyError as e: raise Sf2Error(f"missing {group}/{cid} chunk") from e
    def _raw(self,group,cid):
        c=self._chunk(group,cid); return self.data[c.offset:c.offset+c.size]
    @staticmethod
    def _multiple(raw,size,name):
        if len(raw)%size: raise Sf2Error(f"{name} length {len(raw)} is not a multiple of {size}")

    def _pdta(self):
        raw=self._raw("pdta","phdr"); self._multiple(raw,38,"phdr")
        self.phdr=[Preset(cstr(raw[i:i+20]),*struct.unpack_from("<HHH",raw,i+20)) for i in range(0,len(raw),38)]
        raw=self._raw("pdta","pbag"); self._multiple(raw,4,"pbag"); self.pbag=[Bag(*struct.unpack_from("<HH",raw,i)) for i in range(0,len(raw),4)]
        raw=self._raw("pdta","pmod"); self._multiple(raw,10,"pmod"); self.pmod=[struct.unpack_from("<HHhHH",raw,i) for i in range(0,len(raw),10)]
        raw=self._raw("pdta","pgen"); self._multiple(raw,4,"pgen"); self.pgen=[Gen(*struct.unpack_from("<HH",raw,i)) for i in range(0,len(raw),4)]
        raw=self._raw("pdta","inst"); self._multiple(raw,22,"inst"); self.inst=[Inst(cstr(raw[i:i+20]),struct.unpack_from("<H",raw,i+20)[0]) for i in range(0,len(raw),22)]
        raw=self._raw("pdta","ibag"); self._multiple(raw,4,"ibag"); self.ibag=[Bag(*struct.unpack_from("<HH",raw,i)) for i in range(0,len(raw),4)]
        raw=self._raw("pdta","imod"); self._multiple(raw,10,"imod"); self.imod=[struct.unpack_from("<HHhHH",raw,i) for i in range(0,len(raw),10)]
        raw=self._raw("pdta","igen"); self._multiple(raw,4,"igen"); self.igen=[Gen(*struct.unpack_from("<HH",raw,i)) for i in range(0,len(raw),4)]
        raw=self._raw("pdta","shdr"); self._multiple(raw,46,"shdr"); self.shdr=[]
        for i in range(0,len(raw),46):
            r=raw[i:i+46]; vals=struct.unpack_from("<IIIIIBbHH",r,20)
            self.shdr.append(Sample(cstr(r[:20]),*vals))
        if not self.phdr or not self.inst or not self.shdr: raise Sf2Error("missing PDTA terminal records")

    @staticmethod
    def _gens(bags:Sequence[Bag],gens:Sequence[Gen],zi:int):
        if zi+1>=len(bags): return ()
        a,b=bags[zi].gen,bags[zi+1].gen
        if a>b or b>len(gens): raise Sf2Error("invalid bag generator index range")
        return gens[a:b]
    @staticmethod
    def _range(gens:Sequence[Gen],op:int):
        for g in gens:
            if g.oper==op:
                lo,hi=g.rng(); return max(0,min(127,lo)),max(0,min(127,hi))
        return 0,127
    @staticmethod
    def _idx(gens:Sequence[Gen],op:int):
        out=None
        for g in gens:
            if g.oper==op: out=g.amount
        return out
    @staticmethod
    def _ix(a,b):
        lo,hi=max(a[0],b[0]),min(a[1],b[1]); return (lo,hi) if lo<=hi else None

    def _preset_zones(self,pi): return range(self.phdr[pi].bag,self.phdr[pi+1].bag) if pi+1<len(self.phdr) else range(0)
    def _inst_zones(self,ii): return range(self.inst[ii].bag,self.inst[ii+1].bag) if ii+1<len(self.inst) else range(0)

    def resolve_preset(self,pi:int):
        p=self.phdr[pi]; pglobal=(); plocal=[]
        for zi in self._preset_zones(pi):
            gs=self._gens(self.pbag,self.pgen,zi); iid=self._idx(gs,41)
            if iid is None: pglobal=gs
            else: plocal.append((gs,iid))
        out=[]; samples=set()
        for pgs,iid in plocal:
            if iid>=len(self.inst)-1: continue
            pk=self._ix(self._range(pglobal,43),self._range(pgs,43)); pv=self._ix(self._range(pglobal,44),self._range(pgs,44))
            if not pk or not pv: continue
            iglobal=(); ilocal=[]
            for zi in self._inst_zones(iid):
                gs=self._gens(self.ibag,self.igen,zi); sid=self._idx(gs,53)
                if sid is None: iglobal=gs
                else: ilocal.append((gs,sid))
            for igs,sid in ilocal:
                if sid>=len(self.shdr)-1: continue
                ik=self._ix(self._range(iglobal,43),self._range(igs,43)); iv=self._ix(self._range(iglobal,44),self._range(igs,44))
                if not ik or not iv: continue
                k=self._ix(pk,ik); v=self._ix(pv,iv)
                if not k or not v: continue
                out.append({"instrument_id":iid,"sample_id":sid,"key_range":list(k),"vel_range":list(v)}); samples.add(sid)
        overlap=self._max_overlap(out); raw=sum(self.shdr[i].pcm_bytes for i in samples); aligned=sum(align4(self.shdr[i].pcm_bytes) for i in samples)
        return {"preset_index":pi,"name":p.name,"bank":p.bank,"program":p.program,"resolved_zone_count":len(out),"unique_sample_count":len(samples),
                "sample_ids":sorted(samples),"pcm_bytes":raw,"aligned_pcm_bytes":aligned,"max_overlapping_zones":overlap[0],
                "max_overlap_at":{"key":overlap[1],"velocity":overlap[2]}}

    @staticmethod
    def _max_overlap(zones):
        if not zones: return 0,0,0
        keys={0,127}; vels={0,127}
        for z in zones: keys.update(z["key_range"]); vels.update(z["vel_range"])
        best=(0,0,0)
        for k in keys:
            for v in vels:
                n=sum(z["key_range"][0]<=k<=z["key_range"][1] and z["vel_range"][0]<=v<=z["vel_range"][1] for z in zones)
                if n>best[0]: best=(n,k,v)
        return best

    def generator_usage(self,bags,gens):
        out={}
        for zi in range(max(0,len(bags)-1)):
            for g in self._gens(bags,gens,zi): out[g.name]=out.get(g.name,0)+1
        return dict(sorted(out.items()))

    @staticmethod
    def nonzero_mods(mods):
        out=[]
        for i,(src,dest,amount,asrc,transform) in enumerate(mods):
            if any((src,dest,amount,asrc,transform)):
                out.append({"index":i,"src_oper":src,"dest_oper":dest,"dest_name":GEN_NAMES.get(dest,f"oper_{dest}"),"amount":amount,
                            "amount_src_oper":asrc,"transform_oper":transform})
        return out

    def chunk_summary(self):
        def one(c):
            x={"id":c.id,"size":c.size}
            if c.list_type: x["list_type"]=c.list_type
            if c.children: x["children"]=[one(y) for y in c.children]
            return x
        return [one(c) for c in self.chunks]
