import {describe,it,expect} from 'vitest';
import {defaults,previewName,readParts,safeName} from './model';
describe('recording names',()=>{
 it('matches the ShadowPlay example',()=>expect(previewName(defaults().presets[0].parts)).toBe('Cyberpunk 2077 2023.03.27 - 13.30.59.62.DVR.mp4'));
 it('sanitizes Windows filenames and leaves literal braces alone',()=>{expect(safeName('../CON: / test* ')).toBe('.._CON_ _ test_');expect(safeName('CON')).toBe('_CON');expect(safeName('{custom}')).toBe('{custom}');expect(safeName('x'.repeat(200)).length).toBe(160);});
 it('reads text around an atomic block without duplicating the block label',()=>{const root=document.createElement('div');root.append(document.createTextNode('Мой '));const block=document.createElement('span');block.dataset.token='game';block.textContent='Название игры';root.append(block,document.createTextNode(' момент'));expect(readParts(root)).toEqual([{kind:'text',value:'Мой '},{kind:'token',value:'game'},{kind:'text',value:' момент'}]);});
});
