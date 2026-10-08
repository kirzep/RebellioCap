import {useEffect,useLayoutEffect,useRef,useState} from 'react';
export type MenuAction={label:string;disabled?:boolean;danger?:boolean;run:()=>void};
export function TimelineMenu({x,y,actions,onClose}:{x:number;y:number;actions:MenuAction[];onClose:()=>void}) {
 const ref=useRef<HTMLDivElement>(null);const [position,setPosition]=useState({x,y});
 useLayoutEffect(()=>{const rect=ref.current!.getBoundingClientRect();setPosition({x:Math.max(8,Math.min(x,window.innerWidth-rect.width-8)),y:Math.max(8,Math.min(y,window.innerHeight-rect.height-8))});ref.current?.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus();},[x,y]);
 useEffect(()=>{const key=(e:KeyboardEvent)=>{if(e.key==='Escape'){e.preventDefault();e.stopImmediatePropagation();onClose();}};window.addEventListener('keydown',key,true);return()=>window.removeEventListener('keydown',key,true);},[onClose]);
 return <div className="ed-context-backdrop" onPointerDown={onClose} onContextMenu={e=>{e.preventDefault();onClose();}}>
 <div ref={ref} className="ed-context-menu" role="menu" aria-label="Действия с фрагментом" style={{left:position.x,top:position.y}} onPointerDown={e=>e.stopPropagation()} onContextMenu={e=>{e.preventDefault();e.stopPropagation();}} onKeyDown={e=>{if(e.key==='ArrowUp'||e.key==='ArrowDown'){e.preventDefault();e.stopPropagation();const buttons=Array.from(ref.current!.querySelectorAll<HTMLButtonElement>('button:not(:disabled)'));const index=buttons.indexOf(document.activeElement as HTMLButtonElement);buttons[(index+(e.key==='ArrowUp'?-1:1)+buttons.length)%buttons.length]?.focus();}}}>
 {actions.map(action=><button key={action.label} role="menuitem" disabled={action.disabled} className={action.danger?'ed-menu-danger':''} onClick={()=>{onClose();action.run();}}>{action.label}</button>)}
 </div></div>;
}
