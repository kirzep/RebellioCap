export const blocks=[
 {id:'game',label:'Название игры',sample:'Cyberpunk 2077'},
 {id:'date',label:'Дата',sample:'2023.03.27'},
 {id:'time',label:'Время',sample:'13.30.59'},
 {id:'time_extended',label:'Расширенное время',sample:'13.30.59.62'},
 {id:'counter',label:'Счётчик',sample:'42'},
 {id:'type',label:'Тип записи',sample:'Replay'},
 {id:'resolution',label:'Разрешение',sample:'1920x1080'},
 {id:'fps',label:'FPS',sample:'60'},
 {id:'year',label:'Год',sample:'2023'},
 {id:'month',label:'Месяц',sample:'03'},
 {id:'day',label:'День',sample:'27'},
] as const;
export type Part={kind:'text'|'token';value:string};
export type Preset={id:string;name:string;parts:Part[]};
export type NamingSettings={activeId:string;presets:Preset[]};
const token=(value:string):Part=>({kind:'token',value}),text=(value:string):Part=>({kind:'text',value});
export function defaults():NamingSettings{return {activeId:'shadowplay',presets:[
 {id:'shadowplay',name:'NVIDIA ShadowPlay',parts:[token('game'),text(' '),token('date'),text(' - '),token('time_extended'),text('.DVR')]},
 {id:'rebcap',name:'RebellioCap',parts:[token('game'),text(' — '),token('date'),text(' '),token('time'),text(' — '),token('type'),text(' '),token('counter')]}
]};}
export function safeName(value:string){
 let name=value.replace(/[<>:"/\\|?*\u0000-\u001f]/g,'_').slice(0,160).replace(/[\uD800-\uDBFF]$/,'').replace(/[. ]+$/,'').replace(/^ +/,'')||'Clip';
 if(/^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)/i.test(name))name='_'+name;
 return name;
}
export function previewName(parts:Part[]){return safeName(parts.map(p=>p.kind==='text'?p.value:blocks.find(b=>b.id===p.value)?.sample??'').join(''))+'.mp4';}
export function readParts(root:Node):Part[]{
 const result:Part[]=[];
 function walk(node:Node){if(node instanceof HTMLElement&&node.dataset.token){if(blocks.some(b=>b.id===node.dataset.token))result.push({kind:'token',value:node.dataset.token});return;}if(node.nodeType===Node.TEXT_NODE){const value=(node.textContent??'').replace(/[\r\n\u00a0]/g,' ');if(value){const last=result.at(-1);if(last?.kind==='text')last.value+=value;else result.push({kind:'text',value});}return;}node.childNodes.forEach(walk);}
 root.childNodes.forEach(walk);return result;
}
