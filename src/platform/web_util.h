//
// web_util.h
//
// BM-DOS - the small, testable pieces of the card's web server (bare_web.cpp):
// request lines, query parameters, URL decoding, the path rules that keep every
// request inside the card, HTML escaping, and Base64 for the password.  No
// Circle: scripts/hostcheck/web_util_test.cpp (make check-web).
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_WEB_UTIL_H
#define BARE_WEB_UTIL_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bareweb {

inline int HexValue (char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/// %xx and (in a query) '+' as a space.  A malformed %xx is kept as it is.
inline std::string UrlDecode (const std::string &In, bool bQuery)
{
	std::string Out;
	for (size_t i = 0; i < In.size (); i++)
	{
		if (In[i] == '%' && i + 2 < In.size () && HexValue (In[i + 1]) >= 0 && HexValue (In[i + 2]) >= 0)
		{
			Out += static_cast<char> (HexValue (In[i + 1]) * 16 + HexValue (In[i + 2]));
			i += 2;
		}
		else if (In[i] == '+' && bQuery)
		{
			Out += ' ';
		}
		else
		{
			Out += In[i];
		}
	}
	return Out;
}

/// For links: everything but unreserved characters and '/' becomes %xx.
inline std::string UrlEncode (const std::string &In)
{
	static const char Hex[] = "0123456789ABCDEF";
	std::string Out;
	for (unsigned char c : In)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		    || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
		{
			Out += static_cast<char> (c);
		}
		else
		{
			Out += '%'; Out += Hex[c >> 4]; Out += Hex[c & 15];
		}
	}
	return Out;
}

inline std::string HtmlEscape (const std::string &In)
{
	std::string Out;
	for (char c : In)
	{
		switch (c)
		{
		case '&': Out += "&amp;"; break;
		case '<': Out += "&lt;"; break;
		case '>': Out += "&gt;"; break;
		case '"': Out += "&quot;"; break;
		case '\'': Out += "&#39;"; break;
		default:  Out += c; break;
		}
	}
	return Out;
}

/// A JavaScript string literal's contents, safe both inside <script> (which
/// the browser does not HTML-decode, so HtmlEscape would put "&amp;" in the
/// path) and inside an HTML attribute such as onclick: quotes, backslash,
/// < > & and control characters become \xNN.  Without quotes around it.
inline std::string JsString (const std::string &In)
{
	static const char Hex[] = "0123456789ABCDEF";
	std::string Out;
	for (unsigned char c : In)
	{
		if (c == '\\' || c == '\'' || c == '"' || c == '<' || c == '>' || c == '&' || c < 0x20)
		{
			Out += "\\x"; Out += Hex[c >> 4]; Out += Hex[c & 15];
		}
		else
		{
			Out += static_cast<char> (c);
		}
	}
	return Out;
}

/// A file or folder name typed in the page (rename, new folder): one name,
/// not a path - no / \\ : * ? " < > |, no control characters, not "." or "..",
/// at most 255 bytes, and not ending in a space or a dot (FAT drops those).
inline bool ValidName (const std::string &Name)
{
	if (Name.empty () || Name.size () > 255 || Name == "." || Name == "..") return false;
	if (Name.back () == ' ' || Name.back () == '.') return false;
	for (unsigned char c : Name)
	{
		if (c < 0x20 || c == 0x7F) return false;
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"'
		    || c == '<' || c == '>' || c == '|') return false;
	}
	return true;
}

/// A web password as boot.conf can keep it (web_password): 1 to 32 printable
/// ASCII characters, no spaces, no '#' (a comment in boot.conf).
inline bool ValidPassword (const std::string &Password)
{
	if (Password.empty () || Password.size () > 32) return false;
	for (unsigned char c : Password)
	{
		if (c <= ' ' || c >= 0x7F || c == '#') return false;
	}
	return true;
}

struct TRequestLine
{
	std::string Method;	// "GET", "PUT", "POST"
	std::string Path;	// decoded, without the query
	std::string Query;	// raw, after '?'
};

