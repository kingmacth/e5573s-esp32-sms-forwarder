#pragma once
#include <Arduino.h>
static const char WEB_UI[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>短信转发设置</title><style>body{font:16px system-ui;background:#f2f5f8;color:#15283e;max-width:620px;margin:24px auto;padding:16px}section{background:white;padding:22px;border-radius:14px;margin:16px 0}label{display:block;margin:16px 0 6px}input,select,button{box-sizing:border-box;width:100%;padding:12px;border:1px solid #b9c5d2;border-radius:7px;font:inherit}button{background:#145dc4;color:white;margin-top:20px;cursor:pointer}small{color:#596b7e}pre{white-space:pre-wrap;word-break:break-word}#result{white-space:pre-wrap}</style>
<h1>短信转发器</h1><section><h2>运行状态</h2><pre id="status">正在读取…</pre></section>
<section><form id="settings"><h2>设备设置</h2>
<label>Wi-Fi 名称</label><input name="ssid" maxlength="32" required>
<button id="scanWifi" type="button">扫描附近 Wi-Fi</button>
<p id="scanHint" role="status"></p><select id="wifiList" aria-label="选择附近 Wi-Fi" hidden></select>
<small>可扫描后选择，也可手动填写隐藏网络。仅支持 2.4 GHz Wi-Fi。</small>
<label>Wi-Fi 密码</label><input name="wifiPass" type="password" maxlength="63" placeholder="留空保留已保存密码">
<label><input name="openWifi" type="checkbox" style="width:auto"> 此 Wi-Fi 无密码（清除原密码）</label>
<small>通常连接 E5573S 自身热点，须能访问它的管理地址并连接互联网。</small>
<label>E5573S 管理地址</label><input name="host" placeholder="192.168.8.1" required>
<label>管理用户名</label><input name="user" maxlength="64" required>
<label>管理密码</label><input name="devicePass" type="password" maxlength="128" placeholder="留空保留已保存密码">
<label>企业微信机器人 Webhook</label><input name="webhook" type="password" maxlength="200" placeholder="留空保留设备端地址" autocomplete="off">
<small id="hookHint"></small>
<label>推送消息开头标记</label><input name="prefix" maxlength="80">
<label>查询 / 失败重试间隔（秒）</label><input name="interval" type="number" min="2" max="3600" required>
<label>休眠模式</label><select name="sleep"><option value="modem">Wi-Fi 节能，保持网页和串口可用</option><option value="light">轻度休眠，每轮断开 Wi-Fi 再定时唤醒</option><option value="off">关闭休眠</option></select>
<small>轻度休眠在开机或访问网页后等待 60 秒再启用；GPIO27 按键可唤醒并开启配网。</small>
<button>保存并重启</button><p id="result" role="status"></p></form></section>
<small>固定热点 E5573S-SMS-Setup / 密码 E5573sSetup；地址 192.168.4.1。GPIO27 短按配网，长按 8 秒恢复出厂。</small>
<script>
const form=document.querySelector('form'), result=document.querySelector('#result');
const scanButton=document.querySelector('#scanWifi'),scanHint=document.querySelector('#scanHint'),wifiList=document.querySelector('#wifiList');
let scannedNetworks=[];
wifiList.onchange=()=>{if(wifiList.value==='')return;const n=scannedNetworks[Number(wifiList.value)];if(!n)return;form.elements.ssid.value=n.ssid;form.elements.openWifi.checked=n.open;};
scanButton.onclick=async()=>{
  scanButton.disabled=true;wifiList.hidden=true;scanHint.textContent='正在扫描，请稍候…';
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),25000);
  try{
    let r=await fetch('/api/wifi/scan',{method:'POST',signal:controller.signal}),s=await r.json();if(!r.ok)throw new Error(s.message||'扫描启动失败');
    for(;;){
      await new Promise(resolve=>setTimeout(resolve,800));
      r=await fetch('/api/wifi/scan',{signal:controller.signal});s=await r.json();if(!r.ok)throw new Error(s.message||'扫描失败');
      if(s.state==='scanning')continue;if(s.state!=='done')throw new Error('请重新扫描');
      scannedNetworks=s.networks;wifiList.replaceChildren(new Option('请选择 Wi-Fi',''));
      scannedNetworks.forEach((n,i)=>wifiList.add(new Option(`${n.ssid} · ${n.rssi} dBm · ${n.open?'无密码':'需密码'}`,String(i))));
      wifiList.hidden=scannedNetworks.length===0;scanHint.textContent=scannedNetworks.length?`找到 ${scannedNetworks.length} 个 Wi-Fi，请选择。`:'未找到可见 Wi-Fi，可重新扫描或手动填写。';break;
    }
  }catch(e){scanHint.textContent=e.name==='AbortError'?'扫描超时，请重试或手动填写名称。':e.message;}
  finally{clearTimeout(timeout);scanButton.disabled=false;}
};
async function load(){try{let r=await fetch('/api/config'),c=await r.json();for(let n of ['ssid','host','user','prefix','interval','sleep'])form.elements[n].value=c[n];document.querySelector('#hookHint').textContent=c.webhookSet?'已在设备端保存，留空保留。':'尚未配置推送地址。';}catch(e){result.textContent='读取失败，请刷新。';}}
async function status(){try{let r=await fetch('/api/status'),s=await r.json();document.querySelector('#status').textContent=`网络：${s.wifi?'已连接':'未连接'}  热点：${s.ap?'已开启':'已关闭'}\n设备地址：${s.ip}\n${s.message}\n本次开机推送成功：${s.success}，失败：${s.failure}\n最近检查距今：${s.age} 秒`; }catch(e){} }
form.onsubmit=async e=>{e.preventDefault();result.textContent='正在保存…';try{let data=Object.fromEntries(new FormData(form));let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});let s=await r.json();result.textContent=s.message;}catch(e){result.textContent='连接中断，请重新连接设备后检查设置。';}};
load();status();setInterval(status,5000);
</script></html>)HTML";
