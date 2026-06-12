#include "TuneServer.h"
#include "FeatureFlags.h"
#include "ParamScript.h"

#ifndef __EMSCRIPTEN__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fds {
namespace {

// Knobs the CONSOLE set (vs CLI/env): bake only ever writes these into
// the scene script — and never strips a CLI flag's precedence.
std::mutex            gWebSetMu;
std::set<std::string> gWebSet;

// The whole console is one self-contained page: fetches /api/params +
// /api/state, renders knobs grouped by category (badges: green dot =
// script-driven, orange diamond = time-keyed), debounce-POSTs edits to
// /api/set, polls at 400ms so scripted ramps animate the knobs, and the
// bake button writes console-tuned knobs into SCRIPTS/<scene>.params and
// releases them to it.
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
 .row{display:grid;grid-template-columns:18px 220px 1fr 150px 60px 26px;gap:8px;align-items:center;padding:4px 12px;border-top:1px solid #20202e}
 .pin{cursor:pointer;color:#444;user-select:none}
 .pin.on{color:#fc6}
 .row.set{background:#20283a}
 .nm{font-family:ui-monospace,monospace;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:help}
 .row input[type=range]{width:100%}
 .row input[type=number]{width:100%;background:#1d1d29;border:1px solid #333;color:#eee;border-radius:4px;padding:3px 6px}
 .rel{visibility:hidden;border:none;background:none;color:#e8a;cursor:pointer;font-size:14px}
 .row.set .rel{visibility:visible}
 .def{color:#666;font-size:11px;text-align:right}
 #msg{color:#7c8;min-width:120px;text-align:right;font-size:12px}
</style>
<h1>REVIVAL live tune <span id=scene style="color:#667"></span></h1>
<div id=bar>
 <input id=q type=search placeholder="filter (name / category / help)...">
 <button id=bake onclick="bake()" title="write tuned knobs into the scene's SCRIPTS/*.params and hand them to it">bake to script</button>
 <button onclick="copyOut('cli')">copy CLI</button>
 <span id=msg></span>
</div>
<div id=root></div>
<script>
let P=[],focused=null,ST={scene:'',driven:[],keyed:[],web:[]};
const root=document.getElementById('root'),q=document.getElementById('q'),msg=document.getElementById('msg');
function sliderRange(p){
  const d=Math.abs(parseFloat(p.def))||1,v=Math.abs(parseFloat(p.value))||0;
  const hi=Math.max(d*4,v*2,d+10);
  const lo=Math.min(0,parseFloat(p.def),parseFloat(p.value));
  return [lo,hi,p.type==='int'?1:hi/400];
}
const pins=new Set(JSON.parse(localStorage.tunePins||'[]'));
function savePins(){localStorage.tunePins=JSON.stringify([...pins])}
function render(){
  const f=q.value.toLowerCase();
  root.innerHTML='';
  // WORKING SET on top: pinned + console-tuned + script-driven — the
  // knobs that matter for the scene you're tuning right now.
  if(!f){
    const ws=P.filter(p=>pins.has(p.name)||p.set||ST.driven.includes(p.name));
    if(ws.length){
      const d=document.createElement('details');d.open=true;
      d.innerHTML=`<summary>working set <span style="color:#555">(${ws.length})</span></summary>`;
      for(const p of ws)d.appendChild(row(p));
      root.appendChild(d);
    }
  }
  const cats={};
  for(const p of P){
    if(f&&!(p.name.includes(f)||p.cat.includes(f)||p.help.toLowerCase().includes(f)))continue;
    (cats[p.cat]=cats[p.cat]||[]).push(p);
  }
  for(const c of Object.keys(cats).sort()){
    const d=document.createElement('details');d.open=!!f;
    d.innerHTML=`<summary>${c} <span style="color:#555">(${cats[c].length})</span></summary>`;
    for(const p of cats[c])d.appendChild(row(p));
    root.appendChild(d);
  }
}
function row(p){
  const r=document.createElement('div');r.className='row'+(p.set?' set':'');r.dataset.name=p.name;
  const pin=document.createElement('span');pin.className='pin'+(pins.has(p.name)?' on':'');pin.textContent='\u2605';
  pin.title='pin to working set';
  pin.onclick=()=>{pins.has(p.name)?pins.delete(p.name):pins.add(p.name);savePins();render()};
  r.appendChild(pin);
  const nm=document.createElement('div');nm.className='nm';nm.title=p.help;
  const drv=ST.driven.includes(p.name),key=ST.keyed.includes(p.name);
  nm.innerHTML=(drv?'<span style="color:#6c6" title="script-driven">&#9679;</span> ':key?'<span style="color:#fa5" title="time-keyed in script">&#9670;</span> ':'')+p.name;
  r.appendChild(nm);
  let ctl;
  if(p.type==='bool'){
    ctl=document.createElement('input');ctl.type='checkbox';ctl.checked=p.value===true||p.value==='true';
    ctl.onchange=()=>send(p,ctl.checked?'1':'0');
    r.appendChild(ctl);r.appendChild(document.createElement('div'));
  }else{
    const [lo,hi,st]=sliderRange(p);
    ctl=document.createElement('input');ctl.type='range';ctl.min=lo;ctl.max=hi;ctl.step=st;ctl.value=p.value;
    const num=document.createElement('input');num.type='number';num.step=st;num.value=p.value;
    ctl.onpointerdown=()=>focused=p.name;ctl.onpointerup=()=>focused=null;
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
  fetch('/api/all').then(r=>r.json()).then(a=>{
    const j=a.params,st=a;
    ST={scene:st.info.scene,driven:st.info.driven,keyed:st.info.keyed,web:st.web};
    document.getElementById('scene').textContent=ST.scene?('— scene: '+ST.scene):'';
    document.getElementById('bake').textContent='bake to '+(ST.scene||'?')+'.params'+(ST.web.length?' ('+ST.web.length+')':'');
    const open=new Set([...root.querySelectorAll('details[open] summary')].map(s=>s.textContent));
    P=j;render();
    if(open.size)for(const d of root.querySelectorAll('details'))
      d.open=open.has(d.querySelector('summary').textContent);
  });
}
function bake(){
  api('/api/save').then(r=>r.text()).then(t=>{msg.textContent=t;refresh()});
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
setInterval(()=>{if(!focused)refresh()},500);
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

void handleConn(int fd);

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
		// One detached thread per connection: browsers open SPECULATIVE
		// connections that never send a request — a serial loop blocks in
		// recv() on them while real clicks queue in the backlog ('buttons
		// only work after a few tries'). The timeout reaps the idlers.
		std::thread(handleConn, fd).detach();
	}
}

void handleConn(int fd) {
	{
		timeval tv{2, 0};
		::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	}
	char req[8192];
	const ssize_t n = ::recv(fd, req, sizeof req - 1, 0);
	if (n <= 0) { ::close(fd); return; }
	req[n] = 0;
	{
		// "METHOD /path?query HTTP/1.1"
		char *path = strchr(req, ' ');
		if (!path) { ::close(fd); return; }
		++path;
		char *pend = strchr(path, ' ');
		if (pend) *pend = 0;
		char *qs = strchr(path, '?');
		if (qs) *qs++ = 0;

		if (strcmp(path, "/") == 0) {
			respond(fd, "200 OK", "text/html; charset=utf-8", kPage);
		} else if (strcmp(path, "/api/all") == 0) {
			std::string js = "{\"params\":";
			js.reserve(64 * 1024);
			FeatureFlags::dumpParamsJson(js);
			js += ",\"info\":";
			ParamScript_Info(js);
			js += ",\"web\":[";
			{
				std::lock_guard<std::mutex> lk(gWebSetMu);
				bool first = true;
				for (const auto &nm : gWebSet) {
					if (!first) js += ",";
					first = false;
					js += "\"" + nm + "\"";
				}
			}
			js += "]}";
			respond(fd, "200 OK", "application/json", js);
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
			if (ok) {
				std::lock_guard<std::mutex> lk(gWebSetMu);
				gWebSet.insert(name);
			}
			respond(fd, ok ? "200 OK" : "400 Bad Request", "text/plain", ok ? "ok" : "bad");
		} else if (strcmp(path, "/api/unset") == 0) {
			char name[128] = {0};
			const bool ok = queryParam(qs, "name", name, sizeof name)
			             && FeatureFlags::unsetParam(name);
			if (ok) {
				std::lock_guard<std::mutex> lk(gWebSetMu);
				gWebSet.erase(name);
			}
			respond(fd, ok ? "200 OK" : "400 Bad Request", "text/plain", ok ? "ok" : "bad");
		} else if (strcmp(path, "/api/state") == 0) {
			// Scene + script-driven/keyed params + which knobs the console
			// owns — drives the page's badges and the bake button label.
			std::string js = "{\"info\":";
			ParamScript_Info(js);
			js += ",\"web\":[";
			{
				std::lock_guard<std::mutex> lk(gWebSetMu);
				bool first = true;
				for (const auto &n : gWebSet) {
					if (!first) js += ",";
					first = false;
					js += "\"" + n + "\"";
				}
			}
			js += "]}";
			respond(fd, "200 OK", "application/json", js);
		} else if (strcmp(path, "/api/save") == 0) {
			// Bake ONLY console-owned knobs into the scene script, then
			// release them to it (clearSetMark inside the bake — same
			// values, no flash; the script hot-reloads within ~15 frames).
			std::vector<std::pair<std::string, std::string>> all, mine;
			FeatureFlags::dumpSetParams(all);
			{
				std::lock_guard<std::mutex> lk(gWebSetMu);
				for (auto &p : all)
					if (gWebSet.count(p.first)) mine.push_back(p);
			}
			std::string report;
			const bool ok = ParamScript_BakeParams(mine, report);
			if (ok) {
				std::lock_guard<std::mutex> lk(gWebSetMu);
				for (auto &p : mine) gWebSet.erase(p.first);
			}
			respond(fd, ok ? "200 OK" : "409 Conflict", "text/plain", report);
		} else {
			respond(fd, "404 Not Found", "text/plain", "nope");
		}
	}
	::close(fd);
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
