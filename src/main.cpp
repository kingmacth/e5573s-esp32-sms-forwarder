#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_sleep.h>
#include <driver/uart.h>
#include <driver/rtc_io.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include "core.h"
#include "web_ui.h"
#include "wecom_ca.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef DEFAULT_WEBHOOK
#define DEFAULT_WEBHOOK ""
#endif
#ifndef AUTH_PAUSE_ON_UPGRADE
#define AUTH_PAUSE_ON_UPGRADE 0
#endif

constexpr int LED=26, BUTTON=27;
constexpr char AP_SSID[]="E5573S-SMS-Setup", AP_PASS[]="E5573sSetup";
struct Config {
  String ssid,wifiPass,host="192.168.8.1",user="admin",devicePass,webhook=DEFAULT_WEBHOOK,prefix="【新短信】",sleep="modem";
  uint32_t interval=10;
} cfg;
WebServer web(80); DNSServer dns;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t workGate;
String message="等待 Wi-Fi 连接";
uint32_t successCount=0,failureCount=0,lastCheck=0;
uint32_t statusVersion=0;
volatile bool ap=false,workerBusy=false,updating=false,storageOK=false;
volatile bool serialActive=false,forcePoll=true;
volatile bool testRequested=false,resetSession=false;
volatile uint32_t lastInteraction=0;
uint32_t restartAt=0,lastConnect=0;
bool wifiScanning=false,wifiScanStarted=false;
volatile bool authPaused=false;
volatile bool singleSmsRequested=false;
volatile uint32_t authCooldownStart=0,authCooldownDuration=0;
unsigned authFailures=0;
bool authTestsOK=false;
String authDiagnostic="尚未进行登录诊断";
String cookie,token;
std::vector<String> currentUnread;
bool snapshotValid=false;

