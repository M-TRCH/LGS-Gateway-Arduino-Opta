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
:root{--bg:#e6f2f7;--card:#fff;--ink:#0d2b3a;--mut:#46697a;--line:#bcd6e2;--brand:#0a7ea4;--ok:#0f8a4f;--warn:#9a6700;--bad:#c62828}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;background:var(--bg);color:var(--ink)}
header{background:var(--brand);color:#fff;padding:12px 16px;font-weight:600}nav{display:flex;gap:6px;padding:10px 12px;flex-wrap:wrap}
nav button{flex:1;min-width:72px;padding:10px;border:1px solid var(--line);border-radius:10px;background:var(--card);font:inherit}
nav button.on{background:var(--brand);color:#fff;border-color:transparent}main{padding:0 12px 24px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin-bottom:12px}
pre{white-space:pre-wrap;word-break:break-all;font:12px/1.4 ui-monospace,Consolas,monospace;margin:0}
.grid{display:grid;grid-template-columns:repeat(8,1fr);gap:4px}.c{padding:6px 0;text-align:center;border-radius:6px;font-size:12px;background:#eee}
.ok{background:#dff5e8;color:var(--ok)}.slow{background:#fff1cc;color:var(--warn)}.silent{background:#fde0e0;color:var(--bad)}
label{display:block;font-size:13px;color:var(--mut);margin-top:8px}input,select{width:100%;padding:9px;border:1px solid var(--line);border-radius:8px;font:inherit}
.b{margin-top:10px;width:100%;padding:11px;border:0;border-radius:10px;background:var(--brand);color:#fff;font:inherit;font-weight:600}
.b.r{background:var(--bad)}.m{color:var(--mut);font-size:13px}</style></head><body>
<header id="h">LGS service</header>
<nav><button data-t="s" class="on">สถานะ</button><button data-t="l">บันทึก</button><button data-t="m">โมดูล</button><button data-t="n">เครือข่าย</button></nav>
<main>
<section id="s" class="card"><pre id="so">...</pre></section>
<section id="l" class="card" hidden><pre id="lo">...</pre></section>
<section id="m" class="card" hidden><div class="m" id="ms">กดเริ่มเพื่อไล่ทุกโมดูล</div><div class="grid" id="mg"></div><button class="b" id="mb">เริ่มไล่โมดูล</button></section>
<section id="n" class="card" hidden><div id="nf"></div><button class="b" id="nb">บันทึกการตั้งค่า</button><button class="b r" id="rb">รีบูตเกตเวย์</button><pre id="no" class="m"></pre></section>
</main><script>
const K=["sys.name","net.dhcp","net.ip","net.mask","net.gw","net.dns","net.ntp"];
async function cmd(l){const r=await fetch("/api?l="+encodeURIComponent(l),{cache:"no-store"});return r.text()}
function kv(t){const o={};t.split("\n").forEach(x=>{if(x.startsWith("#OK GET "))x="#DATA "+x.slice(8);else if(!x.startsWith("#DATA"))return;x.slice(6).split(" ").forEach(p=>{const i=p.indexOf("=");if(i>0)o[p.slice(0,i)]=p.slice(i+1)})});return o}
const $=i=>document.getElementById(i);
document.querySelectorAll("nav button").forEach(b=>b.onclick=()=>{document.querySelectorAll("nav button").forEach(x=>x.classList.toggle("on",x===b));
 ["s","l","m","n"].forEach(i=>$(i).hidden=i!==b.dataset.t);load(b.dataset.t)});
async function load(t){try{if(t==="s"){const x=await cmd("INFO");$("so").textContent=x.replace(/#DATA /g,"");const o=kv(x);$("h").textContent="LGS service · "+(o["sys.name"]||"")+" · fw "+(o.fw||"")}
 if(t==="l"){$("lo").textContent=(await cmd("LOG 20")).replace(/#DATA /g,"")}
 if(t==="n"){const o={};for(const k of K){const r=kv(await cmd("GET "+k));Object.assign(o,r)}
  $("nf").innerHTML=K.map(k=>'<label>'+k+'</label><input id="k_'+k+'" value="'+(o[k]||"").replace(/"/g,"")+'">').join("")}
}catch(e){}}
async function sweep(){const x=await cmd("SWEEP STATUS"),rows=x.split("\n").filter(l=>l.startsWith("#DATA id="));const h=kv(x.split("\n")[0]);
 $("ms").textContent=(h.state||"")+" "+(h.done||0)+"/"+(h.total||0)+" · ok "+(h.ok||0)+" · ช้า "+(h.slow||0)+" · เงียบ "+(h.silent||0);
 $("mg").innerHTML=rows.map(r=>{const o={};r.slice(6).split(" ").forEach(p=>{const i=p.indexOf("=");o[p.slice(0,i)]=p.slice(i+1)});return '<div class="c '+o.st+'">'+o.id+'</div>'}).join("");
 if(h.state==="running")setTimeout(sweep,1000)}
$("mb").onclick=async()=>{await cmd("SWEEP START");setTimeout(sweep,500)};
$("nb").onclick=async()=>{const h=await cmd("HELLO web");if(!h.includes("#OK")){$("no").textContent=h;return}
 const s=K.map(k=>k+"="+$("k_"+k).value.trim()).filter(p=>!p.endsWith("=")).join(" ");let out=await cmd("SET "+s);out+=await cmd("SAVE");$("no").textContent=out};
$("rb").onclick=async()=>{if(!confirm("รีบูตเกตเวย์? AP จะปิด ต้องเปิดใหม่หลังบูต"))return;await cmd("HELLO web");$("no").textContent=await cmd("REBOOT")};
load("s");
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
