export type Asset = { id: string; path: string; name: string; kind: 'video'|'audio'|'image'; duration: number; width: number; height: number; audio: number[]; url?: string; previewDirectory?: string; missing?: boolean; audioUrls?: {path:string;label:string;enabled:boolean}[] };
export type Item = { id: string; asset?: string; kind: Asset['kind']|'text'; track: number; start: number; duration: number; source: number; stream: number; link?: string; x: number; y: number; width: number; height: number; opacity: number; gain: number; brightness: number; contrast: number; saturation: number; text: string; fontSize: number; color: string; fit: 'contain'|'cover' };
export type Project = { version: 1; name: string; width: number; height: number; fps: number; assets: Asset[]; items: Item[]; template?: boolean; recoveryId?: string };
export const uid = () => crypto.randomUUID();
export const emptyProject = (): Project => ({ version:1, name:'Новый монтаж', width:1920, height:1080, fps:60, assets:[], items:[] });
export const length = (p: Project) => Math.max(0, ...p.items.map(i=>i.start+i.duration));
export function newItem(kind: Item['kind'], track: number, start: number, duration: number): Item { return {id:uid(),kind,track,start,duration,source:0,stream:0,x:0,y:0,width:1,height:1,opacity:1,gain:1,brightness:0,contrast:1,saturation:1,text:'Ваш текст',fontSize:72,color:'#ffffff',fit:'contain'}; }
export function addAsset(p: Project, a: Asset, start=length(p)): Project {
 const track = Math.max(-1,...p.items.map(i=>i.track))+1;
 const link=uid(); const video={...newItem(a.kind,track,start,a.kind==='image'?5:a.duration),asset:a.id,link};
 if(a.kind!=='audio'&&a.width>0&&a.height>0){const scale=Math.min(p.width/a.width,p.height/a.height);video.width=a.width*scale/p.width;video.height=a.height*scale/p.height;video.x=(1-video.width)/2;video.y=(1-video.height)/2;}
 const audio=a.audio.map((stream,n)=>({...newItem('audio',track+1+n,start,a.duration),asset:a.id,stream,link,gain:a.audioUrls?.[n]?.enabled===false?0:1}));
 return {...p,assets:p.assets.some(v=>v.id===a.id)?p.assets:[...p.assets,a],items:[...p.items,video,...audio].filter(i=>!(a.kind==='audio' && i.id===video.id))};
}
export function linked(p: Project, id: string) { const i=p.items.find(i=>i.id===id); return p.items.filter(j=>j.id===id || (!!i?.link && j.link===i.link)); }
export function split(p: Project,id:string,time:number):Project { const ids=new Set(linked(p,id).map(i=>i.id)); const rightLink=uid(); return {...p,items:p.items.flatMap(i=>{ if(!ids.has(i.id)||time<=i.start+.02||time>=i.start+i.duration-.02)return [i]; const d=time-i.start; return [{...i,duration:d},{...i,id:uid(),link:i.link?rightLink:undefined,start:time,source:i.source+d,duration:i.duration-d}]; })}; }
export function remove(p: Project,id:string,ripple:boolean): Project { const group=linked(p,id); const selected=group.find(i=>i.id===id); if(!selected)return p; const ids=new Set(group.map(i=>i.id)); return {...p,items:p.items.filter(i=>!ids.has(i.id)).map(i=>ripple&&i.start>=selected.start+selected.duration-.001?{...i,start:Math.max(0,i.start-selected.duration)}:i)}; }
export function changeGroup(p: Project,id:string,mode:'move'|'left'|'right',delta:number):Project { const group=linked(p,id); if(!group.length)return p; const ids=new Set(group.map(i=>i.id)); const selected=group.find(i=>i.id===id)!;
 const maxSource=(i:Item)=>p.assets.find(a=>a.id===i.asset)?.duration??Infinity;
 let d=delta;
 if(mode==='move')d=Math.max(-Math.min(...group.map(i=>i.start)),d);
 if(mode==='left')d=Math.max(-selected.start,-selected.source,Math.min(selected.duration-.05,d));
 if(mode==='right')d=Math.max(.05-selected.duration,Math.min(...group.map(i=>i.kind==='text'||i.kind==='image'?Infinity:maxSource(i)-i.source-i.duration),d));
 return {...p,items:p.items.map(i=>!ids.has(i.id)?i: mode==='move'?{...i,start:i.start+d}:mode==='left'?{...i,start:i.start+d,source:i.source+d,duration:i.duration-d}:{...i,duration:i.duration+d})}; }