void status(const String &s){xSemaphoreTake(stateMutex,portMAX_DELAY);message=s;statusVersion++;xSemaphoreGive(stateMutex);Serial.println(s);}
bool elapsed(uint32_t now,uint32_t then,uint32_t ms){return uint32_t(now-then)>=ms;}
void loadConfig(){
  Preferences p;p.begin("smscfg",true);
  cfg.ssid=p.getString("ssid","");cfg.wifiPass=p.getString("wifiPass","");
  cfg.host=p.getString("host","192.168.8.1");cfg.user=p.getString("user","admin");
  cfg.devicePass=p.getString("devicePass","");cfg.webhook=p.getString("webhook",DEFAULT_WEBHOOK);
  cfg.prefix=p.getString("prefix","【新短信】");cfg.interval=constrain(p.getUInt("interval",10),2U,3600U);
  cfg.sleep=p.getString("sleep","modem");p.end();
}
void startAP(){
  if(ap)return;
  WiFi.mode(WIFI_AP_STA);WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  if(!WiFi.softAP(AP_SSID,AP_PASS)){status("配网热点启动失败");return;}
  ap=true;WiFi.setSleep(false);dns.start(53,"*",WiFi.softAPIP());
  Serial.println("AP: E5573S-SMS-Setup / 192.168.4.1");
}
bool allowedRequest(){
  // Reject browser cross-origin writes. No wildcard CORS, secrets are never returned.
  String origin=web.header("Origin");
  return origin.isEmpty()||origin=="http://"+web.hostHeader();
}
void jsonReply(int code,const String &msg){
  DynamicJsonDocument d(512);d["message"]=msg;String out;serializeJson(d,out);web.send(code,"application/json; charset=utf-8",out);
}
void setupWeb(){
  const char *keys[]={"Origin"};web.collectHeaders(keys,1);
  web.on("/",HTTP_GET,[]{lastInteraction=millis();web.send_P(200,"text/html; charset=utf-8",WEB_UI);});
  web.on("/api/wifi/scan",HTTP_POST,[]{
    lastInteraction=millis();
    if(!allowedRequest()){jsonReply(403,"来源无效");return;}
    if(updating||serialActive){jsonReply(409,"固件更新期间无法扫描 Wi-Fi");return;}
    if(!wifiScanning){
      WiFi.scanDelete();wifiScanStarted=true;
      int16_t n=WiFi.scanNetworks(true,false,false,150);
      wifiScanning=n==WIFI_SCAN_RUNNING;
      if(n==WIFI_SCAN_FAILED){jsonReply(503,"扫描启动失败，设备可能正在连接 Wi-Fi，请稍后重试");return;}
    }
    web.sendHeader("Cache-Control","no-store");web.send(202,"application/json","{\"state\":\"scanning\"}");
  });
  web.on("/api/wifi/scan",HTTP_GET,[]{
    lastInteraction=millis();int16_t n=WiFi.scanComplete();
    if(!wifiScanStarted){web.send(200,"application/json","{\"state\":\"idle\"}");return;}
    if(n==WIFI_SCAN_RUNNING){web.send(202,"application/json","{\"state\":\"scanning\"}");return;}
    wifiScanning=false;
    if(n<0){jsonReply(503,"Wi-Fi 扫描失败，请稍后重试或手动填写名称");return;}
    DynamicJsonDocument d(8192);d["state"]="done";JsonArray networks=d.createNestedArray("networks");
    std::vector<int> order;for(int i=0;i<n;i++)if(WiFi.SSID(i).length())order.push_back(i);
    std::sort(order.begin(),order.end(),[](int a,int b){return WiFi.RSSI(a)>WiFi.RSSI(b);});
    std::vector<String> seen;
    for(int i:order){
      String ssid=WiFi.SSID(i);bool duplicate=false;for(const String &s:seen)if(s==ssid){duplicate=true;break;}
      if(duplicate)continue;seen.push_back(ssid);
      JsonObject item=networks.createNestedObject();item["ssid"]=ssid;item["rssi"]=WiFi.RSSI(i);item["open"]=WiFi.encryptionType(i)==WIFI_AUTH_OPEN;
      if(networks.size()>=30)break;
    }
    String out;serializeJson(d,out);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json; charset=utf-8",out);
  });
  web.on("/api/config",HTTP_GET,[]{
    lastInteraction=millis();DynamicJsonDocument d(1024);
    d["ssid"]=cfg.ssid;d["host"]=cfg.host;d["user"]=cfg.user;d["prefix"]=cfg.prefix;
    d["interval"]=cfg.interval;d["sleep"]=cfg.sleep;d["webhookSet"]=cfg.webhook.length()>0;
    String s;serializeJson(d,s);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json; charset=utf-8",s);
  });
  web.on("/api/status",HTTP_GET,[]{
    DynamicJsonDocument d(1024);d["wifi"]=WiFi.status()==WL_CONNECTED;d["ap"]=ap;
    d["ip"]=WiFi.localIP().toString();
    xSemaphoreTake(stateMutex,portMAX_DELAY);d["message"]=message;d["success"]=successCount;d["failure"]=failureCount;
    d["age"]=lastCheck?(millis()-lastCheck)/1000:0;xSemaphoreGive(stateMutex);
    String s;serializeJson(d,s);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json; charset=utf-8",s);
  });
  web.on("/api/config",HTTP_POST,[]{
    if(!allowedRequest()){jsonReply(403,"来源无效");return;}
    if(web.arg("plain").length()>2048){jsonReply(413,"配置过长");return;}
    DynamicJsonDocument d(4096);if(deserializeJson(d,web.arg("plain"))){jsonReply(400,"配置格式错误");return;}
    Config n=cfg;
    n.ssid=d["ssid"].as<String>();n.host=d["host"].as<String>();n.user=d["user"].as<String>();
    n.prefix=d["prefix"].as<String>();n.sleep=d["sleep"].as<String>();
    String v=d["interval"].as<String>();char *tail;long interval=strtol(v.c_str(),&tail,10);
    if(v.isEmpty()||*tail||interval<2||interval>3600){jsonReply(400,"间隔必须为 2～3600 的整数");return;}
    n.interval=interval;IPAddress ip;
    if(n.ssid.isEmpty()||n.ssid.length()>32||!ip.fromString(n.host)||n.user.isEmpty()||n.user.length()>64||n.prefix.length()>240||
       (n.sleep!="off"&&n.sleep!="modem"&&n.sleep!="light")){jsonReply(400,"请检查 Wi-Fi、管理 IPv4 地址、用户名和休眠模式");return;}
    String wp=d["wifiPass"]|"",dp=d["devicePass"]|"",hook=d["webhook"]|"";
    if(d["openWifi"]=="on")n.wifiPass="";else if(wp.length())n.wifiPass=wp;
    if(dp.length())n.devicePass=dp;if(hook.length())n.webhook=hook;
    if((n.wifiPass.length()&&n.wifiPass.length()<8)||n.wifiPass.length()>63||n.devicePass.length()>128||!validWebhook(n.webhook)){
      jsonReply(400,"请检查密码长度及企业微信机器人地址");return;
    }
    DynamicJsonDocument saved(4096);
    saved["ssid"]=n.ssid;saved["wifiPass"]=n.wifiPass;saved["host"]=n.host;saved["user"]=n.user;
    saved["devicePass"]=n.devicePass;saved["webhook"]=n.webhook;saved["prefix"]=n.prefix;saved["interval"]=n.interval;saved["sleep"]=n.sleep;
    // Store one versioned blob so interrupted saves cannot mix old and new fields.
    String blob;serializeJson(saved,blob);Preferences p;
    if(!p.begin("smscfg",false)){jsonReply(500,"配置存储不可用");return;}
    bool ok=p.putString("config",blob)==blob.length();p.end();
    if(!ok){jsonReply(500,"配置保存失败");return;}
    jsonReply(200,"已保存，即将重启。请连接所设置的 Wi-Fi，或等待配网热点恢复。");restartAt=millis()+1000;
  });
  web.onNotFound([]{web.sendHeader("Location","http://192.168.4.1/",true);web.send(302,"text/plain","");});web.begin();
}
void loadBlob(){
  Preferences p;p.begin("smscfg",true);String s=p.getString("config","");p.end();if(!s.length())return;
  DynamicJsonDocument d(4096);if(deserializeJson(d,s))return;
  cfg.ssid=d["ssid"].as<String>();cfg.wifiPass=d["wifiPass"].as<String>();cfg.host=d["host"].as<String>();cfg.user=d["user"].as<String>();
  cfg.devicePass=d["devicePass"].as<String>();cfg.webhook=d["webhook"].as<String>();cfg.prefix=d["prefix"].as<String>();
  cfg.interval=constrain(d["interval"].as<uint32_t>(),2U,3600U);cfg.sleep=d["sleep"].as<String>();
}

