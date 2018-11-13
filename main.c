#pragma optimize 79
#pragma lint -1

#include <Locator.h>
#include <tcpip.h>
#include <MiscTool.h>
#include <Memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "connection.h"

#define IncBusy() asm { jsl 0xE10064 }
#define DecBusy() asm { jsl 0xE10068 }
#define Resched() asm { cop 0x7f }

#define BusyFlag ((byte *)0xE100FFl)


// startup/shutdown flags.
enum {
  kLoaded = 1,
  kStarted = 2,
  kConnected = 4,

  kLoadError = -1,
  kVersionError = -2
};

int StartUpTCP(displayPtr fx)
{
  word status;
  word flags = 0;
  
  // TCPIP is an init, not a tool, so it should always
  // be loaded.
  
  status = TCPIPStatus();
  if (_toolErr)
  {
    LoadOneTool(54, 0x0300);
    if (_toolErr == toolVersionErr) return kVersionError;
    if (_toolErr) return kLoadError;

    status = 0;
    flags |= kLoaded;
  }


  // require 3.0b3
  if (TCPIPLongVersion() < 0x03006003)
  {
    if (flags & kLoaded)
      UnloadOneTool(54);

    return kVersionError;     
  }

  if (!status)
  {
    TCPIPStartUp();
    if (_toolErr) return kLoadError;
    flags |= kStarted;
  }

  status = TCPIPGetConnectStatus();
  if (!status)
  {
    TCPIPConnect(fx);
    flags |= kConnected;
  }

  return flags;
}

void ShutDownTCP(int flags, Boolean force, displayPtr fx)
{
  if (flags <= 0) return;

  if (flags & kConnected)
  {
    TCPIPDisconnect(force, fx);
    if (_toolErr) return;
  }
  if (flags & kStarted)
  {
    TCPIPShutDown();
    if (_toolErr) return;
  }
  if (flags & kLoaded)
  {
    UnloadOneTool(54);
  }
}


// #pragma databank [push | pop] would be nice...
#pragma databank 1
pascal void DisplayCallback(const char *message)
{
  unsigned length;

  // message is a p-string.
  length = message ? message[0] : 0;
  if (!length) return;

  fprintf(stderr, "%.*s\n", length, message + 1);
}
#pragma databank 0


int ConnectLoop(char *host, Word port, Connection *connection)
{
  LongWord qtick;

  ConnectionInit(connection, MMStartUp(), DisplayCallback);
  ConnectionOpenC(connection, host,  port);

  // 30 second timeout.
  qtick = GetTick() + 30 * 60;
  while (!ConnectionPoll(connection))
  {
    if (GetTick() >= qtick)
    {
      fprintf(stderr, "Connection timed out.\n");

      IncBusy();
      TCPIPAbortTCP(connection->ipid);
      TCPIPLogout(connection->ipid);      
      DecBusy();
      
      return 0;
    }
  }

  if (connection->state != kConnectionStateConnected)
  {
    fprintf(stderr, "Unable to open host: %s:%u\n", 
      host, 
      port);
    return 0;
  }

  return 1;
}


int CloseLoop(Connection *connection)
{
    ConnectionClose(connection);

    while (!ConnectionPoll(connection)) ; // wait for it to close.
      
    return 1;
}



static char buffer[512];

int ReadLineSync(word ipid) {

  LongWord qtick;
  static srBuff sr;
  static rlrBuff rlr;
  unsigned x;
  word terr;


  //asm { brk 0xea }
  buffer[0] = 0;

  qtick = GetTick() + 30 * 60;
	for(;;) {
		Word terr;
		TCPIPPoll();
    terr = TCPIPStatusTCP(ipid, &sr);
    if (sr.srRcvQueued) break;		

    if (terr) return -1;
    if (GetTick() >= qtick) {
      fprintf(stderr, "Read timed out.\n");
      return -1;
    }
	}
	for(;;) {
	  terr = TCPIPReadLineTCP(ipid, "\p\r\n", 0x0000, (Ref)&buffer, sizeof(buffer) - 1, &rlr);
	  /* if (terr) return -1; */ /* nb - marinetti return bug */
	  if (!rlr.rlrIsDataFlag) {

	    if (GetTick() >= qtick) {
	      fprintf(stderr, "Read timed out.\n");
	      return -1;
	    }
	  	TCPIPPoll();
	  	continue;
	  }
	  break;
	}
	x = rlr.rlrBuffCount;
	buffer[x] = 0;
	return x;
}