export function changeSource(p:Project,id:string,value:number):Project {const group=linked(p,id),selected=group.find(i=>i.id===id);if(!selected)return p;const delta=Math.max(-Math.min(...group.map(i=>i.source)),Math.min(value-selected.source,...group.map(i=>(p.assets.find(a=>a.id===i.asset)?.duration??Infinity)-i.source-i.duration)));const ids=new Set(group.map(i=>i.id));return {...p,items:p.items.map(i=>ids.has(i.id)?{...i,source:i.source+delta}:i)};}
export function exportPlan(p:Project, width:number,height:number,fps:number,bitrate:number,discord:boolean,priority:string,discordLimitMb=20) {
 if(![width,height,fps,bitrate].every(Number.isFinite)||width<16||height<16||width>7680||height>7680||fps<1||fps>240||bitrate<.064||bitrate>200)throw Error('Проверьте разрешение, FPS и битрейт.');
 const duration=length(p);let rate=bitrate*1e6;
 if(discord){if(![20,50,500].includes(discordLimitMb))throw Error('Выберите лимит Discord.');rate=Math.min(rate,Math.floor((discordLimitMb*1_000_000*.95-65536)*8/Math.max(duration,.1)-128000));if(rate<64000)throw Error(`Монтаж слишком длинный для ${discordLimitMb} МБ. Сократите его.`);
 const scales=[1,.75,.5,.375,.25],rates=[fps,Math.min(fps,30),Math.min(fps,24)];
 const choices:number[][]=priority==='motion'?scales.map(s=>[s,fps]):priority==='detail'?scales.flatMap(s=>rates.map(f=>[s,f])):[[1,fps],[.75,fps],[1,rates[1]],[.75,rates[1]],[.5,rates[1]],[.5,rates[2]],[.375,rates[2]],[.25,rates[2]]];
 const [scale,chosenFps]=choices.find(([s,f])=>rate/(width*height*s*s*f)>=.065)??choices.at(-1)!;
 width=Math.max(16,Math.floor(width*scale/2)*2);height=Math.max(16,Math.floor(height*scale/2)*2);fps=chosenFps;
 }
 return {width:Math.floor(width/2)*2,height:Math.floor(height/2)*2,fps,bitrate:Math.min(200_000_000,Math.max(64000,rate)),discord,discordLimitMb,duration};
}
export function reorderTrack(p:Project,from:number,to:number):Project {
 const tracks=[...new Set(p.items.map(i=>i.track))].sort((a,b)=>b-a);
 if(from===to||!tracks.includes(from)||!tracks.includes(to))return p;
 const ordered=[...tracks];ordered.splice(ordered.indexOf(from),1);ordered.splice(tracks.indexOf(to),0,from);
 const mapping=new Map(ordered.map((track,index)=>[track,tracks[index]]));
 return {...p,items:p.items.map(i=>({...i,track:mapping.get(i.track)!}))};
}
export function snapDelta(p:Project,id:string,mode:'move'|'left'|'right',delta:number,tolerance:number):{delta:number;at?:number} {
 const item=p.items.find(i=>i.id===id);if(!item||item.kind==='audio')return {delta};
 const excluded=new Set(linked(p,id).map(i=>i.id));
 const targets=p.items.filter(i=>i.track===item.track&&!excluded.has(i.id)).flatMap(i=>[i.start,i.start+i.duration]);
 const moving=mode==='left'?[item.start+delta]:mode==='right'?[item.start+item.duration+delta]:[item.start+delta,item.start+item.duration+delta];
 let best=tolerance,correction=0,at:number|undefined;
 for(const edge of moving)for(const target of targets){const distance=Math.abs(target-edge);if(distance<best){best=distance;correction=target-edge;at=target;}}
 return {delta:delta+correction,at};
}
export function duplicate(p:Project,id:string):Project {
 const group=linked(p,id);if(!group.length)return p;const offset=Math.max(...group.map(i=>i.start+i.duration))-Math.min(...group.map(i=>i.start));const link=uid();
 return {...p,items:[...p.items,...group.map(i=>({...i,id:uid(),link:i.link?link:undefined,start:i.start+offset}))]};
}
