//
// web_util_test.cpp - host test for src/platform/web_util.h (make check-web)
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/web_util.h"

#include <cstdio>

using namespace bareweb;

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

int main (void)
{
	TRequestLine R;
	Check (ParseRequestLine ("GET /get?path=%2Fdos%2FDOOM%20II&x=1 HTTP/1.1", &R)
	       && R.Method == "GET" && R.Path == "/get" && QueryParam (R.Query, "path") == "/dos/DOOM II",
	       "a request line and a decoded query parameter");
	Check (!ParseRequestLine ("GARBAGE", &R) && !ParseRequestLine ("GET nope HTTP/1.1", &R),
	       "malformed request lines are refused");
	Check (QueryParam ("a=1&name=Mi+juego&b=", "name") == "Mi juego" && QueryParam ("a=1", "z").empty (),
	       "'+' is a space in a query; a missing parameter is empty");
	Check (UrlDecode ("caf%C3%A9", false) == "café" && UrlDecode ("100%", false) == "100%",
	       "UTF-8 through %xx; a stray % is kept");
	Check (UrlEncode ("/dos/Mi Juego/ñ.exe") == "/dos/Mi%20Juego/%C3%B1.exe", "links are encoded, '/' kept");

	Check (SafePath ("/dos/GAME/../..") == "" && SafePath ("/dos/../boot.conf") == "",
	       "no way out of a folder with '..'");
	Check (SafePath ("/dos//GAME/./X.EXE") == "/dos/GAME/X.EXE" && SafePath ("\\\\dos\\\\X") == "/dos/X",
	       "double slashes, '.', and backslashes are tidied");
	Check (SafePath ("/") == "/" && SafePath ("dos") == "" && SafePath ("/a:b") == "" && SafePath ("/a\nb") == "",
	       "the root is '/'; relative paths, drive colons and control characters are refused");
	Check (Parent ("/dos/GAME/X.EXE") == "/dos/GAME" && Parent ("/X") == "/" && Parent ("/") == "/",
	       "parent folders");

	Check (HtmlEscape ("<a href=\"x\">&'") == "&lt;a href=&quot;x&quot;&gt;&amp;&#39;", "HTML is escaped");
	Check (Base64Decode ("dXNlcjpzZWNyZXQ=") == "user:secret", "Base64");
	Check (BasicAuthOk ("Basic dXNlcjpzZWNyZXQ=", "secret") && !BasicAuthOk ("Basic dXNlcjp3cm9uZw==", "secret")
	       && !BasicAuthOk ("Bearer x", "secret"), "the password of Basic auth, whatever the user name");
	Check (HumanSize (512) == "512 B" && HumanSize (1536) == "1.5 KB" && HumanSize (3 * 1024 * 1024) == "3.0 MB",
	       "human sizes");

	Check (JsString ("Tom's & <b>\\") == "Tom\\x27s \\x26 \\x3Cb\\x3E\\x5C"
	       && JsString ("/dos/GAMES") == "/dos/GAMES",
	       "names with quotes, & and < survive as a JS string, in <script> and in onclick");

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
