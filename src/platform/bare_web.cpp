//
// bare_web.cpp
//
// BM-DOS - the card's web server: from a browser on the same network, list the
// card's folders, download, upload (files or whole folders), create folders and
// delete.  boot.conf web_server = on, optionally web_password.
//
// Not Circle's CHTTPDaemon: it holds a whole request and a whole response in a
// buffer, which suits small pages and not a 200 MB game.  This server is a
// scheduler task with one listening socket and one client at a time, and moves
// files in 16 KB pieces between the socket and the card:
//
//   GET  /?dir=/dos           the page: the folder, its files, the upload box
//   GET  /get?path=/dos/X     the file, as a download
//   PUT  /put?path=/dos/X     the request body becomes the file, written to
//                             X.part and renamed at the end (a cut transfer
//                             leaves the old file); missing folders are made
//   POST /mkdir?path=/dos/N   a folder
//   POST /delete?path=/dos/X  a file, or a folder and everything in it
//
// The page sends each file of an upload as its own PUT, from JavaScript, with
// the folder structure in the path - no multipart parsing on this side.
//
// Every path goes through bareweb::SafePath (web_util.h): inside the card,
// no "..".  It runs as a task on core 0, so it only works when the frame loop
// yields - DOSBox keeps running, a little slower during a big transfer - and
// its file calls never interleave with DOSBox's inside FatFs.  After uploading
// while DOS runs, RESCAN in DOS makes DOSBox see the new files.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"
#include "web_util.h"

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

static const char FromWeb[] = "web";

