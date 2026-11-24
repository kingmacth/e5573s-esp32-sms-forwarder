#pragma once
#include <Arduino.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

inline String sha256(const String &s) {
  unsigned char hash[32]; char hex[65];
  mbedtls_sha256_ret((const unsigned char*)s.c_str(), s.length(), hash, 0);
  for (int i=0;i<32;i++) sprintf(hex+i*2,"%02x",hash[i]);
  hex[64]=0; return hex;
}
inline String b64(const String &s) {
  size_t n=0; unsigned char out[512];
  if (mbedtls_base64_encode(out,sizeof(out)-1,&n,(const unsigned char*)s.c_str(),s.length())) return "";
  out[n]=0; return (const char*)out;
}
inline String tag(const String &xml,const String &name) {
  String open="<"+name+">", close="</"+name+">";
  int a=xml.indexOf(open); if(a<0) return "";
  a+=open.length(); int b=xml.indexOf(close,a);
  return b<0?String(""):xml.substring(a,b);
}
// Trim protocol scalars only; preserve whitespace in SMS content.
inline String scalarTag(const String &xml,const String &name) {
  String value=tag(xml,name);value.trim();return value;
}
inline String hilinkPassword(const String &user,const String &password,const String &token) {
  return b64(sha256(user+b64(sha256(password))+token));
}
inline uint32_t remainingCooldown(uint32_t now,uint32_t start,uint32_t duration) {
  uint32_t passed=now-start;return passed>=duration?0:duration-passed;
}
inline uint32_t loginRetryDelay(unsigned failures) {
  return failures<=1?300000UL:failures==2?600000UL:failures==3?1200000UL:1800000UL;
}
inline bool authSelfTest() {
  bool parse=scalarTag("<response><password_type>4</password_type></response>","password_type")=="4"
    &&scalarTag("<password_type>\r\n 4 \t</password_type>","password_type")=="4"
    &&scalarTag("<password_type>4","password_type").isEmpty();
  bool hash=hilinkPassword("admin","test-password","0123456789abcdef0123456789abcdef")
    =="NGFkYjgxZTc0ZTA4MDgyMWUzMDRmYjgyMjI5ZThkMWNjMTk1NjQ0ZmRjMjVkMDUyNTk0NmUzYzg2OWFmMzcwZg==";
  bool backoff=remainingCooldown(0x20,0xfffffff0,100)==52
    &&remainingCooldown(0x60,0xfffffff0,100)==0&&remainingCooldown(123,0,0)==0
    &&loginRetryDelay(1)==300000&&loginRetryDelay(2)==600000
    &&loginRetryDelay(3)==1200000&&loginRetryDelay(4)==1800000&&loginRetryDelay(100)==1800000;
  Serial.printf("[auth-selftest] XML=%s HASH=%s BACKOFF=%s\n",parse?"PASS":"FAIL",hash?"PASS":"FAIL",backoff?"PASS":"FAIL");
  return parse&&hash&&backoff;
}
inline String xmlEscape(String s) {
  s.replace("&","&amp;"); s.replace("<","&lt;"); s.replace(">","&gt;");
  s.replace("\"","&quot;"); s.replace("'","&apos;"); return s;
}
inline String utf8(uint32_t c) {
  char b[5]={0};
  if(c<0x80)b[0]=c;
  else if(c<0x800){b[0]=0xc0|(c>>6);b[1]=0x80|(c&63);}
  else if(c<0x10000){b[0]=0xe0|(c>>12);b[1]=0x80|((c>>6)&63);b[2]=0x80|(c&63);}
  else if(c<=0x10ffff){b[0]=0xf0|(c>>18);b[1]=0x80|((c>>12)&63);b[2]=0x80|((c>>6)&63);b[3]=0x80|(c&63);}
  return b;
}
inline String xmlDecode(const String &s) {
  if(s.startsWith("<![CDATA[") && s.endsWith("]]>"))return s.substring(9,s.length()-3);
  String out;
  for(unsigned i=0;i<s.length();i++) {
    if(s[i]!='&'){out+=s[i];continue;}
    int end=s.indexOf(';',i); if(end<0 || end-(int)i>12){out+=s[i];continue;}
    String e=s.substring(i+1,end), v;
    if(e=="amp")v="&";else if(e=="lt")v="<";else if(e=="gt")v=">";
    else if(e=="quot")v="\"";else if(e=="apos")v="'";
    else if(e.startsWith("#")){bool h=e.startsWith("#x")||e.startsWith("#X");char *tail;
      uint32_t c=strtoul(e.c_str()+(h?2:1),&tail,h?16:10);
      if(*tail || !c || c>0x10ffff || (c>=0xd800&&c<=0xdfff)){out+=s[i];continue;}v=utf8(c);
    } else {out+=s[i];continue;}
    out+=v;i=end;
  } return out;
}
// Never split a UTF-8 code point at a chunk boundary.
inline size_t chunkBytes(const String &s,size_t offset,size_t limit) {
  size_t end=min((size_t)s.length(),offset+limit);
  if(end<s.length())while(end>offset && (((uint8_t)s[end]&0xc0)==0x80))--end;
  return end-offset;
}
inline bool validWebhook(const String &s) {
  const String p="https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=";
  if(!s.startsWith(p)||s.length()<=p.length()||s.length()>200)return false;
  for(unsigned i=p.length();i<s.length();i++)if(!isalnum((unsigned char)s[i])&&s[i]!='-')return false;
  return true;
}