String hilink(const String &path,const String &body=""){
  WiFiClient client;HTTPClient http;http.setConnectTimeout(4000);http.setTimeout(5000);
  if(!http.begin(client,"http://"+cfg.host+path))return "";
  const char *headers[]={"Set-Cookie"};http.collectHeaders(headers,1);
  http.addHeader("User-Agent","Mozilla/5.0");http.addHeader("Accept","*/*");
  if(cookie.length())http.addHeader("Cookie",cookie);
  if(token.length())http.addHeader("__RequestVerificationToken",token);
  if(body.length())http.addHeader("Content-Type","text/xml; charset=UTF-8");
  String requestBody=body;
  if(body.length()&&!body.startsWith("<?xml"))requestBody="<?xml version=\"1.0\" encoding=\"UTF-8\"?>"+body;
  int code=body.length()?http.POST(requestBody):http.GET();String result;
  if(code==200){result=http.getString();String c=http.header("Set-Cookie");int a=c.indexOf("SessionID=");
    if(a>=0){int b=c.indexOf(';',a);cookie=b<0?c.substring(a):c.substring(a,b);}}
  if(code!=200)Serial.printf("[hilink] path=%s HTTP=%d\n",path.c_str(),code);
  http.end();return result;
}
bool fetchToken(){
  String x=hilink("/api/webserver/SesTokInfo");token=scalarTag(x,"TokInfo");
  String s=xmlDecode(scalarTag(x,"SesInfo"));if(s.startsWith("SessionID=")){int e=s.indexOf(';');cookie=e<0?s:s.substring(0,e);}
  return token.length()>0 && cookie.length()>0;
}
String post(const String &path,const String &body){
  for(int i=0;i<2;i++){
    if(!fetchToken())return "";
    String r=hilink(path,body);
    String code=scalarTag(r,"code");if(code!="125003"&&code!="125001")return r;
  }return "";
}
bool login(bool singleTest=false){
  if(!authTestsOK){status("登录算法自检失败，已停止登录请求");return false;}
  if(authPaused&&!singleTest){status("E5573S 登录已手动暂停");return false;}
  uint32_t wait=remainingCooldown(millis(),authCooldownStart,authCooldownDuration);
  if(wait){status("E5573S 登录退避中，剩余 "+String((wait+999)/1000)+" 秒，不发送登录请求");return false;}
  authCooldownDuration=0;
  if(cookie.isEmpty()){hilink("/html/mobileconnection.html");if(cookie.isEmpty())hilink("/");}
  if(!fetchToken()){status("E5573S 会话连接失败，请检查网络和管理地址");return false;}
  String state=hilink("/api/user/state-login");String stateValue=scalarTag(state,"State");
  if(stateValue=="0"){
    authFailures=0;authCooldownDuration=0;
    xSemaphoreTake(stateMutex,portMAX_DELAY);authDiagnostic="State=0 已登录，复用当前会话";xSemaphoreGive(stateMutex);
    return true;
  }
  if(stateValue.isEmpty()){status("读取 E5573S 登录状态失败，未提交密码");return false;}
  if(scalarTag(state,"extern_password_type")=="1"){status("此固件要求 SCRAM，当前仅支持已验证的传统登录");return false;}
  if(!fetchToken())return false;
  String type=scalarTag(state,"password_type");if(type.isEmpty())type="4";
  if(type!="4"&&type!="3"){status("不支持的 HiLink 密码类型");return false;}
  String diagnostic="State="+stateValue+" password_type="+type+" extern_password_type="+scalarTag(state,"extern_password_type")
    +" Cookie="+(cookie.length()?String("present"):String("missing"))+" token_len="+String(token.length());
  xSemaphoreTake(stateMutex,portMAX_DELAY);authDiagnostic=diagnostic;xSemaphoreGive(stateMutex);
  Serial.println("[login] "+diagnostic);
  String password=type=="4"?hilinkPassword(cfg.user,cfg.devicePass,token):b64(cfg.devicePass);
  String r=hilink("/api/user/login","<request><Username>"+xmlEscape(cfg.user)+"</Username><Password>"+password+"</Password><password_type>"+type+"</password_type></request>");
  bool ok=scalarTag(r,"response")=="OK";
  if(ok)ok=scalarTag(hilink("/api/user/state-login"),"State")=="0";
  if(!ok){
    String code=scalarTag(r,"code");if(code.isEmpty())code="响应异常或登录态未确认";
    authFailures=min(authFailures+1,4U);authCooldownStart=millis();authCooldownDuration=loginRetryDelay(authFailures);
    status("E5573S 登录失败，错误码："+code+"；停止尝试 "+String(authCooldownDuration/60000)+" 分钟");
    xSemaphoreTake(stateMutex,portMAX_DELAY);authDiagnostic=diagnostic+" error="+code;xSemaphoreGive(stateMutex);
    cookie="";token="";return false;
  }
  authFailures=0;authCooldownDuration=0;status("E5573S 登录成功，已确认登录状态");return true;
}
String receiptPath(const String &id){return "/q/"+id+".json";}
bool saveReceipt(const String &id,JsonDocument &d){
  String p=receiptPath(id);File f=LittleFS.open(p+".tmp","w");if(!f)return false;
  size_t expected=measureJson(d),actual=serializeJson(d,f);f.flush();f.close();
  if(actual!=expected){LittleFS.remove(p+".tmp");return false;}
  return LittleFS.rename(p+".tmp",p);
}
struct ReadSms {
  bool found=false;
  String phone,date,text;
};

