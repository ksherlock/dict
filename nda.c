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

#include "connection.h"
#include "nda.h"

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
  st_init,
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

void InsertString(word length, char *cp) {
  Handle handle;
  // TERecord **temp;
  longword oldStart, oldEnd;

  handle = (Handle)GetCtlHandleFromID(MyWindow, rCtrlTE);
  // temp = (TERecord **)handle;

  //(**temp).textFlags &= (~fReadOnly);

  TEGetSelection((pointer)&oldStart, (pointer)&oldEnd, handle);

  TESetSelection((Pointer)-1, (Pointer)-1, handle);
  TEInsert(teDataIsTextBlock, (Ref)cp, length, NULL, NULL, /* no style info */
           handle);

  //(**temp).textFlags |= fReadOnly;

  TESetSelection((Pointer)oldStart, (Pointer)oldEnd, handle);
}

static char buffer[512];

void TCPLoop(void) {

  static srBuff sr;
  static rlrBuff rlr;

  Word terr;
  unsigned x;
  int status;

  switch (st) {

  case st_connect:
    x = ConnectionPoll(&connection);
    switch (x) {
    case kConnectionStateConnected:
      ++st;
      break;
    case kConnectionStateDisconnected:
    case kConnectionStateError:
      st = st_none;
      break;
    }
    return;
  case st_disconnect:
    x = ConnectionPoll(&connection);
    switch (x) {
    case kConnectionStateDisconnected:
      st = st_none;
      break;
    case kConnectionStateError:
      st = st_none;
      break;
    }
    return;

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
    if (!rlr.rlrIsDataFlag)
      return;

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
    ++x;
    rlr.rlrBuffCount = x;

    if (st != st_define3) {
      unsigned i;
      status = 0;
      for (i = 0;; ++i) {
        unsigned c = buffer[i];
        if (isdigit(x)) {
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
    }
    /* else error */
    break;
  case st_define1:
    /* expect 550, 552, or 150 status */
    if (status == 150) {
      ++st;
      break;
    }

    if (status == 552) {
      InsertString(10, "No match\r");
      st = st_none;
    } else {
      InsertString(rlr.rlrBuffCount, buffer);
      st = st_none;
    }
    break;
  case st_define2:
    /* expect 151 */
    if (status == 151)
      ++st;
    else if (status == 250)
      st = st_none;
    else {
    }
    break;
  case st_define3:
    /* expect definition text. '.' terminates. */
    if (buffer[0] == '.') {
      InsertString(rlr.rlrBuffCount - 1, buffer + 1);
      if (buffer[1] == '\r') {
        --st;
      }
    } else {
      InsertString(rlr.rlrBuffCount, buffer);
    }
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
    MoveTo(/*8, 22*/ rect->h1 + 2, rect->v1 + 2);
    DrawString(str);
  }
}

void DrawWindow(void) { DrawControls(GetPort()); }

#pragma databank 0

void NDAInit(Word code) {
  if (code) {
    MyWindow = NULL;
    FlagTCP = false;
    ToolsLoaded = false;

    MyID = MMStartUp();
    ipid = 0;
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

  NDAResourceShutDown(&resInfo);
}

GrafPortPtr NDAOpen(void) {

  MyWindow = NULL;

  if (!ToolsLoaded) {
    if (NDAStartUpTools(MyID, &ss)) {
      NDAShutDownTools(&ss);
      return NULL;
    }
    ToolsLoaded = true;
  }

  if (TCPIPLongVersion() < 0x03006010) {
    AlertWindow(awCString, NULL,
                (Ref) "24~Marinetti 3.0b10 or newer required.~^Ok");
    return NULL;
  }

  // Check if Marinetti Active.
  FlagTCP = TCPIPGetConnectStatus();

  if (NDAResourceStartUp(MyID, readEnable, &resInfo)) {

    MyWindow = NewWindow2(NULL, 0, DrawWindow, NULL, refIsResource, rWindow,
                          rWindParam1);

    SetInfoDraw(DrawInfo, MyWindow);
    AcceptRequests(ReqName, MyID, &HandleRequest);

    SetSysWindow(MyWindow);
    ShowWindow(MyWindow);
    SelectWindow(MyWindow);

    ConnectionInit(&connection, MyID, MarinettiCallback);

    NDAResourceRestore(&resInfo);
    return MyWindow;
  }
  return NULL;
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
      case rCtrlDefine: {

        break;
      }
      }
      // todo - Command-A selects all.
    }
  } else if (code == copyAction) {
    TECopy(NULL);
    return 1; // yes we handled it.
  }

  return 0;
}