/*
 * Copyright (c) 2000, 2001, Red Hat, Inc.
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by DJ Delorie <dj@cygnus.com>
 *
 */

/* The purpose of this file is to coordinate the various access
   methods known to setup.  To add a new method, create a pair of
   nio-*.[ch] files and add the logic to NetIO::open here */

#include "netio.h"

#include "LogFile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resource.h"
#include "state.h"
#include "msg.h"
#include "nio-file.h"
#include "nio-ie5.h"
#include "nio-http.h"
#include "nio-ftp.h"
#include "dialog.h"

int NetIO::net_method;
char *NetIO::net_proxy_host;
int NetIO::net_proxy_port;

char *NetIO::net_user;
char *NetIO::net_passwd;
char *NetIO::net_proxy_user;
char *NetIO::net_proxy_passwd;
char *NetIO::net_ftp_user;
char *NetIO::net_ftp_passwd;

NetIO::NetIO (char const *Purl)
{
  set_url (Purl);
}

NetIO::~NetIO ()
{
  if (url)
    delete[] url;
  if (proto)
    delete[] proto;
  if (host)
    delete[] host;
  if (path)
    delete[] path;
}

void
NetIO::set_url (char const *Purl)
{
  char *bp, *ep, c;

  file_size = 0;
  url = new char[strlen (Purl) + 1];
  strcpy (url, Purl);
  proto = 0;
  host = 0;
  port = 0;
  path = 0;

  bp = url;
  ep = strstr (bp, "://");
  if (!ep)
    {
      path = strdup (url);
      return;
    }

  *ep = 0;
  proto = new char [strlen (bp)+1];
  strcpy (proto, bp);
  *ep = ':';
  bp = ep + 3;

  ep = bp + strcspn (bp, ":/");
  c = *ep;
  *ep = 0;
  host = new char [strlen (bp) + 1];
  strcpy (host, bp);
  *ep = c;

  if (*ep == ':')
    {
      port = atoi (ep + 1);
      ep = strchr (ep, '/');
    }

  if (*ep)
    {
      path = new char [strlen (ep)+1];
      strcpy (path, ep);
    }
}

int
NetIO::ok ()
{
  return 0;
}

int
NetIO::read (char *buf, int nbytes)
{
  return 0;
}

NetIO *
NetIO::open (char const *url, bool cachable)
{
  NetIO *rv = 0;
  enum
  { http, https, ftp, ftps, file }
  proto;
  if (strncmp (url, "http://", 7) == 0)
    proto = http;
  else if (strncmp (url, "https://", 8) == 0)
    proto = https;
  else if (strncmp (url, "ftp://", 6) == 0)
    proto = ftp;
  else if (strncmp (url, "ftps://", 7) == 0)
    proto = ftps;
  else
    proto = file;

  if (proto == file)
    rv = new NetIO_File (url);
  else if (net_method == IDC_NET_IE5)
    rv = new NetIO_IE5 (url, false, cachable);
  else if (net_method == IDC_NET_PROXY)
    rv = new NetIO_HTTP (url);
  else if (net_method == IDC_NET_DIRECT)
    rv = new NetIO_IE5 (url, true, cachable);
  else if (net_method == IDC_NET_DIRECT_LEGACY)
    {
      switch (proto)
	{
	case http:
	  rv = new NetIO_HTTP (url);
	  break;
	case ftp:
	  rv = new NetIO_FTP (url);
	  break;
	case file:
	  rv = new NetIO_File (url);
	  break;
	default:
	  mbox (NULL, "Protocol not handled by legacy URL handler", "Cygwin Setup", MB_OK);
	}
    }

  if (rv && !rv->ok ())
    {
      delete rv;
      return 0;
    }

  return rv;
}


static char **user, **passwd;
static int loading = 0;

static void
check_if_enable_ok (HWND h)
{
  int e = 0;
  if (*user)
    e = 1;
  EnableWindow (GetDlgItem (h, IDOK), e);
}

static void
load_dialog (HWND h)
{
  loading = 1;
  eset (h, IDC_NET_USER, *user);
  eset (h, IDC_NET_PASSWD, *passwd);
  check_if_enable_ok (h);
  loading = 0;
}

static void
save_dialog (HWND h)
{
  *user = eget (h, IDC_NET_USER, *user);
  *passwd = eget (h, IDC_NET_PASSWD, *passwd);
  if (! *passwd) {
    *passwd = new char[1];
    passwd[0] = '\0';
  }
}

static BOOL
auth_cmd (HWND h, int id, HWND hwndctl, UINT code)
{
  switch (id)
    {

    case IDC_NET_USER:
    case IDC_NET_PASSWD:
      if (code == EN_CHANGE && !loading)
	{
	  save_dialog (h);
	  check_if_enable_ok (h);
	}
      break;

    case IDOK:
      save_dialog (h);
      EndDialog (h, 0);
      break;

    case IDCANCEL:
      EndDialog (h, 1);
      Logger ().exit (1);
      break;
    }
  return 0;
}

static INT_PTR CALLBACK
auth_proc (HWND h, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
    {
    case WM_INITDIALOG:
      load_dialog (h);
      return FALSE;
    case WM_COMMAND:
      auth_cmd (h, LOWORD(wParam), (HWND)lParam, HIWORD(wParam));
      return 0;
    }
  return FALSE;
}

static int
auth_common (HINSTANCE h, int id, HWND owner)
{
  return DialogBox (h, MAKEINTRESOURCE (id), owner, auth_proc);
}

int
NetIO::get_auth (HWND owner)
{
  user = &net_user;
  passwd = &net_passwd;
  return auth_common (hinstance, IDD_NET_AUTH, owner);
}

int
NetIO::get_proxy_auth (HWND owner)
{
  user = &net_proxy_user;
  passwd = &net_proxy_passwd;
  return auth_common (hinstance, IDD_PROXY_AUTH, owner);
}

int
NetIO::get_ftp_auth (HWND owner)
{
  if (net_ftp_user)
    {
      delete[] net_ftp_user;
      net_ftp_user = NULL;
    }
  if (net_ftp_passwd)
    {
      delete[] net_ftp_passwd;
      net_ftp_passwd = NULL;
    }
  if (!ftp_auth)
    return IDCANCEL;
  user = &net_ftp_user;
  passwd = &net_ftp_passwd;
  return auth_common (hinstance, IDD_FTP_AUTH, owner);
}

const char *
NetIO::net_method_name ()
{
  switch (net_method)
    {
    case IDC_NET_IE5:
      return "IE5";
    case IDC_NET_DIRECT:
      return "Direct";
    case IDC_NET_PROXY:
      return "Proxy";
    case IDC_NET_DIRECT_LEGACY:
      return "Direct (legacy)";
    default:
      return "Unknown";
    }
}