bool discover(String *selected=nullptr,ReadSms *readFallback=nullptr){
  currentUnread.clear();snapshotValid=false;
  // Snapshot every page before set-read, so page positions cannot shift during processing.
  for(int page=1;page<=100;page++){
    if(updating)return false;
    String r=post("/api/sms/sms-list","<request><PageIndex>"+String(page)+"</PageIndex><ReadCount>20</ReadCount><BoxType>1</BoxType><SortType>0</SortType><Ascending>"+(selected?"0":"1")+"</Ascending><UnreadPreferred>0</UnreadPreferred></request>");
    if(r.indexOf("<Messages")==-1||r.indexOf("<error>")>=0){status("读取短信列表失败："+tag(r,"code"));cookie="";return false;}
    int pos=0,count=0;
    while((pos=r.indexOf("<Message>",pos))>=0){
      int end=r.indexOf("</Message>",pos);if(end<0){status("短信 XML 不完整");return false;}
      String m=r.substring(pos,end+10);pos=end+10;count++;
      String smsState=scalarTag(m,"Smstat");
      if(smsState!="0"){
        if(readFallback&&!readFallback->found&&smsState=="1"){
          readFallback->found=true;
          readFallback->phone=xmlDecode(tag(m,"Phone"));
          readFallback->date=xmlDecode(tag(m,"Date"));
          readFallback->text=xmlDecode(tag(m,"Content"));
        }
        continue;
      }
      String index=tag(m,"Index"),phone=xmlDecode(tag(m,"Phone")),date=xmlDecode(tag(m,"Date")),text=xmlDecode(tag(m,"Content"));
      if(index.isEmpty()||index.toInt()<=0){status("短信索引无效");return false;}
      String id=sha256(cfg.host+"\n"+index+"\n"+phone+"\n"+date+"\n"+text);
      currentUnread.push_back(id);
      if(!LittleFS.exists(receiptPath(id))){
        DynamicJsonDocument d(24576);d["id"]=id;d["host"]=cfg.host;d["index"]=index;
        d["phone"]=phone;d["date"]=date;d["text"]=text;d["offset"]=0;d["sent"]=false;d["read"]=false;
        if(d.overflowed()||!saveReceipt(id,d)){status("短信队列存储失败，保留原短信，下轮重试");return false;}
      }
      if(selected){*selected=id;snapshotValid=true;return true;}
    }
    if(count<20){snapshotValid=true;return true;}
    vTaskDelay(1);
  }status("收件箱超过 2000 条，请清理历史短信后继续");return false;
}
// 4 seconds between requests, including failed attempts (15 attempts/minute).
uint32_t lastPush=0;bool hasPushed=false;
bool pushText(const String &text,bool manual=false){
  if(!validWebhook(cfg.webhook)){status("请配置企业微信 Webhook");return false;}
  if(time(nullptr)<1700000000){status("等待网络校时，暂不发送 HTTPS 请求");return false;}
  while(hasPushed&&!elapsed(millis(),lastPush,4000)){if(updating)return false;vTaskDelay(pdMS_TO_TICKS(20));}
  if(updating)return false;
  WiFiClientSecure tls;tls.setCACert(WECOM_CA);HTTPClient http;http.setConnectTimeout(5000);http.setTimeout(8000);
  if(!http.begin(tls,cfg.webhook)){status("推送 TLS 初始化失败");return false;}
  DynamicJsonDocument d(4096);d["msgtype"]="text";d["text"]["content"]=text;String body;serializeJson(d,body);
  http.addHeader("Content-Type","application/json; charset=utf-8");lastPush=millis();hasPushed=true;
  int code=http.POST(body);String reply=code>0?http.getString():"";http.end();
  DynamicJsonDocument response(1024);bool ok=code==200&&!deserializeJson(response,reply)&&response["errcode"].is<int>()&&response["errcode"].as<int>()==0;
  xSemaphoreTake(stateMutex,portMAX_DELAY);if(ok)successCount++;else failureCount++;xSemaphoreGive(stateMutex);
  if(!ok)status("推送失败，HTTP="+String(code)+"，企业微信错误码="+String(response["errcode"]|-1)+(manual?"":"；下轮重试"));
  return ok;
}

