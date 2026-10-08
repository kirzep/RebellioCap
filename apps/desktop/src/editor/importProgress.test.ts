import {beforeEach,afterEach,it,expect,vi} from 'vitest';
import {subscribeImportProgress,formatImportProgress} from './importProgress';
const native=vi.hoisted(()=>({listener:undefined as ((event:{payload:unknown})=>void)|undefined,release:vi.fn(),listen:vi.fn()}));
vi.mock('@tauri-apps/api/webviewWindow',()=>({getCurrentWebviewWindow:()=>({listen:native.listen})}));
beforeEach(()=>{Object.defineProperty(window,'__TAURI_INTERNALS__',{value:{},configurable:true});native.listen.mockImplementation(async (_event:string,listener:typeof native.listener)=>{native.listener=listener;return native.release;});});
afterEach(()=>{delete (window as unknown as Record<string,unknown>).__TAURI_INTERNALS__;vi.resetAllMocks();});
it('formats real stages and file counts without a fabricated percentage',()=>{
 expect(formatImportProgress({owner:'one',operationId:'op',phase:'preparing_preview',completed:1,total:3,filename:'clip.mp4'})).toBe('Готовим превью · исходник 2 из 3 · clip.mp4');
 expect(formatImportProgress({owner:'one',operationId:'op',phase:'selecting',completed:0,total:0})).toBe('Выберите файл в системном диалоге');
});
it('accepts only valid progress for the current owner and operation',async()=>{
 const update=vi.fn();const release=await subscribeImportProgress('one','op',update);
 const event={owner:'one',operationId:'op',phase:'analyzing',completed:0,total:1,filename:'clip.mp4'};
 for(const payload of [{...event,owner:'other'},{...event,operationId:'old'},{...event,phase:'wrong'},{...event,total:-1},{...event,completed:2},{...event,total:201},null])native.listener?.({payload});
 expect(update).not.toHaveBeenCalled();
 native.listener?.({payload:event});expect(update).toHaveBeenCalledWith(event);
 release();native.listener?.({payload:event});expect(update).toHaveBeenCalledTimes(1);expect(native.release).toHaveBeenCalledOnce();
});
it('removes a late listener if the subscription is released before it resolves',async()=>{
 let registered!:(release:()=>void)=>void;
 native.listen.mockImplementationOnce(()=>new Promise(resolve=>{registered=resolve;}));
 let active=true;
 const update=vi.fn();const pending=subscribeImportProgress('one','op',update,()=>active);
 active=false;
 const releaseNative=vi.fn();registered(releaseNative);const release=await pending;release();expect(releaseNative).toHaveBeenCalledOnce();expect(update).not.toHaveBeenCalled();
});