/// "GET /get?path=%2Fdos HTTP/1.1" -> GET, /get, path=%2Fdos.  False if malformed.
inline bool ParseRequestLine (const std::string &Line, TRequestLine *pOut)
{
	const size_t a = Line.find (' ');
	if (a == std::string::npos) return false;
	const size_t b = Line.find (' ', a + 1);
	if (b == std::string::npos) return false;
	pOut->Method = Line.substr (0, a);
	const std::string Target = Line.substr (a + 1, b - a - 1);
	if (Target.empty () || Target[0] != '/') return false;
	const size_t q = Target.find ('?');
	pOut->Path = UrlDecode (Target.substr (0, q), false);
	pOut->Query = q == std::string::npos ? "" : Target.substr (q + 1);
	return !pOut->Method.empty ();
}

/// A parameter of a query string, decoded; "" if absent.
inline std::string QueryParam (const std::string &Query, const std::string &Name)
{
	size_t i = 0;
	while (i <= Query.size ())
	{
		size_t e = Query.find ('&', i);
		if (e == std::string::npos) e = Query.size ();
		const std::string Pair = Query.substr (i, e - i);
		const size_t eq = Pair.find ('=');
		if (UrlDecode (Pair.substr (0, eq), true) == Name)
		{
			return eq == std::string::npos ? "" : UrlDecode (Pair.substr (eq + 1), true);
		}
		i = e + 1;
	}
	return "";
}

/// A path on the card, made safe: it starts with '/', uses '/', has no empty,
/// "." or ".." parts and no control characters.  "" when it cannot be made so -
/// the caller refuses the request.  "/" is the card's root.
inline std::string SafePath (const std::string &In)
{
	if (In.empty () || (In[0] != '/' && In[0] != '\\')) return "";
	std::string Out;
	std::string Part;
	auto Flush = [&] () -> bool
	{
		if (Part.empty () || Part == ".") { Part.clear (); return true; }
		if (Part == "..") return false;
		Out += "/" + Part;
		Part.clear ();
		return true;
	};
	for (char c : In)
	{
		if (static_cast<unsigned char> (c) < 0x20 || c == 0x7F) return "";
		if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return "";
		if (c == '/' || c == '\\')
		{
			if (!Flush ()) return "";
		}
		else
		{
			Part += c;
		}
	}
	if (!Flush ()) return "";
	return Out.empty () ? "/" : Out;
}

/// The folder a path is in: "/dos/GAME/X.EXE" -> "/dos/GAME"; "/X" -> "/".
inline std::string Parent (const std::string &Path)
{
	const size_t n = Path.find_last_of ('/');
	return n == 0 || n == std::string::npos ? "/" : Path.substr (0, n);
}

inline std::string Base64Decode (const std::string &In)
{
	auto Value = [] (char c) -> int
	{
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::string Out;
	unsigned nBits = 0, nAcc = 0;
	for (char c : In)
	{
		const int v = Value (c);
		if (v < 0) continue;			// '=' padding, spaces
		nAcc = (nAcc << 6) | static_cast<unsigned> (v);
		nBits += 6;
		if (nBits >= 8)
		{
			nBits -= 8;
			Out += static_cast<char> ((nAcc >> nBits) & 0xFF);
		}
	}
	return Out;
}

/// "Authorization: Basic dXNlcjpwYXNz" -> does the password part match?  Any
/// user name is accepted: there is one password.
inline bool BasicAuthOk (const std::string &HeaderValue, const std::string &Password)
{
	const std::string Prefix = "Basic ";
	if (HeaderValue.compare (0, Prefix.size (), Prefix) != 0) return false;
	const std::string Plain = Base64Decode (HeaderValue.substr (Prefix.size ()));
	const size_t n = Plain.find (':');
	return n != std::string::npos && Plain.substr (n + 1) == Password;
}

/// "1.2 MB" style sizes for the listing.
inline std::string HumanSize (std::uint64_t n)
{
	char Buf[32];
	if (n < 1024) std::snprintf (Buf, sizeof Buf, "%llu B", static_cast<unsigned long long> (n));
	else if (n < 1024 * 1024) std::snprintf (Buf, sizeof Buf, "%.1f KB", n / 1024.0);
	else std::snprintf (Buf, sizeof Buf, "%.1f MB", n / (1024.0 * 1024.0));
	return Buf;
}

} // namespace bareweb

#endif