String smsHeading(const String &phone,const String &date){
  return cfg.prefix+"\n发件人："+phone+"\n时间："+date+"\n";
}

void testReadSms(const ReadSms &sms){
  size_t offset=0;
  do {
    String heading=smsHeading(sms.phone,sms.date);
    if(sms.text.length()>1800)heading+="[分段，字节偏移 "+String(offset)+"]\n";
    if(heading.length()>1024){status("短信元信息过长");return;}
    const size_t bytes=chunkBytes(sms.text,offset,2048-heading.length());
    if(!pushText(heading+sms.text.substring(offset,offset+bytes),true))return;
    offset+=bytes;
  }while(offset<sms.text.length());
  status("已读短信测试推送成功，企业微信已确认（errcode=0）");
}
void processQueue(const String &onlyId=""){
  File dir=LittleFS.open("/q");if(!dir || !dir.isDirectory())return;
  for(File f=dir.openNextFile();f;f=dir.openNextFile()){
    if(updating||(authPaused&&onlyId.isEmpty())){f.close();return;}
    String name=f.name();if(!name.endsWith(".json")){f.close();continue;}
    DynamicJsonDocument d(24576);auto error=deserializeJson(d,f);f.close();
    if(error){status("队列记录损坏，请保留闪存并检查");continue;}
    if(d["host"].as<String>()!=cfg.host || d["read"]==true)continue;
    String id=d["id"].as<String>();
    if(onlyId.length()&&id!=onlyId)continue;
    const bool alreadySent=d["sent"]==true;
    if(d["sent"]!=true){
      String text=d["text"].as<String>();size_t offset=d["offset"]|0U;
      do {
        if(updating||(authPaused&&onlyId.isEmpty()))return;
        String head=smsHeading(d["phone"].as<String>(),d["date"].as<String>());
        if(text.length()>1800)head+="[分段，字节偏移 "+String(offset)+"]\n";
        if(head.length()>1024){status("短信元信息过长");return;}
        size_t n=chunkBytes(text,offset,2048-head.length());
        if(!pushText(head+text.substring(offset,offset+n)))return;
        offset+=n;d["offset"]=offset;if(offset>=text.length())d["sent"]=true;
        if(!saveReceipt(id,d)){status("推送已成功但记录写入失败，暂停处理；重启可能重复");storageOK=false;return;}
      }while(offset<text.length());
      status("短信已成功推送，正在标记已读");
    }
    if(!snapshotValid)continue;
    bool present=false;for(const String &item:currentUnread)if(item==id){present=true;break;}
    // Never mark a reused index belonging to another message; only mark IDs in this snapshot.
    String r=present?post("/api/sms/set-read","<request><Index>"+d["index"].as<String>()+"</Index></request>"):String("<response>OK</response>");
    if(tag(r,"response")=="OK"||tag(r,"code")=="113055"){
      d["read"]=true;d.remove("text");d.remove("phone");d.remove("date");
      if(!saveReceipt(id,d)){status("已读状态保存失败");storageOK=false;return;}
      if(onlyId.length()){
        status(alreadySent?"此前已推送，本次仅标记已读":"单条短信测试完成，企业微信已确认，短信已标记已读");
        return;
      }
    }else {status("推送已成功，标记已读失败，下轮仅重试已读操作");cookie="";return;}
  }
  if(onlyId.length())status("所选短信已有完成记录，未重复推送");
}
void worker(void*){
  uint32_t completed=0;bool first=true;
  for(;;){
    if(!updating && storageOK && WiFi.status()==WL_CONNECTED && (singleSmsRequested||testRequested||first||forcePoll||(!authPaused&&elapsed(millis(),completed,cfg.interval*1000))) && xSemaphoreTake(workGate,0)==pdTRUE){
      if(updating){xSemaphoreGive(workGate);continue;}
      first=false;forcePoll=false;workerBusy=true;
      uint32_t priorVersion=statusVersion;
      if(resetSession){cookie="";token="";resetSession=false;}
      if(testRequested){testRequested=false;bool ok=pushText(cfg.prefix+"\n这是一条 ESP32 短信转发器测试消息。\nIP："+WiFi.localIP().toString());if(ok)status("测试推送成功，errcode=0");}
      snapshotValid=false;
      if(singleSmsRequested){
        singleSmsRequested=false;
        String selected;
        ReadSms fallback;
        if(!updating&&login(true)&&discover(&selected,&fallback)){
          if(selected.length())processQueue(selected);
          else if(fallback.found)testReadSms(fallback);
          else status("收件箱没有短信，单条测试未发送");
        }
        Serial.println("SMS_TEST_ONE DONE");
      }else{
        if(!updating&&login())discover();
        if(!updating&&!authPaused)processQueue();
      }
      if(priorVersion==statusVersion)status("本轮短信检查完成，等待下次查询");
      xSemaphoreTake(stateMutex,portMAX_DELAY);lastCheck=millis();xSemaphoreGive(stateMutex);
      completed=millis();workerBusy=false;xSemaphoreGive(workGate);
    }vTaskDelay(pdMS_TO_TICKS(50));
  }
}

