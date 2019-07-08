#pragma nda NDAOpen NDAClose NDAAction NDAInit 30 0xffff "--Dictionary\\H**"
#pragma lint - 1
#pragma optimize - 1

#include <Control.h>
#include <Desk.h>
#include <Event.h>
#include <GSOS.h>
#include <Loader.h>
#include <Locator.h>
#include <Memory.h>
#include <Resources.h>
#include <TCPIP.h>
#include <TextEdit.h>
#include <Types.h>
#include <Window.h>
#include <intmath.h>
#include <misctool.h>
#include <quickdraw.h>

#include <ctype.h>
#include <stdio.h>

#include "connection.h"
#include "nda.h"


#define TBBlack "\x01" "C" "\x00\x00"
#define TBBlue "\x01" "C" "\x11\x11"
#define TBRed "\x01" "C" "\x44\x44"



unsigned NDAStartUpTools(Word memID, StartStopRecord *ssRef);
void NDAShutDownTools(StartStopRecord *ssRef);

typedef struct NDAResourceCookie {
  Word oldPrefs;
  Word oldRApp;
  Word resFileID;
} NDAResourceCookie;

void NDAResourceRestore(NDAResourceCookie *cookie);
void NDAResourceShutDown(NDAResourceCookie *cookie);
Word NDAResourceStartUp(Word memID, Word access, NDAResourceCookie *cookie);

Word MyID;
Word ipid;
Word FlagTCP;
Word ToolsLoaded;
GrafPortPtr MyWindow;
Handle TECtrlHandle;


Handle TextHandle;
LongWord TextHandleSize;
LongWord TextHandleUsed;

static StartStopRecord ss = {0,
                             0,
                             0,
                             0,
                             4,
                             {
                                 0x12, 0x0000, /* QD Aux */
                                 0x1b, 0x0000, /* Font Manager */
                                 0x22, 0x0000, /* Text Edit */
                                 0x36, 0x0300, /* TCP */
                             }

};

static NDAResourceCookie resInfo;
static Connection connection;

enum {
  st_none,
  st_idle,
  st_connect,
  st_login,
  st_client,
  st_define1,
  st_define2,
  st_define3,
  st_quit,
  st_disconnect,
};

static unsigned st = 0;
static LongWord qtick = 0;
const char *ReqName = "\pTCP/IP~kelvin~dict~";

int define(Word ipid, const char *dict);
pascal void MarinettiCallback(char *str);

void EnableControls(void);
void DisableControls(void);


void AppendText(word length, char *cp) {

  Handle h = TextHandle;
  LongWord size;

  size = TextHandleUsed + length;
  if (size > TextHandleSize) {
    size += 4095;
    size &= 4096;

    if (h) {
      HUnlock(h);
      SetHandleSize(size, h);
      if (_toolErr) return;
      HLock(h);
      TextHandleSize = size;
    } else {
      TextHandle = h = NewHandle(size, MyID, attrLocked, 0);
      if (_toolErr) return;
      TextHandleSize = size;
    }
    HLock(h);
  }
  BlockMove(cp, *h + TextHandleUsed, length);
  TextHandleUsed += length;
}


void AppendText2(word length, char *cp) {

  unsigned i;
  unsigned char c;
  unsigned start = 0;
  for (i = 0; i < length; ++i) {
    c = cp[i];

    if (c == '{' || c == '}' || c == 0x01) {

      /* flush any pending data */
      if (start < i)
        AppendText(i - start, cp + start);

      if (c == '{') AppendText(4, TBBlue);
      if (c == '}') AppendText(4, TBBlack);
      start = i + 1;
    }
  }
  if (start < length)
    AppendText(length - start, cp + start);

}

void SetText(void) {
  longword oldStart, oldEnd;

  TESetSelection((Pointer)-1, (Pointer)-1, TECtrlHandle);
  TESetText(teDataIsTextBox2|teTextIsPtr, (Ref)*TextHandle, TextHandleUsed, NULL, NULL, TECtrlHandle);


  TextHandleUsed = 0;
}



static char buffer[512];

