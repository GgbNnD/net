(function(){'use strict';
const API='/api';
var devs=[],msg={},selected='',selfName='';

function init(){
  refreshDevices();setInterval(refreshDevices,3000);
  refreshTransfers();setInterval(refreshTransfers,1000);
  refreshMessages();setInterval(refreshMessages,2000);
  document.getElementById('send-btn').onclick=sendMessage;
  document.getElementById('add-peer-btn').onclick=addPeer;
  document.getElementById('file-input').onchange=onFileSelected;
  var ta=document.getElementById('msg-input');
  ta.onkeydown=function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendMessage();}};
  ta.oninput=function(){this.style.height='auto';this.style.height=Math.min(this.scrollHeight,120)+'px';};
}

async function refreshDevices(){
  try{var r=await fetch(API+'/devices'),d=await r.json();devs=d.devices||[];selfName=d.self_name||'';renderDevices();}
  catch(e){}
}

function renderDevices(){
  var list=document.getElementById('device-list'),count=document.getElementById('online-count');
  count.textContent='('+devs.length+')';
  if(!devs.length){list.innerHTML='<li class="empty">暂无在线设备</li>';return}
  list.innerHTML='';
  devs.forEach(function(d){
    var li=document.createElement('li');li.className='dev';
    if(d.id===selected)li.classList.add('selected');
    var av=d.name[0]||'?';
    var statusCls=d.online?'online':'offline';
    li.innerHTML='<div class="avatar">'+av+'</div><div class="info"><div class="name">'+d.name+'</div><div class="ip">'+d.addr+'</div></div><span class="status '+statusCls+'"></span>';
    li.onclick=function(){selectDevice(d.id,d.name,d.addr);};
    list.appendChild(li);
  });
}

function selectDevice(id,name,addr){
  selected=id;
  document.getElementById('chat-header').innerHTML=name+' <small style="color:#888;font-weight:400">('+addr+')</small> <button class="delete-btn" onclick="event.stopPropagation();removePeer(\''+addr+'\')">删除</button>';
  document.getElementById('send-btn').disabled=false;
  document.querySelectorAll('#device-list .dev').forEach(function(el){el.classList.remove('selected')});
  renderMessages();
  renderDevices();
}

function renderMessages(){
  var area=document.getElementById('chat-area');
  var msgs=msg[selected]||[];
  if(!msgs.length){area.innerHTML='<div class="empty-chat">暂无消息，发送一个吧</div>';return}
  area.innerHTML='';
  msgs.forEach(function(m){
    var div=document.createElement('div');div.className='msg '+m.cls;
    var h='';
    if(m.text)h+=m.text;
    if(m.file){
      h+='<div style="margin-top:6px">';
      h+='<div style="font-size:13px;font-weight:600">'+m.file.name+'</div>';
      h+='<div style="font-size:11px;opacity:0.7;margin-top:2px">'+formatSize(m.file.size)+'</div>';
      if(m.file.progress!==undefined&&m.file.done!==true){
        var pct=m.file.progress||0;
        h+='<div style="background:rgba(255,255,255,.15);border-radius:4px;height:4px;margin-top:6px;overflow:hidden"><div style="background:#58a6ff;height:100%;width:'+pct+'%"></div></div>';
        h+='<div style="font-size:11px;margin-top:3px">'+pct+'%</div>';
      }
      if(m.file.done===true)h+='<div style="font-size:11px;color:#4caf50;margin-top:3px">已完成</div>';
    }
    div.innerHTML=h;
    area.appendChild(div);
  });
  area.scrollTop=area.scrollHeight;
}

function findOrCreateMsg(deviceId,fileId,cls,text,fileInfo){
  if(!msg[deviceId])msg[deviceId]=[];
  var found=msg[deviceId].find(function(m){return m.file&&m.file.fileId===fileId});
  if(found){
    if(fileInfo.progress!==undefined)found.file.progress=fileInfo.progress;
    if(fileInfo.done!==undefined)found.file.done=fileInfo.done;
    return found;
  }
  var m={cls:cls,text:text,file:fileInfo,time:Date.now()};
  msg[deviceId].push(m);
  return m;
}

