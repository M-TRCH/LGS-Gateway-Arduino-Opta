#include "ap_http.h"

#include <string.h>

#include "gw_console.h"
#include "gw_status.h"
#include "wifi_ap.h"

#define REQ_MAX        320       // request line + headers we keep (the rest is drained)
#define HDR_DRAIN_MAX  2048
#define HDR_WAIT_MS    1500
#define LINE_MAX_API   120       // same gate as the unit 255 tunnel
#define OUT_MAX        8192

// One file, no external fetches, no favicon (a second request would take the other client slot).
static const char PAGE[] = R"HTML(<!doctype html><html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><link rel="icon" href="data:,">
<title>LGS service</title><style>
:root{--bg:#e6f2f7;--card:#fff;--ink:#0d2b3a;--mut:#46697a;--line:#bcd6e2;--s2:#d9ebf3;--brand:#0a7ea4;--ok:#0f8a4f;--warn:#9a6700;--bad:#c62828}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,"Segoe UI","Noto Sans Thai","Leelawadee UI",Tahoma,sans-serif;background:var(--bg);color:var(--ink)}
header{background:var(--brand);color:#fff;padding:12px 16px;font-weight:600;display:flex;justify-content:space-between;gap:8px}header small{font-weight:400;opacity:.85}
nav{display:flex;gap:6px;padding:10px 12px}nav button{flex:1;padding:10px;border:1px solid var(--line);border-radius:10px;background:var(--card);color:var(--ink);font:inherit}
nav button.on{background:var(--brand);color:#fff;border-color:transparent}main{padding:0 12px 24px;max-width:760px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin-bottom:12px}h3{margin:0 0 8px;font-size:14px;color:var(--mut);font-weight:600}
.kv{display:grid;grid-template-columns:auto 1fr;gap:3px 12px;font-size:14px}.kv b{font-weight:400;color:var(--mut)}.kv span{word-break:break-all}
.t{width:100%;border-collapse:collapse;font-size:13px}.t td,.t th{padding:5px 6px;border-bottom:1px solid var(--s2);text-align:left}.t th{color:var(--mut);font-weight:600}
.tag{display:inline-block;padding:1px 8px;border-radius:99px;font-size:12px;background:var(--s2)}.ok{color:var(--ok)}.slow{color:var(--warn)}.silent{color:var(--bad)}
label{display:block;font-size:13px;color:var(--mut);margin-top:8px}input,select{width:100%;padding:9px;border:1px solid var(--line);border-radius:8px;font:inherit;background:#fff;color:var(--ink)}
.b{margin-top:10px;width:100%;padding:11px;border:0;border-radius:10px;background:var(--brand);color:#fff;font:inherit;font-weight:600}.b.r{background:var(--bad)}.m{color:var(--mut);font-size:13px}
</style></head><body>
<header><span id="h">LGS service</span><small id="hv"></small></header>
<nav><button data-t="g" class="on">เกตเวย์</button><button data-t="e">เหตุการณ์</button><button data-t="m">โมดูล</button></nav>
<main>
<section id="g"><div class="card"><h3>สถานะ</h3><div class="kv" id="gs">...</div></div>
<div class="card"><h3>ตั้งค่า</h3><div id="gf"></div><button class="b" id="sv">บันทึกการตั้งค่า</button><button class="b r" id="rb">รีบูตเกตเวย์</button><div class="m" id="go"></div></div></section>
<section id="e" hidden><div class="card"><select id="ef"><option value="">ทุกชนิด</option></select><div class="m" id="en"></div><table class="t"><tbody id="et"></tbody></table></div></section>
<section id="m" hidden><div class="card"><div class="m" id="ms"></div><table class="t"><thead><tr><th>ID</th><th>ชนิด</th><th>เฟิร์มแวร์</th><th>สถานะ</th></tr></thead><tbody id="mt"></tbody></table><button class="b" id="mb">อ่านโมดูลอีกครั้ง</button></div></section>
</main><script>
const K=["sys.name","net.dhcp","net.ip","net.mask","net.gw","net.dns","net.ntp","time.tz_min"],$=i=>document.getElementById(i),TY={10:"STANDARD",20:"NARCOTIC",30:"LITE",40:"DELIVERY"};
let EV=[],CUR="g",MS=0;
async function cmd(l){const r=await fetch("/api?l="+encodeURIComponent(l),{cache:"no-store"});return r.text()}
function kv(s){const o={};s.split(" ").forEach(p=>{const i=p.indexOf("=");if(i>0)o[p.slice(0,i)]=p.slice(i+1)});return o}
function rows(t){return t.split("\n").filter(l=>l.startsWith("#DATA ")).map(l=>kv(l.slice(6)))}
function fw(n){n=+n;return n>=10000?"v"+Math.floor(n/10000)+"."+Math.floor(n/100)%100+"."+n%100:"รุ่นเก่า"}
function up(s){s=+s;const d=Math.floor(s/86400),h=Math.floor(s/3600)%24,m=Math.floor(s/60)%60;return (d?d+" วัน ":"")+h+" ชม. "+m+" นาที"}
function line(a){return a.map(x=>"<b>"+x[0]+"</b><span>"+x[1]+"</span>").join("")}
document.querySelectorAll("nav button").forEach(b=>b.onclick=()=>{document.querySelectorAll("nav button").forEach(x=>x.classList.toggle("on",x===b));
 CUR=b.dataset.t;["g","e","m"].forEach(i=>$(i).hidden=i!==CUR);load()});
async function load(){try{
 if(CUR==="g"){const o=Object.assign({},...rows(await cmd("INFO")));
  $("h").textContent=o["sys.name"]||"LGS service";$("hv").textContent="fw "+(o.fw||"");
  $("gs").innerHTML=line([["เปิดมาแล้ว",up(o["sys.up"])],["เครือข่าย",(o["net.state"]||"")+" · "+(o["net.ip"]||"")],["ไคลเอนต์ TCP",o["net.client"]||0],
   ["เวลา",(o["time.now"]||"").replace("T"," ")+(o["time.set"]==="1"?"":" (ยังไม่ตั้ง)")],["รีเซตตู้",o["sched.reset"]||"-"],
   ["RS485 ตอบ / ไม่ตอบ",(o["cnt.rs485_ok"]||0)+" / "+(o["cnt.rs485_timeout"]||0)],["USB / TCP",(o["cnt.usb_ok"]||0)+" / "+(o["cnt.tcp_ok"]||0)],
   ["รีเซตล่าสุดเพราะ",o["sys.reset"]||"-"],["MAC",o.mac||"-"],["บันทึกเหตุการณ์",(o["log.state"]||"")+" · "+(o["log.n"]||0)+" รายการ"]]);
  if(!$("gf").children.length){const v={};for(const k of K)Object.assign(v,kv((await cmd("GET "+k)).replace(/^#OK GET /,"")));
   $("gf").innerHTML=K.map(k=>'<label>'+k+'</label><input id="k_'+k+'" value="'+(v[k]||"").replace(/"/g,"")+'">').join("")}}
 if(CUR==="e"){const t=await cmd("LOG 60");EV=rows(t);const s=$("ef"),cur=s.value,ty=[...new Set(EV.map(r=>r.ev))].sort();
  s.innerHTML='<option value="">ทุกชนิด</option>'+ty.map(x=>'<option'+(x===cur?" selected":"")+'>'+x+'</option>').join("");showEv()}
 if(CUR==="m"){let t=await cmd("SWEEP STATUS");if(t.includes("state=idle")||!MS){await cmd("SWEEP START");MS=1;t=await cmd("SWEEP STATUS")}paintMods(t)}
}catch(e){}}
function showEv(){const f=$("ef").value,l=EV.filter(r=>!f||r.ev===f);$("en").textContent="ล่าสุด "+l.length+" รายการ";
 $("et").innerHTML=l.map(r=>"<tr><td>"+(r.t||"-").replace("T","<br>")+"</td><td><span class=tag>"+r.ev+"</span></td><td>"+r.a+" / "+r.p+"</td></tr>").join("")}
$("ef").onchange=showEv;
function paintMods(t){const L=t.split("\n"),h=kv((L.find(l=>l.startsWith("#DATA state="))||"").slice(6)),r=rows(t).filter(x=>x.id);
 $("ms").textContent=(h.state==="running"?"กำลังอ่าน ":"อ่านแล้ว ")+(h.done||0)+"/"+(h.total||0)+" · ปกติ "+(h.ok||0)+" · ช้า "+(h.slow||0)+" · เงียบ "+(h.silent||0);
 $("mt").innerHTML=r.map(x=>"<tr><td>"+x.id+"</td><td>"+(x.st==="silent"?"-":TY[x.ty]||x.ty)+"</td><td>"+(x.st==="silent"?"-":fw(x.fw))+"</td><td class="+x.st+">"+({ok:"ปกติ",slow:"ช้า",silent:"เงียบ"}[x.st]||x.st)+"</td></tr>").join("");
 if(h.state==="running"&&CUR==="m")setTimeout(async()=>{if(CUR==="m")paintMods(await cmd("SWEEP STATUS"))},1200)}
$("mb").onclick=async()=>{await cmd("SWEEP START");MS=1;setTimeout(async()=>paintMods(await cmd("SWEEP STATUS")),600)};
$("sv").onclick=async()=>{const h=await cmd("HELLO web");if(!h.includes("#OK")){$("go").textContent=h;return}
 const s=K.map(k=>k+"="+$("k_"+k).value.trim()).filter(p=>!p.endsWith("=")).join(" ");$("go").textContent=(await cmd("SET "+s))+(await cmd("SAVE"))};
$("rb").onclick=async()=>{if(!confirm("รีบูตเกตเวย์? AP จะปิด ต้องเปิดใหม่หลังบูต"))return;await cmd("HELLO web");$("go").textContent=await cmd("REBOOT")};
load();
</script></body></html>)HTML";

static char _out[OUT_MAX];
static size_t _outLen = 0;
static void sink(const char* line) {
    const size_t n = strlen(line);
    if (_outLen + n + 1 >= OUT_MAX) return;
    memcpy(_out + _outLen, line, n);
    _outLen += n;
    _out[_outLen++] = '\n';
}

static void sendAll(EthernetClient& cl, const char* p, size_t n) {
    while (n) {
        const size_t k = n > 1024 ? 1024 : n;
        gwStatus_watchdogKick();
        const size_t w = cl.write((const uint8_t*)p, k);
        if (w == 0) return;
        p += w;
        n -= w;
    }
}

static void reply(EthernetClient& cl, const char* status, const char* type, const char* body, size_t len) {
    char h[200];
    const int hn = snprintf(h, sizeof(h),
                            "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                            status, type, (unsigned)len);
    sendAll(cl, h, (size_t)hn);
    if (len) sendAll(cl, body, len);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decode `src` (up to a space or the end) into `dst`; false when it is not 1..LINE_MAX_API printable bytes.
static bool decodeLine(const char* src, char* dst) {
    size_t n = 0;
    for (const char* p = src; *p && *p != ' ' && *p != '&'; p++) {
        char c = *p;
        if (c == '+') c = ' ';
        else if (c == '%') {
            const int a = hexv(p[1]), b = a < 0 ? -1 : hexv(p[2]);
            if (b < 0) return false;
            c = (char)(a * 16 + b);
            p += 2;
        }
        if (c < 0x20 || c > 0x7E || n >= LINE_MAX_API) return false;
        dst[n++] = c;
    }
    dst[n] = 0;
    return n > 0;
}

void apHttp_serve(EthernetClient& cl, const uint8_t* head, int headLen) {
    wifiAp_touch();
    char req[REQ_MAX + 1];
    int n = 0;
    for (int i = 0; i < headLen && n < REQ_MAX; i++) req[n++] = (char)head[i];
    // The request line, then the headers up to the blank line (kept up to REQ_MAX, drained up to HDR_DRAIN_MAX).
    const uint32_t t0 = millis();
    int seen = n, crlf = 0;
    while (millis() - t0 < HDR_WAIT_MS && seen < HDR_DRAIN_MAX) {
        if (!cl.available()) { if (!cl.connected()) break; delay(2); continue; }
        const int c = cl.read();
        if (c < 0) break;
        seen++;
        if (n < REQ_MAX) req[n++] = (char)c;
        crlf = (c == '\r' || c == '\n') ? crlf + 1 : 0;
        if (crlf >= 4 || (crlf >= 2 && c == '\n' && n > 1 && req[n - 2] == '\n')) break;
    }
    req[n] = 0;
    gwStatus_watchdogKick();

    // "GET <path> HTTP/1.x"
    const char* path = req + 4;
    if (!strncmp(path, "/api?l=", 7)) {
        char line[LINE_MAX_API + 1];
        if (!decodeLine(path + 7, line)) {
            reply(cl, "400 Bad Request", "text/plain", "#ERR HTTP err=bad_line\n", 23);
        } else {
            _outLen = 0;
            gwConsole_execute(line, sink);
            reply(cl, "200 OK", "text/plain; charset=utf-8", _out, _outLen);
        }
    } else if (!strncmp(path, "/ ", 2) || !strncmp(path, "/index.html", 11)) {
        reply(cl, "200 OK", "text/html; charset=utf-8", PAGE, sizeof(PAGE) - 1);
    } else {
        reply(cl, "404 Not Found", "text/plain", "not found\n", 10);
    }
    delay(5);
    cl.stop();
}