void TCPLoop(void) {

  static srBuff sr;
  static rlrBuff rlr;

  Word terr;
  unsigned x;
  int status;

  Word ipid = connection.ipid;

  switch (st) {

  case st_idle:
  	if (GetTick() >= qtick) {
  		TCPIPWriteTCP(ipid, "QUIT\r\n", 6, 1, 0);
  		qtick = GetTick() + 60 * 30;
  		st = st_quit;
  		return;
	}
	break;

  case st_connect:
    x = ConnectionPoll(&connection);
    switch (connection.state) {
    case kConnectionStateConnected:
      MarinettiCallback("\pConnected.");
      ++st;
      break;
    case kConnectionStateDisconnected:
      MarinettiCallback("\pDisconnected.");
      EnableControls();
      st = st_none;
      break;
    case kConnectionStateError:
      MarinettiCallback("\pConnection Error.");
      EnableControls();
      st = st_none;
      break;
    }
    return;
  case st_disconnect:
    x = ConnectionPoll(&connection);
    switch (connection.state) {
    case kConnectionStateDisconnected:
    case kConnectionStateError:
      EnableControls();
      st = st_none;
      break;
    }
    return;

redo:
  case st_login:
  case st_client:
  case st_define1:
  case st_define2:
  case st_define3:
  case st_quit:
    TCPIPPoll();

    sr.srRcvQueued = 0;
    terr = TCPIPStatusTCP(ipid, &sr);
    if (sr.srRcvQueued == 0) {
      if (GetTick() >= qtick) {
        /* timeout */
      }
      return;
    }
    /* all data should be \r\n delimited. we want to keep the \r */
    terr = TCPIPReadLineTCP(ipid, "\p\n", 0x0000, (Ref)&buffer,
                            sizeof(buffer) - 2, &rlr);

    if (terr) return;
    if (!rlr.rlrIsDataFlag)
      return;
  	qtick += 60 * 15; /* bump timeout */
    /* ensure a trailing \r and 0 */
    x = rlr.rlrBuffCount;
    if (x) {
      --x;
      if (buffer[x] != '\r') {
        buffer[x] = '\r';
        ++x;
      }
      ++x;
    }
    buffer[x] = 0;
    rlr.rlrBuffCount = x;

    if (st != st_define3) {
      unsigned i;
      status = 0;
      for (i = 0;; ++i) {
        unsigned c = buffer[i];
        if (isdigit(c)) {
          status *= 10;
          status += c - '0';
          continue;
        }
        if (isspace(c) || c == 0)
          break;
        status = -1;
        break;
      }
      if (i == 0)
        status = -1;
    }
    break;
  }

  switch (st) {
  case st_login:
    /* expect a 220 status */
    if (status == 220) {
      ++st;
      terr = TCPIPWriteTCP(ipid, "CLIENT dict-nda-iigs\r\n", 22, 1, 0);
    }
    /* else error... */
    break;
  case st_client:
    /* expect 250 status */
    if (status == 250) {
      ++st;
      /* send define string... */
      define(ipid, NULL);
    }
    /* else error */
    break;
  case st_define1:
    /* expect 550, 552, or 150 status */
    if (status == 150) {
      ++st;
      AppendText(4, TBBlack);
      break;
    }

    if (status == 552) {
      MarinettiCallback("\pNo match");
      st = st_idle;
    } else {
      AppendText(4, TBRed);
      AppendText(rlr.rlrBuffCount, buffer);
      SetText();
      st = st_idle;
    }
    EnableControls();
  	qtick = GetTick() + 60 * 60 * 2; /* 2-minute timeout */
    break;
  case st_define2:
    /* expect 151 */
    if (status == 151)
      ++st;
    else if (status == 250) {
      st = st_idle;
  	  qtick = GetTick() + 60 * 60 * 2; /* 2-minute timeout */
      SetText();
      EnableControls();
    } else {
    }
    break;
  case st_define3:
    /* expect definition text. '.' terminates. */
    if (buffer[0] == '.') {
      AppendText(rlr.rlrBuffCount - 1, buffer + 1);
      if (buffer[1] == '\r') {
        --st;
      }
    } else {
      AppendText2(rlr.rlrBuffCount, buffer);
    }
    goto redo;
    break;

  case st_quit:
    /* expect 221 but doesn't really matter... */
    ConnectionClose(&connection);
    st = st_disconnect;
    break;
  }
}

// activate/inactivate controls based on Marinetti status
void UpdateStatus(Boolean redraw) {
  if (FlagTCP) // TCP started
  {

    SetInfoRefCon((LongWord) "\pNetwork Connected", MyWindow);
  } else {

    SetInfoRefCon((LongWord) "\pNetwork Disconnected", MyWindow);
  }
  if (redraw)
    DrawInfoBar(MyWindow);
}

#pragma databank 1

/*
 *  Watch for TCP status updates.
 */
pascal word HandleRequest(word request, longword dataIn, longword dataOut) {
  Word oldRApp;

  oldRApp = GetCurResourceApp();
  SetCurResourceApp(MyID);

  if (request == TCPIPSaysNetworkUp) {
    FlagTCP = true;
    UpdateStatus(true);
  }

  if (request == TCPIPSaysNetworkDown) {
    FlagTCP = false;
    ipid = 0;
    UpdateStatus(true);
  }
  SetCurResourceApp(oldRApp);
}

pascal void MarinettiCallback(char *str) {
  if (MyWindow) {
    SetInfoRefCon((LongWord)str, MyWindow);
    DrawInfoBar(MyWindow);
  }
}

pascal void DrawInfo(Rect *rect, const char *str, GrafPortPtr w) {
  SetForeColor(0x00);
  SetBackColor(0x0f);
  EraseRect(rect);
  if (str) {
    MoveTo(/*8, 22*/ rect->h1 + 10, rect->v1 + 9);
    DrawString(str);
  }
}

void DrawWindow(void) { DrawControls(GetPort()); }

#pragma databank 0

void MakeCtlTargetByID(GrafPortPtr window, Long resID) {
	CtlRecHndl h = GetCtlHandleFromID(window, resID);
	if (h) MakeThisCtlTarget(h); 
}