int status(void) {
	unsigned x;
	if (sscanf(buffer, "%u", &x) == 1) {
		fprintf(stderr, "status: %d\n", x);
		return x;
	}
	return -1;
}

int client(word ipid) {
	word terr;
	int ok;

	/* also checks for 220 header. */

	ok = ReadLineSync(ipid);
	if (ok < 0) return ok;
	ok = status();
	if (ok != 220) return -1;

	terr = TCPIPWriteTCP(ipid, "CLIENT dict-iigs\r\n", 18, 1, 0);
	if (terr != 0) fprintf(stderr, "terr: %04x\n", terr);

	ok = ReadLineSync(ipid);
	if (ok < 0) return ok;

/*
3.6.2.  Responses

             250 ok (optional timing information here)
*/
	ok = status();
	if (ok != 250) return -1;
	return 0;
}

int quit(word ipid) {
	word terr;
	int ok;

	terr = TCPIPWriteTCP(ipid, "QUIT\r\n", 6, 1, 0);
	if (terr != 0) fprintf(stderr, "terr: %04x\n", terr);
	ok = ReadLineSync(ipid);
	if (ok < 0) return ok;

/*
3.9.2.  Responses

             221 Closing Connection
*/


	return 0;
}

int one_def(Word ipid) {
	int ok;

	for(;;) {
		ok = ReadLineSync(ipid);
		if (ok < 0) return ok;
		ok = status();
		if (ok == 250) return 0;
		if (ok != 151) return -1;
		for(;;) {
			ok = ReadLineSync(ipid);
			if (ok < 0) return ok;
			if (buffer[0] == '.') {
				fputc('\n', stdout);
				break;
			}
			fputs(buffer, stdout);
			fputc('\n', stdout);
		}


	}
	fputc('\n', stdout);
	return 0;
}

int define(Word ipid, const char *term, const char *dict) {
	word terr;
	int ok;
	unsigned x;
	static char buffer[512];

	if (!dict || !*dict) dict = "!";
	if (!term || !*term) return -1;

	x = sprintf(buffer, "DEFINE %s \"%s\"\r\n", dict, term);

	terr = TCPIPWriteTCP(ipid, buffer, x, 1, 0);
	if (terr != 0) fprintf(stderr, "terr: %04x\n", terr);

/*
3.2.2.  Responses

       550 Invalid database, use "SHOW DB" for list of databases
       552 No match
       150 n definitions retrieved - definitions follow
       151 word database name - text follows
       250 ok (optional timing information here)
*/

	ok = ReadLineSync(ipid);
	if (ok < 0) return ok;

	ok = status();
	switch(status()) {
		default: return -1;

		case 550:
			fprintf(stdout, "Invalid database.\n");
			return -1;

		case 552:		
			fprintf(stdout, "No match.\n");
			return 0;

		case 150:
			return one_def(ipid);
	}
}


int main(int argc, char **argv) {


  Connection connection;
  int mf;
  int ok;
  word terr;

  mf = StartUpTCP(DisplayCallback);

  if (argc < 1) exit(1);

  if (mf < 0) {
    fprintf(stderr, "Marinetti 3.0b3 or greater is required.\n");
    exit(1);
  }

  ok = ConnectLoop("dict.org",  2628, &connection);
  if (ok) {
  	unsigned i;
  	int ok;

  	ok = client(connection.ipid);
  	if (ok == 0) {
			for (i = 1; i < argc; ++i) {
				ok = define(connection.ipid, argv[i], NULL);
				if (ok < 0) break;
			}
		}

		quit(connection.ipid);
  	CloseLoop(&connection);
  }


  ShutDownTCP(mf, false, DisplayCallback);
  return 0;
}