bool saveAuthPause(bool paused){
  Preferences p;
  return p.begin("smscfg",false)&&p.putBool("authpaused",paused)>0;
}

void factoryReset(){
  updating=true;status("恢复出厂设置…");
  // Wait for any flash transaction in the worker before formatting.
  if(xSemaphoreTake(workGate,pdMS_TO_TICKS(30000))!=pdTRUE){status("设备忙，请稍后重试恢复出厂");updating=false;return;}
  Preferences p;if(p.begin("smscfg",false)){p.clear();p.end();}
  LittleFS.format();WiFi.disconnect(false,true);delay(100);ESP.restart();
}
void buttonTick(){
  static bool pressed=false,resetDone=false;static uint32_t down=0;
  bool low=digitalRead(BUTTON)==LOW;
  if(low&&!pressed){pressed=true;resetDone=false;down=millis();lastInteraction=down;}
  if(low&&pressed&&elapsed(millis(),down,8000)&&!resetDone){resetDone=true;factoryReset();}
  if(!low&&pressed){pressed=false;if(!resetDone&&elapsed(millis(),down,40)){lastInteraction=millis();startAP();}}
  if(low&&pressed&&elapsed(millis(),down,2000))digitalWrite(LED,(millis()/70)%2);
}
// UART updater uses the inactive OTA partition. It does not pretend to be the ROM loader.
enum class UartUpdateState {Command,Header,WaitWorker,Data};
UartUpdateState serialMode=UartUpdateState::Command;String serialLine;size_t updateSize=0,updateReceived=0;String updateMd5;uint32_t updateLast=0;
bool updateHasGate=false;
void abortUpdate(){Update.abort();serialMode=UartUpdateState::Command;updating=false;serialActive=false;serialLine="";if(updateHasGate){updateHasGate=false;xSemaphoreGive(workGate);}Serial.println("UPDATE ERROR");}
void serialTick(){
  if(serialMode==UartUpdateState::WaitWorker&&xSemaphoreTake(workGate,0)==pdTRUE){
    updateHasGate=true;
    if(!Update.begin(updateSize,U_FLASH)||!Update.setMD5(updateMd5.c_str())){abortUpdate();return;}
    serialMode=UartUpdateState::Data;updateLast=millis();Serial.println("UPDATE READY");
  }
  if(serialMode!=UartUpdateState::Command&&elapsed(millis(),updateLast,60000)){abortUpdate();return;}
  if(serialMode==UartUpdateState::Data){
    uint8_t data[1024];size_t n=min((size_t)Serial.available(),min(sizeof(data),updateSize-updateReceived));
    if(n){Serial.readBytes(data,n);if(Update.write(data,n)!=n){abortUpdate();return;}updateReceived+=n;updateLast=millis();Serial.printf("UPDATE RX %u\n",(unsigned)updateReceived);}
    if(updateReceived==updateSize){
      if(!Update.end(false)){abortUpdate();return;}
      Serial.println("UPDATE OK");Serial.flush();delay(200);ESP.restart();
    }return;
  }
  while(Serial.available()){
    char c=Serial.read();lastInteraction=millis();
    if(c=='\r')continue;
    if(c!='\n'){if(serialLine.length()<128)serialLine+=c;else serialLine="";continue;}
    String line=serialLine;serialLine="";line.trim();
    if(serialMode==UartUpdateState::Header){
      unsigned size=0;char md5[33]={0};
      if(sscanf(line.c_str(),"BEGIN %u %32s",&size,md5)!=2||!size||strlen(md5)!=32||size>ESP.getFreeSketchSpace()){abortUpdate();continue;}
      bool valid=true;for(unsigned i=0;i<32;i++)if(!isxdigit((unsigned char)md5[i]))valid=false;
      if(!valid){abortUpdate();continue;}
      updateSize=size;updateMd5=md5;updateReceived=0;updating=true;serialMode=UartUpdateState::WaitWorker;updateLast=millis();
    }else if(line=="UPDATE"||line=="BOOTLOADER"){
      serialActive=true;serialMode=UartUpdateState::Header;updateLast=millis();Serial.println("UPDATE MODE");
    }else if(line=="AP"){startAP();}
    else if(line=="IP"){
      Serial.printf("STA=%s AP=%s gateway=%s RSSI=%d dBm\n",WiFi.localIP().toString().c_str(),ap?"192.168.4.1":"off",WiFi.gatewayIP().toString().c_str(),WiFi.RSSI());
    }else if(line=="STATUS"){
      Serial.printf("WiFi=%d AP=%d polling=%lus sleep=%s busy=%d storage=%d heap=%u FS=%u/%u\n",WiFi.status(),ap,(unsigned long)cfg.interval,cfg.sleep.c_str(),workerBusy,storageOK,ESP.getFreeHeap(),(unsigned)LittleFS.usedBytes(),(unsigned)LittleFS.totalBytes());
      xSemaphoreTake(stateMutex,portMAX_DELAY);Serial.printf("success=%lu failure=%lu lastCheck=%lus message=%s\n",(unsigned long)successCount,(unsigned long)failureCount,(unsigned long)(lastCheck?(millis()-lastCheck)/1000:0),message.c_str());xSemaphoreGive(stateMutex);
    }else if(line=="AUTH_DIAG"){
      Serial.printf("[auth] paused=%d retry_in=%lus\n",authPaused,(unsigned long)((remainingCooldown(millis(),authCooldownStart,authCooldownDuration)+999)/1000));
      xSemaphoreTake(stateMutex,portMAX_DELAY);Serial.println(authDiagnostic);xSemaphoreGive(stateMutex);
    }else if(line=="AUTH_PAUSE"){
      authPaused=true;
      Serial.println(saveAuthPause(true)?"AUTH PAUSED (an in-flight request may finish)":"AUTH PAUSED for this boot only");
    }else if(line=="AUTH_RESUME"){
      if(saveAuthPause(false)){authPaused=false;forcePoll=true;Serial.println("AUTH RESUMED (existing cooldown retained)");}
      else Serial.println("AUTH RESUME ERROR: could not save setting");
    }else if(line=="SMS_TEST_ONE"){
      if(!storageOK||updating||WiFi.status()!=WL_CONNECTED)Serial.println("SMS_TEST_ONE ERROR: device offline, updating, or storage unavailable");
      else{singleSmsRequested=true;Serial.println("SMS_TEST_ONE QUEUED");}
    }
    else if(line=="AUTH_SELFTEST"){authSelfTest();}
    else if(line=="TEST"){
      if(WiFi.status()!=WL_CONNECTED)Serial.println("TEST ERROR: Wi-Fi disconnected");
      else {testRequested=true;Serial.println("TEST QUEUED");}
    }else if(line=="POLL"){forcePoll=true;Serial.println("POLL QUEUED");}
    else if(line=="REBOOT"){updating=true;restartAt=millis()+100;}
    else if(line=="HELP"){Serial.println("HELP | IP | STATUS | AUTH_DIAG | AUTH_PAUSE | AUTH_RESUME | AUTH_SELFTEST | TEST (WeCom text) | SMS_TEST_ONE | POLL | AP | REBOOT | UPDATE | BOOTLOADER (application updater)");}
    else if(line.length())Serial.println("Unknown command. Type HELP.");
  }
}
void sleepTick(){
  if(cfg.sleep!="light"||ap||updating||serialActive||wifiScanning||workerBusy||digitalRead(BUTTON)==LOW||!lastCheck||!elapsed(millis(),lastInteraction,60000))return;
  // Worker is idle until next interval. Stop radio explicitly for real light sleep.
  if(!elapsed(millis(),lastCheck,100))return;
  uint32_t passed=millis()-lastCheck;if(passed>=cfg.interval*1000)return;
  if(xSemaphoreTake(workGate,0)!=pdTRUE)return;
  uint64_t us=(uint64_t)(cfg.interval*1000-passed)*1000;
  updating=true;WiFi.disconnect(false);WiFi.mode(WIFI_OFF);digitalWrite(LED,LOW);
  esp_sleep_enable_timer_wakeup(us);esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON,0);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON);rtc_gpio_pulldown_dis((gpio_num_t)BUTTON);
  uart_set_wakeup_threshold(UART_NUM_0,3);esp_sleep_enable_uart_wakeup(UART_NUM_0);
  Serial.flush();esp_light_sleep_start();
  auto reason=esp_sleep_get_wakeup_cause();esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  rtc_gpio_deinit((gpio_num_t)BUTTON);pinMode(BUTTON,INPUT_PULLUP);
  WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.begin(cfg.ssid.c_str(),cfg.wifiPass.c_str());lastConnect=millis();resetSession=true;
  if(reason!=ESP_SLEEP_WAKEUP_TIMER){lastInteraction=millis();if(reason==ESP_SLEEP_WAKEUP_EXT0)startAP();}
  forcePoll=true;updating=false;xSemaphoreGive(workGate);
}
void setup(){
  pinMode(LED,OUTPUT);pinMode(BUTTON,INPUT_PULLUP);Serial.setRxBufferSize(4096);Serial.begin(115200);Serial.setTimeout(100);
  stateMutex=xSemaphoreCreateMutex();workGate=xSemaphoreCreateMutex();loadConfig();loadBlob();lastInteraction=millis();
  authTestsOK=authSelfTest();
  // Format only on the very first boot, never erase an existing damaged queue silently.
  Preferences p;p.begin("smscfg",false);bool initialized=p.getBool("fsinit",false);
  authPaused=p.getBool("authpaused",AUTH_PAUSE_ON_UPGRADE&&cfg.ssid.length());
  storageOK=LittleFS.begin(!initialized);if(storageOK){p.putBool("fsinit",true);LittleFS.mkdir("/q");}p.end();
  if(!storageOK)status("文件系统无法挂载，停止转发以保护成功记录");
  WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);
  if(cfg.ssid.length()){WiFi.begin(cfg.ssid.c_str(),cfg.wifiPass.c_str());lastConnect=millis();}
  else startAP();
  setupWeb();configTime(0,0,"ntp.aliyun.com","pool.ntp.org","time.cloudflare.com");
  xTaskCreatePinnedToCore(worker,"sms",16384,nullptr,1,nullptr,0);
  Serial.println("SMS forwarder ready. HELP for UART commands.");
}
void loop(){
  serialTick();buttonTick();web.handleClient();if(ap)dns.processNextRequest();
  if(wifiScanning&&WiFi.scanComplete()!=WIFI_SCAN_RUNNING)wifiScanning=false;
  bool connected=WiFi.status()==WL_CONNECTED;
  static bool previous=false;
  if(connected&&!previous){resetSession=true;forcePoll=true;Serial.println("Wi-Fi IP: "+WiFi.localIP().toString());}
  previous=connected;
  if(!connected && elapsed(millis(),lastConnect,20000)){
    startAP();if(!wifiScanning&&cfg.ssid.length()&&elapsed(millis(),lastConnect,30000)){WiFi.begin(cfg.ssid.c_str(),cfg.wifiPass.c_str());lastConnect=millis();}
  }
  if(ap&&connected&&WiFi.softAPgetStationNum()==0&&elapsed(millis(),lastInteraction,60000)){
    dns.stop();WiFi.softAPdisconnect(true);ap=false;WiFi.mode(WIFI_STA);
  }
  static int sleepSetting=-1;int target=(connected&&!ap&&cfg.sleep=="modem")?1:0;
  if(target!=sleepSetting){WiFi.setSleep(target);sleepSetting=target;}
  if(digitalRead(BUTTON)!=LOW){uint32_t t=millis();bool on=updating?(t%200<100):ap?(t%1600<100||(t%1600>=250&&t%1600<350)):connected?(t%2000<70):(t%400<200);digitalWrite(LED,on);}
  if(restartAt && (int32_t)(millis()-restartAt)>=0){updating=true;if(xSemaphoreTake(workGate,0)==pdTRUE)ESP.restart();}
  sleepTick();delay(5);
}
