import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { ErrorCenterProvider } from '../../src/components/ErrorCenter';
import { Button, ConfirmDialog, ErrorNotice } from '../../src/components/Primitives';
import '../../src/styles/global.css';

function Fixture() {
 const [show,setShow]=useState(false),[modal,setModal]=useState(false),[recovered,setRecovered]=useState(0);
 return <ErrorCenterProvider><input aria-label="Рабочее поле"/><button onClick={()=>setShow(true)}>Показать ошибку</button><button onClick={()=>setModal(true)}>Открыть диалог</button>
  <output aria-label="Повторы">{recovered}</output>
  {show&&<><ErrorNotice title="Ошибка записи" technicalDetails={'C:/'+ 'long-path/'.repeat(100)+'file.mp4'} action={<Button onClick={()=>setRecovered(value=>value+1)}>Повторить запись</Button>}>Не удалось записать видео.</ErrorNotice><ErrorNotice title="Ошибка записи" technicalDetails={'C:/'+ 'long-path/'.repeat(100)+'file.mp4'}>Не удалось записать видео.</ErrorNotice><ErrorNotice title="Ошибка логов">Не удалось сохранить архив.</ErrorNotice></>}
  <ConfirmDialog open={modal} title="Диалог проверки" confirmLabel="Вызвать ошибку" onConfirm={()=>setShow(true)} onCancel={()=>setModal(false)}><input aria-label="Поле диалога"/></ConfirmDialog>
 </ErrorCenterProvider>;
}
createRoot(document.getElementById('root')!).render(<Fixture/>);
