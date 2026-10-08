import {getCurrentWebviewWindow} from '@tauri-apps/api/webviewWindow';

const phases={selecting:'Выберите файл в системном диалоге',reading_project:'Читаем проект',analyzing:'Проверяем медиа',checking_cache:'Проверяем исходник и кэш',preparing_preview:'Готовим превью',publishing:'Завершаем подготовку'};
export type ImportProgress={owner:string;operationId:string;phase:keyof typeof phases;completed:number;total:number;filename?:string};
function valid(value:unknown):value is ImportProgress {
 if(!value||typeof value!=='object')return false;
 const p=value as ImportProgress;
 return typeof p.owner==='string'&&typeof p.operationId==='string'&&typeof p.phase==='string'&&Object.hasOwn(phases,p.phase)
  &&Number.isInteger(p.total)&&p.total>=0&&p.total<=200&&Number.isInteger(p.completed)&&p.completed>=0&&p.completed<=p.total
  &&(p.filename===undefined||typeof p.filename==='string'&&p.filename.length<=512);
}
export function formatImportProgress(p:ImportProgress):string {
 return [phases[p.phase],p.total>0&&p.completed<p.total?`исходник ${p.completed+1} из ${p.total}`:undefined,p.filename].filter(Boolean).join(' · ');
}
export async function subscribeImportProgress(owner:string,operationId:string,onProgress:(progress:ImportProgress)=>void,isActive:()=>boolean=()=>true):Promise<()=>void> {
 if(!('__TAURI_INTERNALS__' in window))return ()=>{};
 let disposed=false;
 const unlisten=await getCurrentWebviewWindow().listen<unknown>('editor-import-progress',event=>{
  if(!disposed&&isActive()&&valid(event.payload)&&event.payload.owner===owner&&event.payload.operationId===operationId)onProgress(event.payload);
 });
 if(!isActive()){disposed=true;unlisten();return ()=>{};}
 return ()=>{if(!disposed){disposed=true;unlisten();}};
}