void NDAInit(Word code) {
  if (code) {
    MyWindow = NULL;
    FlagTCP = false;
    ToolsLoaded = false;

    MyID = MMStartUp();
    ipid = 0;
    st = st_none;
  } else {
    if (ToolsLoaded)
      NDAShutDownTools(&ss);
    ToolsLoaded = false;
  }
}

void NDAClose(void) {
  // if running, shut down.

  AcceptRequests(ReqName, MyID, NULL);
  ConnectionAbort(&connection);

  CloseWindow(MyWindow);
  MyWindow = NULL;
  TECtrlHandle = NULL;

  NDAResourceShutDown(&resInfo);

  if (TextHandle) {
    DisposeHandle(TextHandle);
    TextHandle = NULL;
    TextHandleUsed = 0;
    TextHandleSize = 0;
  }
}

GrafPortPtr NDAOpen(void) {

  MyWindow = NULL;
  TextHandle = NULL;
  TextHandleSize = 0;
  TextHandleSize = 0;

  if (!ToolsLoaded) {
    if (NDAStartUpTools(MyID, &ss)) {
      NDAShutDownTools(&ss);
      return NULL;
    }
    ToolsLoaded = true;
  }

  if (TCPIPLongVersion() < 0x03006011) {
    AlertWindow(awCString, NULL,
                (Ref) "24~Marinetti 3.0b11 or newer required.~^Ok");
    return NULL;
  }

  // Check if Marinetti Active.
  FlagTCP = TCPIPGetConnectStatus();

  if (NDAResourceStartUp(MyID, readEnable, &resInfo)) {

    MyWindow = NewWindow2(NULL, 0, DrawWindow, NULL, refIsResource, rWindow,
                          rWindParam1);

    SetInfoDraw(DrawInfo, MyWindow);
    UpdateStatus(0);

    AcceptRequests(ReqName, MyID, &HandleRequest);


    MakeCtlTargetByID(MyWindow, rCtrlLE);

    SetSysWindow(MyWindow);
    ShowWindow(MyWindow);
    SelectWindow(MyWindow);

    TECtrlHandle = (Handle)GetCtlHandleFromID(MyWindow, rCtrlTE);

    ConnectionInit(&connection, MyID, MarinettiCallback);

    NDAResourceRestore(&resInfo);
    return MyWindow;
  }
  return NULL;
}


static char word_to_define[256];
int define(Word ipid, const char *dict) {
  word terr;
  int ok;
  unsigned x;
  static char buffer[512];

  if (!dict || !*dict)
    dict = "!";


  x = sprintf(buffer, "DEFINE %s \"%b\"\r\n", dict, word_to_define);

  terr = TCPIPWriteTCP(ipid, buffer, x, 1, 0);
  return 0;
}


void DoDefine(void) {

	unsigned i;
	Handle handle;


	handle = (Handle)GetCtlHandleFromID(MyWindow, rCtrlTE);

	TESetText(teDataIsTextBlock, (Ref)"", 0, NULL, NULL, handle);

	GetLETextByID(MyWindow, rCtrlLE, (StringPtr)word_to_define);

	i = word_to_define[0];
	while (i && isspace(word_to_define[i])) --i;
	if (!i) return;
	word_to_define[0] = i;
	word_to_define[i+1] = 0;


	/* considerations:
	   1. is the network connected?
	   2. is a tcp connection already established?
	 */

	if (!FlagTCP) {

		MarinettiCallback("\pConnecting to network...");
		TCPIPConnect(MarinettiCallback);
		if (!FlagTCP) return;
	}

	qtick = GetTick() + 30 * 60;

	DisableControls();

	switch(st) {
		case st_idle:
			define(connection.ipid, NULL);
			st = st_define1;
			break;
		default:
			ConnectionAbort(&connection);
		case st_none:
			ConnectionOpenC(&connection, "dict.org", 2628);
			st = st_connect;
			break;	


	}
}

void EnableControls(void) {
	HiliteCtlByID(noHilite, MyWindow, rCtrlLE);
	HiliteCtlByID(noHilite, MyWindow, rCtrlDefine);
}

void DisableControls(void) {
	HiliteCtlByID(inactiveHilite, MyWindow, rCtrlLE);
	HiliteCtlByID(inactiveHilite, MyWindow, rCtrlDefine);
}

word NDAAction(void *param, int code) {
  word eventCode;
  static EventRecord event = {0};

  if (code == runAction) {
    if (st)
      TCPLoop();
    return 1;
  }

  else if (code == eventAction) {
    BlockMove((Pointer)param, (Pointer)&event, 16);
    event.wmTaskMask = 0x001FFFFF;
    eventCode = TaskMasterDA(0, &event);
    switch (eventCode) {
    case updateEvt:
      BeginUpdate(MyWindow);
      DrawWindow();
      EndUpdate(MyWindow);
      break;

    case wInControl:
      switch (event.wmTaskData4) {
      /* start marinetti */
      case rCtrlDefine:
      	DoDefine();
        break;
      }
      // todo - Command-A selects all.
    }
  } else if (code == copyAction) {
    TECopy(NULL);
    return 1; // yes we handled it.
  }

  return 0;
}