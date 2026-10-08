import {useTranslation} from '../i18n';
import {useRef,type PointerEvent,type KeyboardEvent} from 'react';
import type {Item} from './model';

export type TransformHandle = 'move'|'nw'|'ne'|'sw'|'se';
export function transformItem(item:Item,mode:TransformHandle,dx:number,dy:number,free=false):Item {
 if(mode==='move')return {...item,x:item.x+dx,y:item.y+dy};
 const west=mode.includes('w'),north=mode.includes('n');
 let width=Math.max(.01,item.width+(west?-dx:dx));
 let height=Math.max(.01,item.height+(north?-dy:dy));
 if(!free&&item.kind!=='text') {const scale=Math.abs(dx/item.width)>=Math.abs(dy/item.height)?width/item.width:height/item.height;width=item.width*scale;height=item.height*scale;}
 return {...item,width,height,x:west?item.x+item.width-width:item.x,y:north?item.y+item.height-height:item.y,fontSize:item.kind==='text'?Math.max(8,Math.min(500,item.fontSize*width/item.width)):item.fontSize};
}
export function PreviewTransform({item,selected,disabled,onSelect,onBegin,onChange,onEnd}:{item:Item;selected:boolean;disabled:boolean;onSelect:()=>void;onBegin:()=>void;onChange:(item:Item)=>void;onEnd:()=>void}) {
 const {t}=useTranslation();
 const drag=useRef<{x:number;y:number;width:number;height:number;item:Item;mode:TransformHandle;moved:boolean}|null>(null);
 function down(e:PointerEvent,mode:TransformHandle){if(disabled||e.button!==0)return;e.preventDefault();e.stopPropagation();const screen=e.currentTarget.closest('.ed-screen')!.getBoundingClientRect();onSelect();e.currentTarget.setPointerCapture(e.pointerId);drag.current={x:e.clientX,y:e.clientY,width:screen.width,height:screen.height,item,mode,moved:false};}
 function move(e:PointerEvent){const d=drag.current;if(!d)return;e.stopPropagation();if(!d.moved&&Math.hypot(e.clientX-d.x,e.clientY-d.y)<2)return;if(!d.moved){onBegin();d.moved=true;}onChange(transformItem(d.item,d.mode,(e.clientX-d.x)/d.width,(e.clientY-d.y)/d.height,e.shiftKey));}
 function end(){if(drag.current?.moved)onEnd();drag.current=null;}
 function key(e:KeyboardEvent,mode:TransformHandle){
  if(disabled||e.ctrlKey||e.metaKey)return;
  const direction=({ArrowLeft:[-1,0],ArrowRight:[1,0],ArrowUp:[0,-1],ArrowDown:[0,1]} as Record<string,number[]>)[e.key];
  if(!direction)return;
  e.preventDefault();e.stopPropagation();
  const screen=e.currentTarget.closest('.ed-screen')?.getBoundingClientRect();
  if(!screen||screen.width<=0||screen.height<=0)return;
  const step=e.shiftKey?10:1;
  onSelect();onBegin();
  onChange(transformItem(item,mode,direction[0]*step/screen.width,direction[1]*step/screen.height,e.altKey));
  onEnd();
 }
 const corners={nw:'левый верхний',ne:'правый верхний',sw:'левый нижний',se:'правый нижний'};
 return <div className={`ed-transform-target ${selected?'ed-transform-selected':''}`} role="group" tabIndex={disabled?-1:0} aria-disabled={disabled} title={t("Стрелки: переместить на 1 px; Shift: 10 px")} aria-label={t(item.kind==='text'?'Переместить текст':item.kind==='image'?'Переместить изображение':'Переместить видео')} style={{left:`${item.x*100}%`,top:`${item.y*100}%`,width:`${item.width*100}%`,height:`${item.height*100}%`}} onKeyDown={e=>key(e,'move')} onPointerDown={e=>down(e,'move')} onPointerMove={move} onPointerUp={end} onPointerCancel={end}>
 {selected&&(['nw','ne','sw','se'] as const).map(mode=><button type="button" disabled={disabled} key={mode} className={`ed-transform-handle ed-transform-${mode}`} aria-label={t('Масштабировать: {0} угол',t(corners[mode]))} title={t("Стрелки: размер; Shift: 10 px; Alt: свободные пропорции")} style={{left:`calc(${(mode.includes('w')?Math.max(0,-item.x):Math.min(item.width,1-item.x))/item.width*100}% ${mode.includes('w')?'+':'-'} 12px)`,top:`calc(${(mode.includes('n')?Math.max(0,-item.y):Math.min(item.height,1-item.y))/item.height*100}% ${mode.includes('n')?'+':'-'} 12px)`,right:'auto',bottom:'auto',transform:'translate(-50%,-50%)'}} onKeyDown={e=>key(e,mode)} onPointerDown={e=>down(e,mode)} onPointerMove={move} onPointerUp={e=>{e.stopPropagation();end();}} onPointerCancel={end}/>)}
 </div>;
}