namespace {

constexpr unsigned ChunkSize = 16 * 1024;
constexpr unsigned MaxHeader = 8 * 1024;

std::string s_Password;
u16 s_nPort = 80;
bool s_bRunning = false;

std::string Card (const std::string &Path)
{
	return "SD:" + Path;
}

bool SendAll (CSocket *pSocket, const void *pData, unsigned nLength)
{
	const u8 *p = static_cast<const u8 *> (pData);
	while (nLength != 0)
	{
		const int n = pSocket->Send (p, nLength, 0);
		if (n <= 0) return false;
		p += n;
		nLength -= static_cast<unsigned> (n);
	}
	return true;
}

bool SendText (CSocket *pSocket, const std::string &Text)
{
	return SendAll (pSocket, Text.data (), static_cast<unsigned> (Text.size ()));
}

void Respond (CSocket *pSocket, const char *pStatus, const char *pType, const std::string &Body,
	      const char *pExtraHeaders = "")
{
	char Header[256];
	snprintf (Header, sizeof Header,
		  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%sConnection: close\r\n\r\n",
		  pStatus, pType, static_cast<unsigned> (Body.size ()), pExtraHeaders);
	SendText (pSocket, Header);
	SendText (pSocket, Body);
}

struct TEntry
{
	std::string Name;
	bool bFolder;
	u64 nSize;
};

std::vector<TEntry> List (const std::string &Dir)
{
	std::vector<TEntry> Out;
	DIR *pDir = opendir (Card (Dir).c_str ());
	if (pDir == nullptr) return Out;
	while (const dirent *pEntry = readdir (pDir))
	{
		const std::string Name = pEntry->d_name;
		if (Name.empty () || Name == "." || Name == "..") continue;
		struct stat Info;
		const std::string Full = Card (Dir == "/" ? "/" + Name : Dir + "/" + Name);
		if (stat (Full.c_str (), &Info) != 0) continue;
		Out.push_back ({ Name, S_ISDIR (Info.st_mode), static_cast<u64> (Info.st_size) });
	}
	closedir (pDir);
	std::sort (Out.begin (), Out.end (), [] (const TEntry &a, const TEntry &b)
	{
		if (a.bFolder != b.bFolder) return a.bFolder;
		return strcasecmp (a.Name.c_str (), b.Name.c_str ()) < 0;
	});
	return Out;
}

// --- the page ---------------------------------------------------------------------

std::string Page (const std::string &Dir)
{
	using namespace bareweb;
	std::string H;
	H += "<!doctype html><html lang=es><head><meta charset=utf-8>"
	     "<meta name=viewport content='width=device-width,initial-scale=1'>"
	     "<title>BM-DOS - " + HtmlEscape (Dir) + "</title><style>"
	     "body{font:15px system-ui,sans-serif;margin:0;background:#0000aa;color:#fff}"
	     "header{background:#00aaaa;color:#000;padding:.6em 1em;font-weight:bold}"
	     "main{padding:1em;max-width:60em}"
	     "a{color:#ffff55}table{border-collapse:collapse;width:100%;background:#aaa;color:#000}"
	     "td,th{padding:.35em .6em;border-bottom:1px solid #888;text-align:left}"
	     "th{background:#555;color:#fff}td.n{text-align:right;white-space:nowrap}"
	     "td a{color:#0000aa}button{font:inherit;cursor:pointer}"
	     "#drop{border:2px dashed #55ffff;padding:1em;margin:1em 0;text-align:center}"
	     "#drop.on{background:#0000ff}#log{white-space:pre-wrap;font-family:monospace}"
	     "</style></head><body><header>BM-DOS &mdash; tarjeta SD</header><main>";

	// Breadcrumbs.
	H += "<p><a href='/?dir=/'>SD:</a>";
	std::string Acc;
	size_t i = 1;
	while (i < Dir.size ())
	{
		size_t e = Dir.find ('/', i);
		if (e == std::string::npos) e = Dir.size ();
		const std::string Part = Dir.substr (i, e - i);
		Acc += "/" + Part;
		H += " / <a href='/?dir=" + UrlEncode (Acc) + "'>" + HtmlEscape (Part) + "</a>";
		i = e + 1;
	}
	H += "</p>";

	H += "<table><tr><th>Nombre</th><th class=n>Tamaño</th><th></th></tr>";
	if (Dir != "/")
	{
		H += "<tr><td><a href='/?dir=" + UrlEncode (Parent (Dir)) + "'>.. (subir)</a></td><td></td><td></td></tr>";
	}
	for (const TEntry &E : List (Dir))
	{
		const std::string Path = Dir == "/" ? "/" + E.Name : Dir + "/" + E.Name;
		H += "<tr><td>";
		if (E.bFolder)
			H += "<a href='/?dir=" + UrlEncode (Path) + "'>[" + HtmlEscape (E.Name) + "]</a>";
		else
			H += "<a href='/get?path=" + UrlEncode (Path) + "'>" + HtmlEscape (E.Name) + "</a>";
		H += "</td><td class=n>" + std::string (E.bFolder ? "carpeta" : HumanSize (E.nSize)) + "</td>"
		     "<td><button onclick=\"del('" + JsString (UrlEncode (Path)) + "','"
		     + JsString (E.Name) + "'," + (E.bFolder ? "1" : "0") + ")\">Borrar</button></td></tr>";
	}
	H += "</table>";

	H += "<div id=drop>Arrastra aquí ficheros o carpetas para subirlos a <b>" + HtmlEscape (Dir) + "</b>"
	     "<br><br><input type=file id=files multiple> <input type=file id=dirs webkitdirectory multiple>"
	     "</div><p><button onclick=mkdir()>Nueva carpeta</button></p><div id=log></div>"
	     "<p><small>Si DOS está en marcha, escribe <b>RESCAN</b> para que vea lo que subas. "
	     "No cortes la corriente mientras se sube algo.</small></p>";

	H += "<script>const DIR=" + std::string ("'") + JsString (Dir) + "';"
	     "const log=t=>document.getElementById('log').textContent+=t+'\\n';"
	     "function enc(p){return encodeURIComponent(p).replace(/%2F/g,'/')}"
	     "async function put(file,rel){const p=(DIR=='/'?'':DIR)+'/'+rel;"
	     "await new Promise((ok,ko)=>{const x=new XMLHttpRequest();x.open('PUT','/put?path='+enc(p));"
	     "x.upload.onprogress=e=>{document.title=rel+' '+Math.round(100*e.loaded/e.total)+'%'};"
	     "x.onload=()=>{log((x.status==200?'ok    ':'ERROR ')+rel);ok()};x.onerror=()=>{log('ERROR '+rel);ok()};"
	     "x.send(file)})}"
	     "async function upload(list){for(const [f,rel] of list)await put(f,rel);location.reload()}"
	     "document.getElementById('files').onchange=e=>upload([...e.target.files].map(f=>[f,f.name]));"
	     "document.getElementById('dirs').onchange=e=>upload([...e.target.files].map(f=>[f,f.webkitRelativePath]));"
	     "const d=document.getElementById('drop');"
	     "d.ondragover=e=>{e.preventDefault();d.classList.add('on')};d.ondragleave=()=>d.classList.remove('on');"
	     "async function walk(entry,path,out){if(entry.isFile){out.push([await new Promise(r=>entry.file(r)),path+entry.name])}"
	     "else{const r=entry.createReader();let b;do{b=await new Promise(ok=>r.readEntries(ok));"
	     "for(const e of b)await walk(e,path+entry.name+'/',out)}while(b.length)}}"
	     "d.ondrop=async e=>{e.preventDefault();d.classList.remove('on');const out=[];"
	     "for(const it of e.dataTransfer.items){const en=it.webkitGetAsEntry();if(en)await walk(en,'',out)}upload(out)};"
	     "async function del(p,n,dir){if(!confirm('¿Borrar '+n+(dir?' y todo lo que contiene':'')+'?'))return;"
	     "const r=await fetch('/delete?path='+p,{method:'POST'});if(!r.ok)alert(await r.text());location.reload()}"
	     "async function mkdir(){const n=prompt('Nombre de la carpeta:');if(!n)return;"
	     "const r=await fetch('/mkdir?path='+enc((DIR=='/'?'':DIR)+'/'+n),{method:'POST'});"
	     "if(!r.ok)alert(await r.text());location.reload()}"
	     "</script></main></body></html>";
	return H;
}

// --- file operations ---------------------------------------------------------------

bool MakeFolders (const std::string &Dir)
{
	if (Dir == "/" || Dir.empty ()) return true;
	struct stat Info;
	if (stat (Card (Dir).c_str (), &Info) == 0) return S_ISDIR (Info.st_mode);
	if (!MakeFolders (bareweb::Parent (Dir))) return false;
	return mkdir (Card (Dir).c_str (), 0777) == 0;
}

bool RemoveTree (const std::string &Path)
{
	struct stat Info;
	if (stat (Card (Path).c_str (), &Info) != 0) return false;
	if (S_ISDIR (Info.st_mode))
	{
		for (const TEntry &E : List (Path))
		{
			if (!RemoveTree (Path + "/" + E.Name)) return false;
		}
	}
	return remove (Card (Path).c_str ()) == 0;
}

void SendFile (CSocket *pSocket, const std::string &Path)
{
	struct stat Info;
	FILE *pFile = nullptr;
	if (stat (Card (Path).c_str (), &Info) != 0 || S_ISDIR (Info.st_mode)
	    || (pFile = fopen (Card (Path).c_str (), "rb")) == nullptr)
	{
		Respond (pSocket, "404 Not Found", "text/plain; charset=utf-8", "No existe: " + Path);
		return;
	}

	const std::string Name = Path.substr (Path.find_last_of ('/') + 1);
	char Header[512];
	snprintf (Header, sizeof Header,
		  "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %llu\r\n"
		  "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n",
		  static_cast<unsigned long long> (Info.st_size), Name.c_str ());
	if (SendText (pSocket, Header))
	{
		static u8 Buffer[ChunkSize];
		size_t n;
		while ((n = fread (Buffer, 1, sizeof Buffer, pFile)) != 0)
		{
			if (!SendAll (pSocket, Buffer, static_cast<unsigned> (n))) break;
		}
	}
	fclose (pFile);
}

// The body (nLength bytes, nAlready of them already read with the header) to
// Path, through Path.part.
bool ReceiveFile (CSocket *pSocket, const std::string &Path, const std::string &Already, u64 nLength)
{
	if (!MakeFolders (bareweb::Parent (Path))) return false;

	const std::string Part = Card (Path + ".part");
	FILE *pFile = fopen (Part.c_str (), "wb");
	if (pFile == nullptr) return false;

	bool bOk = true;
	u64 nGot = Already.size () < nLength ? Already.size () : nLength;
	if (nGot != 0 && fwrite (Already.data (), 1, nGot, pFile) != nGot) bOk = false;

	static u8 Buffer[ChunkSize];
	while (bOk && nGot < nLength)
	{
		const unsigned nWant = nLength - nGot < ChunkSize ? static_cast<unsigned> (nLength - nGot) : ChunkSize;
		const int n = pSocket->Receive (Buffer, nWant, 0);
		if (n <= 0) { bOk = false; break; }
		if (fwrite (Buffer, 1, static_cast<size_t> (n), pFile) != static_cast<size_t> (n)) bOk = false;
		nGot += static_cast<u64> (n);
	}

	bOk = fclose (pFile) == 0 && bOk;		// f_close: f_sync to the card
	if (!bOk)
	{
		remove (Part.c_str ());
		return false;
	}
	remove (Card (Path).c_str ());
	return rename (Part.c_str (), Card (Path).c_str ()) == 0;
}

// --- one request ----------------------------------------------------------------------

void Handle (CSocket *pSocket)
{
	pSocket->SetOptionReceiveTimeout (15 * 1000000);

	// The header, and whatever of the body came with it.
	std::string Data;
	static char Buffer[2048];
	size_t nHeaderEnd;
	while ((nHeaderEnd = Data.find ("\r\n\r\n")) == std::string::npos)
	{
		if (Data.size () > MaxHeader) return;
		const int n = pSocket->Receive (Buffer, sizeof Buffer, 0);
		if (n <= 0) return;
		Data.append (Buffer, static_cast<size_t> (n));
	}
	const std::string Header = Data.substr (0, nHeaderEnd);
	const std::string Body = Data.substr (nHeaderEnd + 4);

	bareweb::TRequestLine R;
	if (!bareweb::ParseRequestLine (Header.substr (0, Header.find ("\r\n")), &R))
	{
		Respond (pSocket, "400 Bad Request", "text/plain", "Bad request");
		return;
	}

	// Headers that matter: the body's length and the password.
	u64 nLength = 0;
	std::string Auth;
	size_t i = Header.find ("\r\n");
	while (i != std::string::npos)
	{
		const size_t e = Header.find ("\r\n", i + 2);
		const std::string Line = Header.substr (i + 2, e == std::string::npos ? std::string::npos : e - i - 2);
		const size_t c = Line.find (':');
		if (c != std::string::npos)
		{
			std::string Name = Line.substr (0, c);
			for (char &ch : Name) ch = static_cast<char> (tolower (static_cast<unsigned char> (ch)));
			std::string Value = Line.substr (c + 1);
			while (!Value.empty () && Value[0] == ' ') Value.erase (0, 1);
			if (Name == "content-length") nLength = strtoull (Value.c_str (), nullptr, 10);
			else if (Name == "authorization") Auth = Value;
		}
		i = e;
	}

	if (!s_Password.empty () && !bareweb::BasicAuthOk (Auth, s_Password))
	{
		Respond (pSocket, "401 Unauthorized", "text/plain; charset=utf-8",
			 "Hace falta la contraseña (web_password de boot.conf).",
			 "WWW-Authenticate: Basic realm=\"BM-DOS\"\r\n");
		return;
	}

	const std::string Raw = bareweb::QueryParam (R.Query, R.Path == "/" ? "dir" : "path");
	const std::string Path = bareweb::SafePath (Raw.empty () ? "/" : Raw);
	if (Path.empty ())
	{
		Respond (pSocket, "400 Bad Request", "text/plain; charset=utf-8", "Ruta no válida: " + Raw);
		return;
	}

	if (R.Method == "GET" && R.Path == "/")
	{
		Respond (pSocket, "200 OK", "text/html; charset=utf-8", Page (Path));
	}
	else if (R.Method == "GET" && R.Path == "/get")
	{
		SendFile (pSocket, Path);
	}
	else if (R.Method == "PUT" && R.Path == "/put" && Path != "/")
	{
		const bool bOk = ReceiveFile (pSocket, Path, Body, nLength);
		CLogger::Get ()->Write (FromWeb, bOk ? LogNotice : LogWarning, "PUT %s: %s", Path.c_str (),
					bOk ? "ok" : "failed");
		Respond (pSocket, bOk ? "200 OK" : "500 Internal Server Error", "text/plain; charset=utf-8",
			 bOk ? "ok" : "No se pudo escribir " + Path);
	}
	else if (R.Method == "POST" && R.Path == "/mkdir" && Path != "/")
	{
		const bool bOk = MakeFolders (Path);
		Respond (pSocket, bOk ? "200 OK" : "500 Internal Server Error", "text/plain; charset=utf-8",
			 bOk ? "ok" : "No se pudo crear " + Path);
	}
	else if (R.Method == "POST" && R.Path == "/delete" && Path != "/")
	{
		const bool bOk = RemoveTree (Path);
		CLogger::Get ()->Write (FromWeb, LogNotice, "delete %s: %s", Path.c_str (), bOk ? "ok" : "failed");
		Respond (pSocket, bOk ? "200 OK" : "500 Internal Server Error", "text/plain; charset=utf-8",
			 bOk ? "ok" : "No se pudo borrar " + Path);
	}
	else
	{
		Respond (pSocket, "404 Not Found", "text/plain", "Not found");
	}
}

class CWebServer : public CTask
{
public:
	void Run (void) override
	{
		// Wait for the network; bare_net.cpp started it.
		CNetSubSystem *pNet = nullptr;
		while ((pNet = CNetSubSystem::Get ()) == nullptr || !pNet->IsRunning ())
		{
			CScheduler::Get ()->MsSleep (500);
		}

		CSocket Listener (pNet, IPPROTO_TCP);
		if (Listener.Bind (s_nPort) < 0 || Listener.Listen () < 0)
		{
			CLogger::Get ()->Write (FromWeb, LogError, "cannot listen on port %u", s_nPort);
			return;
		}
		s_bRunning = true;
		CLogger::Get ()->Write (FromWeb, LogNotice, "serving the card on port %u", s_nPort);

		for (;;)
		{
			CIPAddress ForeignIP;
			u16 nForeignPort;
			CSocket *pClient = Listener.Accept (&ForeignIP, &nForeignPort);
			if (pClient == nullptr)
			{
				CScheduler::Get ()->MsSleep (100);
				continue;
			}
			Handle (pClient);
			delete pClient;
		}
	}
};

} // namespace

boolean BareWebStart (const char *pPassword, unsigned nPort)
{
	if (BareNetMode () == BareNetOff)
	{
		return FALSE;
	}
	s_Password = pPassword != nullptr ? pPassword : "";
	s_nPort = static_cast<u16> (nPort != 0 ? nPort : 80);
	new CWebServer;
	return TRUE;
}

boolean BareWebRunning (void)
{
	return s_bRunning ? TRUE : FALSE;
}