function addTextMsg(deviceId,cls,text){if(!msg[deviceId])msg[deviceId]=[];msg[deviceId].push({cls:cls,text:text,time:Date.now()});}

function formatSize(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';if(b<1073741824)return(b/1048576).toFixed(1)+' MB';return(b/1073741824).toFixed(1)+' GB';}

async function sendMessage(){
  var ta=document.getElementById('msg-input'),txt=ta.value.trim();
  if(!txt||!selected)return;
  var dev=devs.find(function(d){return d.id===selected});
  if(!dev)return;
  try{
    await fetch(API+'/message',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'target_addr='+encodeURIComponent(dev.addr)+'&target_channel='+(dev.port||1)+'&text='+encodeURIComponent(txt)});
    addTextMsg(selected,'sent',txt);
    ta.value='';ta.style.height='auto';renderMessages();
  }catch(e){}
}

async function onFileSelected(){
  var f=document.getElementById('file-input').files[0];
  if(!f||!selected)return;
  var dev=devs.find(function(d){return d.id===selected});
  if(!dev)return;
  var reader=new FileReader();
  reader.onload=async function(){
    var b64=reader.result.split(',')[1];
    try{
      var r=await fetch(API+'/transfer',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'target_addr='+encodeURIComponent(dev.addr)+'&target_channel='+(dev.port||1)+'&filename='+encodeURIComponent(f.name)+'&filedata='+encodeURIComponent(b64)});
      var d=await r.json();
    }catch(e){}
  };
  reader.readAsDataURL(f);
  document.getElementById('file-input').value='';
}

async function refreshTransfers(){
  try{
    var r=await fetch(API+'/transfers'),d=await r.json();
    (d.transfers||[]).forEach(function(t){
      var isSender=t.is_sender;
      var peerAddr=t.target_addr;
      var dev=devs.find(function(dd){return dd.addr===peerAddr});
      var did=dev?dev.id:('addr_'+peerAddr);
      if(!did)return;

      var pct=t.total_chunks>0?Math.round(t.progress_chunk/t.total_chunks*100):0;
      var isDone=t.state==='COMPLETED';
      var cls=isSender?'sent':'received';
      var label=isSender?('发送文件: '+t.filename):('收到文件: '+t.filename);

      findOrCreateMsg(did,t.file_id,cls,label,{
        name:t.filename,size:t.file_size,fileId:t.file_id,
        progress:isDone?100:pct,done:isDone||undefined
      });
    });
    if(selected)renderMessages();
  }catch(e){}
}

function findDeviceIdForReceiving(addr){
  if(!addr)return null;
  for(var i=0;i<devs.length;i++){if(devs[i].addr===addr)return devs[i].id}
  return null;
}

async function addPeer(){
  var addr=document.getElementById('peer-ip').value.trim();if(!addr)return;
  await fetch(API+'/peers/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'addr='+encodeURIComponent(addr)+'&name='+encodeURIComponent(addr)});
  document.getElementById('peer-ip').value='';
  refreshDevices();
}

async function removePeer(addr){
  await fetch(API+'/peers/remove',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'addr='+encodeURIComponent(addr)});
  selected='';document.getElementById('chat-header').textContent='选择一个设备开始聊天';
  document.getElementById('chat-area').innerHTML='<div class="empty-chat">请从左侧选择设备</div>';
  document.getElementById('send-btn').disabled=true;
  refreshDevices();
}

async function refreshMessages(){
  for(var i=0;i<devs.length;i++){
    var d=devs[i];
    try{
      var r=await fetch(API+'/messages/poll',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'addr='+encodeURIComponent(d.addr)});
      var data=await r.json();
      (data.messages||[]).forEach(function(m){
        addTextMsg(d.id,'received',m.text);
      });
    }catch(e){}
  }
  if(selected)renderMessages();
}

init();})();
