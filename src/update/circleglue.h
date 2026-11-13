//
// circleglue.h - Transporte TCP de Circle para el cliente HTTPS
//
// Imported from ZXMetalCircle (github.com/shaeon/ZXMetalCircle,
// src/download/circleglue.h, commit 61494ca), by the same author, into BM-DOS on
// 13-nov-2026 (GPL-3.0-or-later, as both projects).  Changes for BM-DOS:
// none.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef _zx_circleglue_h
#define _zx_circleglue_h

#include "httpsclient.h"
#include <circle/net/socket.h>

class CCircleTransport : public ITransport
{
public:
	CCircleTransport (void);
	~CCircleTransport (void);

	bool Connect (const char *pHost, unsigned nPort);
	int Send (const unsigned char *pData, size_t nLength);
	int Receive (unsigned char *pBuffer, size_t nSize);
	void Close (void);

private:
	CSocket *m_pSocket;
};

#endif
