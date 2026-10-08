import {useRef} from 'react';

/** Clip-wide level control: vertical dragging, one undo step per gesture. */
export function TimelineLevel({value,label,disabled,onSelect,onBegin,onChange,onEnd,onCommit}:{
 value:number;label:string;disabled:boolean;onSelect:()=>void;onBegin:()=>void;onChange:(value:number)=>void;onEnd:()=>void;onCommit:(value:number)=>void;
}) {
 const gesture=useRef<{top:number;height:number;originY:number;moved:boolean}|null>(null);
 function move(y:number){const g=gesture.current;if(g)onChange(Math.max(0,Math.min(1,1-(y-g.top-12)/Math.max(1,g.height-24))));}
 return <button className="ed-level" role="slider" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(value*100)} aria-valuetext={`${Math.round(value*100)}%`} aria-orientation="vertical" disabled={disabled}
 style={{top:`calc(${(1-value)*100}% + ${(2*value-1)*12}px)`}}
 onPointerDown={e=>{e.stopPropagation();e.preventDefault();if(disabled||e.button!==0)return;const parent=e.currentTarget.parentElement!;const box=parent.getBoundingClientRect();gesture.current={top:box.top+parent.clientTop,height:parent.clientHeight,originY:e.clientY,moved:false};e.currentTarget.setPointerCapture(e.pointerId);e.currentTarget.focus();onSelect();}}
 onPointerMove={e=>{e.stopPropagation();const g=gesture.current;if(g){if(!g.moved&&Math.abs(e.clientY-g.originY)>2){g.moved=true;onBegin();}if(g.moved)move(e.clientY);}}}
 onPointerUp={e=>{e.stopPropagation();const g=gesture.current;if(g){if(g.moved){move(e.clientY);onEnd();}gesture.current=null;}}}
 onPointerCancel={e=>{e.stopPropagation();if(gesture.current?.moved)onEnd();gesture.current=null;}}
 onKeyDown={e=>{const step=e.shiftKey?.1:.01;const next=e.key==='ArrowUp'||e.key==='ArrowRight'?value+step:e.key==='ArrowDown'||e.key==='ArrowLeft'?value-step:e.key==='Home'?0:e.key==='End'?1:undefined;if(next!==undefined){e.preventDefault();e.stopPropagation();onCommit(Math.max(0,Math.min(1,next)));}}}
 title={`${label}: ${Math.round(value*100)}%. Потяните линию вверх или вниз.`}>
 <span className="ed-level-line"/><span className="ed-level-value">{Math.round(value*100)}%</span>
 </button>;
}
