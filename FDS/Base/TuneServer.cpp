#include "TuneServer.h"
#include "FeatureFlags.h"

#ifndef __EMSCRIPTEN__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace fds {
namespace {

// The whole console is one self-contained page: fetches /api/params,
// renders knobs grouped by category, debounce-POSTs edits to /api/set,
// polls every 2s to reflect script-driven motion, exports the SET rows
// as CLI flags or params-script lines.
const char kPage[] = R"HTML(<!doctype html>
<meta charset="utf-8"><title>REVIVAL tune</title>
<style>
 body{background:#14141c;color:#cfd2e0;font:13px/1.5 -apple-system,Segoe UI,sans-serif;margin:0;padding:14px}
 h1{font-size:15px;margin:0 0 10px;color:#9aa3ff}
 #bar{position:sticky;top:0;background:#14141c;padding:6px 0;display:flex;gap:8px;align-items:center;z-index:2}
 input[type=search]{flex:1;background:#1d1d29;border:1px solid #333;color:#eee;border-radius:6px;padding:6px 10px}
 button{background:#262638;border:1px solid #444;color:#cfd2e0;border-radius:6px;padding:6px 10px;cursor:pointer}
 button:hover{background:#33334d}
 details{margin:8px 0;border:1px solid #26263a;border-radius:8px;background:#191926}
 summary{cursor:pointer;padding:7px 12px;font-weight:600;color:#8f97e8}
 .row{display:grid;grid-template-columns:220px 1fr 150px 60px 26px;gap:10px;align-items:center;padding:4px 12px;border-top:1px solid #20202e}
 .row.set{background:#20283a}
 .nm{font-family:ui-monospace,monospace;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:help}
 .row input[type=range]{width:100%}
 .row input[type=number]{width:100%;background:#1d1d29;border:1px solid #333;color:#eee;border-radius:4px;padding:3px 6px}
 .rel{visibility:hidden;border:none;background:none;color:#e8a;cursor:pointer;font-size:14px}
 .row.set .rel{visibility:visible}
 .def{color:#666;font-size:11px;text-align:right}
 #msg{color:#7c8;min-width:120px;text-align:right;font-size:12px}
</style>
<h1>REVIVAL live tune</h1>
<div id=bar>
 <input id=q type=search placeholder="filter (name / category / help)...">
 <button onclick="copyOut('cli')">copy CLI</button>
 <button onclick="copyOut('params')">copy .params</button>
 <span id=msg></span>
</div>
<div id=root></div>
<script>
let P=[],focused=null;
const root=document.getElementById('root'),q=document.getElementById('q'),msg=document.getElementById('msg');
function sliderRange(p){
  const d=Math.abs(parseFloat(p.def))||1,v=Math.abs(parseFloat(p.value))||0;
  const hi=Math.max(d*4,v*2,d+10);
  const lo=Math.min(0,parseFloat(p.def),parseFloat(p.value));
  return [lo,hi,p.type==='int'?1:hi/400];
}
function render(){
  const f=q.value.toLowerCase();
  const cats={};
  for(const p of P){
    if(f&&!(p.name.includes(f)||p.cat.includes(f)||p.help.toLowerCase().includes(f)))continue;
    (cats[p.cat]=cats[p.cat]||[]).push(p);
  }
  root.innerHTML='';
  for(const c of Object.keys(cats).sort()){
    const d=document.createElement('details');d.open=!!f||Object.keys(cats).length<4;
    d.innerHTML=`<summary>${c} <span style="color:#555">(${cats[c].length})</span></summary>`;
    for(const p of cats[c])d.appendChild(row(p));
    root.appendChild(d);
  }
}
function row(p){
  const r=document.createElement('div');r.className='row'+(p.set?' set':'');r.dataset.name=p.name;
  const nm=document.createElement('div');nm.className='nm';nm.textContent=p.name;nm.title=p.help;r.appendChild(nm);
  let ctl;
  if(p.type==='bool'){
    ctl=document.createElement('input');ctl.type='checkbox';ctl.checked=p.value===true||p.value==='true';
    ctl.onchange=()=>send(p,ctl.checked?'1':'0');
    r.appendChild(ctl);r.appendChild(document.createElement('div'));
  }else{
    const [lo,hi,st]=sliderRange(p);
    ctl=document.createElement('input');ctl.type='range';ctl.min=lo;ctl.max=hi;ctl.step=st;ctl.value=p.value;
    const num=document.createElement('input');num.type='number';num.step=st;num.value=p.value;
    ctl.oninput=()=>{num.value=ctl.value;send(p,ctl.value)};
    num.onfocus=()=>focused=p.name;num.onblur=()=>focused=null;
    num.onchange=()=>{ctl.value=num.value;send(p,num.value)};
    r.appendChild(ctl);r.appendChild(num);
  }
  const def=document.createElement('div');def.className='def';def.textContent=p.def;def.title='default';r.appendChild(def);
  const rel=document.createElement('button');rel.className='rel';rel.textContent='↺';rel.title='release (back to script/default)';
  rel.onclick=()=>api('/api/unset?name='+p.name).then(refresh);
  r.appendChild(rel);
  return r;
}
let timer=null;
function send(p,v){
  clearTimeout(timer);
  timer=setTimeout(()=>api('/api/set?name='+p.name+'&value='+encodeURIComponent(v)).then(()=>{
    msg.textContent=p.name+' = '+v;
    const e=P.find(x=>x.name===p.name);if(e){e.value=v;e.set=true}
    const r=root.querySelector(`[data-name="${p.name}"]`);if(r)r.classList.add('set');
  }),80);
}
function api(u){return fetch(u,{method:'POST'})}
function refresh(){
  fetch('/api/params').then(r=>r.json()).then(j=>{
    const open=new Set([...root.querySelectorAll('details[open] summary')].map(s=>s.textContent));
    P=j;render();
    if(open.size)for(const d of root.querySelectorAll('details'))
      d.open=open.has(d.querySelector('summary').textContent);
  });
}
function copyOut(kind){
  const set=P.filter(p=>p.set);
  if(!set.length){msg.textContent='nothing set';return}
  const txt=set.map(p=>kind==='cli'
    ?(p.type==='bool'?((p.value===true||p.value==='true')?'--'+p.name:'--no-'+p.name):'--'+p.name+'='+p.value)
    :p.name+' = '+(p.type==='bool'?((p.value===true||p.value==='true')?1:0):p.value)).join(kind==='cli'?' ':'\n');
  navigator.clipboard.writeText(txt).then(()=>msg.textContent='copied '+set.length+' values');
}
q.oninput=render;
refresh();
setInterval(()=>{if(!focused)refresh()},2000);
</script>
)HTML";

int urlDecodeInto(char *dst, const char *src, int cap) {
	int n = 0;
	for (; *src && n < cap - 1; ++src) {
		if (*src == '%' && src[1] && src[2]) {
			char h[3] = { src[1], src[2], 0 };
			dst[n++] = char(strtol(h, nullptr, 16));
			src += 2;
		} else if (*src == '+') {
			dst[n++] = ' ';
		} else {
			dst[n++] = *src;
		}
	}
	dst[n] = 0;
	return n;
}

// Pull "key=value" out of a query string (no allocation; value decoded).
bool queryParam(const char *qs, const char *key, char *out, int cap) {
	const size_t kl = strlen(key);
	while (qs && *qs) {
		const char *amp = strchr(qs, '&');
		if (strncmp(qs, key, kl) == 0 && qs[kl] == '=') {
			char raw[256];
			const char *v = qs + kl + 1;
			const size_t vl = amp ? size_t(amp - v) : strlen(v);
			const size_t n = vl < sizeof(raw) - 1 ? vl : sizeof(raw) - 1;
			memcpy(raw, v, n);
			raw[n] = 0;
			urlDecodeInto(out, raw, cap);
			return true;
		}
		qs = amp ? amp + 1 : nullptr;
	}
	return false;
}

void sendAll(int fd, const char *data, size_t len) {
	while (len) {
		const ssize_t w = ::send(fd, data, len, 0);
		if (w <= 0) return;
		data += w;
		len -= size_t(w);
	}
}

void respond(int fd, const char *status, const char *ctype, const std::string &body) {
	char hdr[256];
	const int hl = snprintf(hdr, sizeof hdr,
	    "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
	    "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
	    status, ctype, body.size());
	sendAll(fd, hdr, size_t(hl));
	sendAll(fd, body.data(), body.size());
}

void serveLoop(int port) {
	const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) return;
	int one = 1;
	::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(uint16_t(port));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost ONLY
	if (::bind(srv, (sockaddr*)&addr, sizeof addr) < 0 || ::listen(srv, 8) < 0) {
		fprintf(stderr, "[TUNE] port %d unavailable — tune server off\n", port);
		::close(srv);
		return;
	}
	fprintf(stderr, "[TUNE] live tuning console: http://localhost:%d\n", port);
	for (;;) {
		const int fd = ::accept(srv, nullptr, nullptr);
		if (fd < 0) continue;
		char req[8192];
		const ssize_t n = ::recv(fd, req, sizeof req - 1, 0);
		if (n <= 0) { ::close(fd); continue; }
		req[n] = 0;
		// "METHOD /path?query HTTP/1.1"
		char *path = strchr(req, ' ');
		if (!path) { ::close(fd); continue; }
		++path;
		char *pend = strchr(path, ' ');
		if (pend) *pend = 0;
		char *qs = strchr(path, '?');
		if (qs) *qs++ = 0;

		if (strcmp(path, "/") == 0) {
			respond(fd, "200 OK", "text/html; charset=utf-8", kPage);
		} else if (strcmp(path, "/api/params") == 0) {
			std::string js;
			js.reserve(64 * 1024);
			FeatureFlags::dumpParamsJson(js);
			respond(fd, "200 OK", "application/json", js);
		} else if (strcmp(path, "/api/set") == 0) {
			char name[128] = {0}, value[256] = {0};
			const bool ok = queryParam(qs, "name", name, sizeof name)
			             && queryParam(qs, "value", value, sizeof value)
			             && FeatureFlags::setParamFromText(name, value);
			respond(fd, ok ? "200 OK" : "400 Bad Request", "text/plain", ok ? "ok" : "bad");
		} else if (strcmp(path, "/api/unset") == 0) {
			char name[128] = {0};
			const bool ok = queryParam(qs, "name", name, sizeof name)
			             && FeatureFlags::unsetParam(name);
			respond(fd, ok ? "200 OK" : "400 Bad Request", "text/plain", ok ? "ok" : "bad");
		} else {
			respond(fd, "404 Not Found", "text/plain", "nope");
		}
		::close(fd);
	}
}

} // namespace

void TuneServer_Start() {
	if (!FeatureFlags::tune_server()) return;
	const int port = FeatureFlags::tune_port();
	std::thread(serveLoop, port).detach();
}

} // namespace fds

#else // __EMSCRIPTEN__

namespace fds {
void TuneServer_Start() {}
} // namespace fds

#endif
