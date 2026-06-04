(function(){'use strict';
const API='/api';
var devs=[],msg={},selected='',selfName='';

function init(){
  refreshDevices();setInterval(refreshDevices,3000);
  refreshTransfers();setInterval(refreshTransfers,2000);
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
    li.innerHTML='<div class="avatar">'+av+'</div><div class="info"><div class="name">'+d.name+'</div><div class="ip">'+d.ip+'</div></div><span class="status online"></span>';
    li.onclick=function(){selectDevice(d.id,d.name,d.ip);};
    list.appendChild(li);
  });
}

function selectDevice(id,name,ip){
  selected=id;
  document.getElementById('chat-header').innerHTML=name+' <small style="color:#888;font-weight:400">('+ip+')</small> <button class="delete-btn" onclick="event.stopPropagation();removePeer(\''+ip+'\')">删除</button>';
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
    var txt=m.text||'';
    if(m.file){
      txt+='<div class="file-info">'+m.file.name+' ('+formatSize(m.file.size)+')';
      if(m.file.done===true)txt+=' <span class="ok">✓ 已完成</span>';
      else if(m.file.done===false)txt+=' <span class="err">✗ 失败</span>';
      else txt+=' <span>...传输中</span>';
      txt+='</div>';
    }
    div.innerHTML=txt;
    area.appendChild(div);
  });
  area.scrollTop=area.scrollHeight;
}

function addMsg(deviceId,cls,text,fileInfo){
  if(!msg[deviceId])msg[deviceId]=[];
  msg[deviceId].push({cls:cls,text:text,file:fileInfo,time:Date.now()});
  if(deviceId===selected)renderMessages();
}

function formatSize(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';if(b<1073741824)return(b/1048576).toFixed(1)+' MB';return(b/1073741824).toFixed(1)+' GB';}

async function sendMessage(){
  var ta=document.getElementById('msg-input'),txt=ta.value.trim();
  if(!txt||!selected)return;
  var dev=devs.find(function(d){return d.id===selected});
  if(!dev)return;
  try{
    await fetch(API+'/message',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'target_ip='+encodeURIComponent(dev.ip)+'&target_port='+(dev.port||8889)+'&text='+encodeURIComponent(txt)});
    addMsg(selected,'sent',txt);
    ta.value='';ta.style.height='auto';
  }catch(e){}
}

async function onFileSelected(){
  var f=document.getElementById('file-input').files[0];
  if(!f||!selected)return;
  var dev=devs.find(function(d){return d.id===selected});
  if(!dev)return;
  addMsg(selected,'sent','发送文件: '+f.name,{name:f.name,size:f.size});
  var reader=new FileReader();
  reader.onload=async function(){
    var b64=reader.result.split(',')[1];
    try{
      var r=await fetch(API+'/transfer',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'target_ip='+encodeURIComponent(dev.ip)+'&target_port='+(dev.port||8889)+'&filename='+encodeURIComponent(f.name)+'&filedata='+encodeURIComponent(b64)});
      var d=await r.json();
      if(!d.success){updateLastFileMsg(selected,false);}
    }catch(e){updateLastFileMsg(selected,false);}
  };
  reader.readAsDataURL(f);
  document.getElementById('file-input').value='';
}

function updateLastFileMsg(deviceId,done){
  if(!msg[deviceId])return;
  for(var i=msg[deviceId].length-1;i>=0;i--){if(msg[deviceId][i].file){msg[deviceId][i].file.done=done;break}}
  if(deviceId===selected)renderMessages();
}

async function refreshTransfers(){
  try{
    var r=await fetch(API+'/transfers'),d=await r.json();
    (d.transfers||[]).forEach(function(t){
      var dev=devs.find(function(dd){return dd.ip===t.target_ip});
      var did=dev?dev.id:'';
      if(!did)return;
      var existing=msg[did]||[];
      var found=existing.find(function(m){return m.file&&m.file.fileId===t.file_id});
      if(t.state==='COMPLETED'){
        if(found){found.file.done=true;}
      }else if(t.state==='TRANSFERRING'){
        if(!found){
          if(!msg[did])msg[did]=[];
          msg[did].push({cls:'sent',text:'发送文件: '+t.filename,file:{name:t.filename,size:t.file_size,fileId:t.file_id}});
        }
      }
    });
    if(selected)renderMessages();
  }catch(e){}
}

async function addPeer(){
  var ip=document.getElementById('peer-ip').value.trim();if(!ip)return;
  await fetch(API+'/peers/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ip='+encodeURIComponent(ip)+'&name='+encodeURIComponent(ip)});
  document.getElementById('peer-ip').value='';
  refreshDevices();
}

async function removePeer(ip){
  await fetch(API+'/peers/remove',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ip='+encodeURIComponent(ip)});
  selected='';document.getElementById('chat-header').textContent='选择一个设备开始聊天';
  document.getElementById('chat-area').innerHTML='<div class="empty-chat">请从左侧选择设备</div>';
  document.getElementById('send-btn').disabled=true;
  refreshDevices();
}

init();})();