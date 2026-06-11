/*
PrintServer Copyright (c) 2006, Henk Jonas
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. The name of the author may not be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (
INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (
INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include <windows.h>
#include <stdio.h>
#include <winspool.h>

/* Fix 1: Use a writable temp directory for the log file instead of C:\ root,
   which requires admin privileges on Windows 11 with UAC enabled. */
#define SLEEP_TIME 5000

/* Log file path is now built at runtime using GetTempPath() */
static char g_logFile[MAX_PATH] = {0};

SERVICE_STATUS ServiceStatus;
SERVICE_STATUS_HANDLE hStatus;

/* Fix 2: Track whether we are running as a service or standalone so we can
   choose the correct registry hive (HKCU vs HKLM). */
static int g_isService = 0;

typedef enum {
  INSTALL = 0,
  REMOVE,
  STANDALONE,
  BACKGROUND,
  PRIVATE_SERVICE,
  INVALID
} CommandType;

/* Same order as CommandType */
struct command {
  const char *commandName;
  int maxCommandArgs;
} commands[] = {{"INSTALL", 2},
                {"REMOVE", 1},
                {"STANDALONE", 2},
                {"BACKGROUND", 2},
                {"PRIVATE_SERVICE", 1}};

#define NUM_COMMANDS (sizeof commands / sizeof *commands)

const char* serviceKey = "System\\CurrentControlSet\\Services\\%s";
/* Fix 3: Standalone registry is now placed under HKCU (see CreatePrintServer /
   InnerLoop helpers). The key string itself is reused; only the HKEY root
   changes depending on g_isService. */
const char* standaloneKey = "SOFTWARE\\Alexander_Pruss\\RawPrintServer\\%s";
const char *regKey = serviceKey;
char printerName[256] = {0};
char strServiceName[64];
DWORD serverPort;
DWORD startPort;

int InnerLoop(DWORD port, int service);
void ServiceMain(int argc, char **argv);
void ControlHandler(DWORD request);
int InitService();

/* ---------------------------------------------------------------------------
   Returns the HKEY root appropriate for the current run mode.
   Services must write to HKLM; standalone runs use HKCU to avoid the UAC
   elevation requirement imposed by Windows 11 on HKLM writes.
   --------------------------------------------------------------------------- */
static HKEY RootKey(void) {
  return g_isService ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

/* Fix 4: Build the log file path once into g_logFile using GetTempPath so
   that the file is always in a directory the current user can write to. */
static void InitLogPath(void) {
  char tempDir[MAX_PATH] = {0};
  if (GetTempPathA(MAX_PATH, tempDir) == 0)
    _snprintf(tempDir, MAX_PATH, "C:\\Temp");
  _snprintf(g_logFile, MAX_PATH, "%sPrintServer.log", tempDir);
}

int WriteToLog(const char *str) {
  FILE *log;
  if (g_logFile[0] == '\0')
    return -1;
  log = fopen(g_logFile, "a+");
  if (log == NULL)
    return -1;
  fprintf(log, "%s\n", str);
  fclose(log);
  return 0;
}

VOID CreatePrintServer(char *strMyPath, char *strPrinter, DWORD port,
                       int service) {
  char strTemp[1024];
  char modulePath[MAX_PATH];
  GetModuleFileNameA(NULL, modulePath, MAX_PATH);
  sprintf(strTemp, "\"%s\" PRIVATE_SERVICE %d", modulePath, port);

  if (service) {
    SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
      printf("Error: ServiceManager %d\n", GetLastError());
      return;
    }

    SC_HANDLE schService = CreateService(
        schSCManager,   // SCManager database
        strServiceName, // name of service
        strServiceName, // service name to display (Attention: we better use the
                        // same name here, or you will never find it...)
        SERVICE_ALL_ACCESS,        // desired access
        SERVICE_WIN32_OWN_PROCESS, // service type
        SERVICE_AUTO_START, // SERVICE_DEMAND_START,      // start type
        SERVICE_ERROR_NORMAL, // error control type
        strTemp, // lpszBinaryPathName,        // service's binary
        NULL,  // no load ordering group
        NULL,  // no tag identifier
        NULL,  // no dependencies
        NULL,  // LocalSystem account
        NULL); // no password

    if (schService == NULL) {
      printf("Error: CreateService %d\n", GetLastError());
      return;
    } else
      printf("CreateService SUCCESS.\n");

    if (StartService(schService, 0, NULL))
      printf("Service started.\n");
    else
      printf("Error starting Service: %d, please do it by hand.\n",
             GetLastError());
    CloseServiceHandle(schService);
  }

  HKEY hdlKey = NULL;
  sprintf(strTemp, regKey, strServiceName);

  if (!service) {
    /* Fix 5: For standalone mode, write under HKCU so no admin rights are
       needed. Also pass NULL for the lpClass parameter (was "" which is
       invalid per the RegCreateKeyEx documentation). */
    RegCreateKeyEx(HKEY_CURRENT_USER, "SOFTWARE\\Alexander_Pruss", 0, NULL,
                   REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, NULL, NULL);
    RegCreateKeyEx(HKEY_CURRENT_USER, "SOFTWARE\\Alexander_Pruss\\RawPrintServer", 0, NULL,
                   REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, NULL, NULL);
  }

  /* Fix 5 (cont.): Use NULL for lpClass and the correct HKEY root. */
  RegCreateKeyEx(RootKey(), strTemp, 0, NULL, REG_OPTION_NON_VOLATILE,
                 KEY_ALL_ACCESS, NULL, &hdlKey, NULL);

  if (hdlKey == NULL) {
    printf("Error: RegCreateKeyEx failed: %d\n", GetLastError());
    return;
  }

  RegSetValueEx(hdlKey, "Description", 0, REG_SZ,
                (const BYTE *)"Routes all traffic from port 910x to a "
                               "local printer",
                53);
  RegSetValueEx(hdlKey, "Printer", 0, REG_SZ, (const BYTE *)strPrinter,
                (DWORD)(strlen(strPrinter) + 1));
  RegSetValueEx(hdlKey, "Port", 0, REG_DWORD, (const BYTE *)&port,
                sizeof(port));
  RegCloseKey(hdlKey);

  /* Also cache the printer name globally so InnerLoop can fall back to it
     if a subsequent registry read somehow fails. */
  strncpy(printerName, strPrinter, sizeof(printerName) - 1);
  printerName[sizeof(printerName) - 1] = '\0';

  printf("Registry configuration saved for printer \"%s\" on port %lu.\n",
         strPrinter, (unsigned long)port);
}

VOID DeletePrintServerService(DWORD port) {
  SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
  if (!schSCManager) {
    printf("Error: ServiceManager %d\n", GetLastError());
    return;
  }

  SC_HANDLE schService =
      OpenService(schSCManager,        // SCManager database
                  strServiceName,      // name of service
                  SERVICE_ALL_ACCESS); // only need DELETE access

  if (schService == NULL) {
    printf("Error: OpenService (%s) %d\n", strServiceName, GetLastError());
    return;
  }

  SERVICE_STATUS ss;
  if (ControlService(schService, SERVICE_CONTROL_STOP, &ss))
    printf("Server stopped.\n");
  else {
    int err = GetLastError();
    if (err != ERROR_SERVICE_NOT_ACTIVE) {
      printf("Error stopping Service: %d, please do it by hand.\n", err);
      return;
    }
  }

  if (!DeleteService(schService)) {
    printf("Error: DeleteService %d\n", GetLastError());
    return;
  } else
    printf("DeleteService SUCCESS\n");

  CloseServiceHandle(schService);
}

int main(int argc, char **argv) {
  int command;

  /* Build writable log path before any WriteToLog calls. */
  InitLogPath();

  remove(g_logFile);

  WriteToLog("RawPrintServer 1.01 created by Henk Jonas (www.metaviewsoft.de)");
  WriteToLog("PrintServer start");

  command = INVALID;

  if (1 < argc) {
    for (command = 0; command < (int)NUM_COMMANDS; command++) {
      /* Fix 6: _stricmp is the Windows standard; stricmp is deprecated. */
      if (0 == _stricmp(argv[1], commands[command].commandName))
        break;
    }

    if ((int)NUM_COMMANDS <= command)
      command = INVALID;
  }

  if (command != INVALID && argc == 2 + commands[command].maxCommandArgs - 1) {
    serverPort = 9100;
  } else if (command != INVALID &&
             argc == 2 + commands[command].maxCommandArgs) {
    int p = atoi(argv[2 + commands[command].maxCommandArgs - 1]);

    if (0 < p)
      serverPort = p;
    else
      serverPort = 9100;
  } else {
    char *lastPart = argv[0];
    char *p = argv[0];

    while (*p) {
      if (*p == ':' || *p == '/' || *p == '\\') {
        lastPart = p + 1;
      }
      p++;
    }

    fprintf(stderr, "%s INSTALL \"Printer Name\" [port]\n"
                    "%s REMOVE [port]\n"
                    "%s STANDALONE \"Printer Name\" [port]\n"
                    "%s BACKGROUND \"Printer Name\" [port]\n"
                    "If port is unspecified, 9100 is assumed.\n",
            lastPart, lastPart, lastPart, lastPart);
    return 1;
  }

  startPort = serverPort;
  sprintf(strServiceName, "RawPrintServer_%d", serverPort);

  switch (command) {
  case INSTALL:
    g_isService = 1;
    CreatePrintServer(argv[0], argv[2], serverPort, 1);
    break;
  case STANDALONE:
  case BACKGROUND: {
    WORD wVersionRequested;
    WSADATA wsaData;

    wVersionRequested = MAKEWORD(2, 2);
    regKey = standaloneKey;
    g_isService = 0;  /* HKCU will be used */

    CreatePrintServer(argv[0], argv[2], serverPort, 0);

    if (command == BACKGROUND)
      FreeConsole();

    int wsaErr = WSAStartup(wVersionRequested, &wsaData);
    if (wsaErr != 0) {
      printf("WSAStartup failed: %d\n", wsaErr);
      return 1;
    }

    printf("Listening on port %lu for printer \"%s\". Press Ctrl+C to stop.\n",
           (unsigned long)serverPort, printerName);

    while (InnerLoop(serverPort, 0))
      ;

    WSACleanup();
    break;
  }
  case REMOVE:
    DeletePrintServerService(serverPort);
    break;
  case PRIVATE_SERVICE:
    g_isService = 1;
    {
      SERVICE_TABLE_ENTRY ServiceTable[2];
      ServiceTable[0].lpServiceName = strServiceName;
      ServiceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)ServiceMain;

      ServiceTable[1].lpServiceName = NULL;
      ServiceTable[1].lpServiceProc = NULL;

      // Start the control dispatcher thread for our service
      StartServiceCtrlDispatcher(ServiceTable);
    }

    WriteToLog("PrintServer exit");
    break;
  default:
    break;
  }

  return 0;
}

int InnerLoop(DWORD port, int service) {
  char strTemp[256];
  DWORD valueSize;

  SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) {
    sprintf(strTemp, "Error: no socket: %d", WSAGetLastError());
    WriteToLog(strTemp);
    printf("%s\n", strTemp);
    if (service) {
      ServiceStatus.dwCurrentState = SERVICE_STOPPED;
      ServiceStatus.dwWin32ExitCode = 3;
      SetServiceStatus(hStatus, &ServiceStatus);
    }
    return 0;
  }

  /* Allow rapid restart on the same port */
  BOOL reuseAddr = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuseAddr,
             sizeof(reuseAddr));

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((u_short)serverPort);

  if (bind(sock, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
    sprintf(strTemp, "Error: couldn't bind to port %lu: %d",
            (unsigned long)serverPort, WSAGetLastError());
    WriteToLog(strTemp);
    printf("%s\n", strTemp);
    closesocket(sock);
    if (service) {
      ServiceStatus.dwCurrentState = SERVICE_STOPPED;
      ServiceStatus.dwWin32ExitCode = 4;
      SetServiceStatus(hStatus, &ServiceStatus);
    }
    return 0;
  }

  /* Fix 7: Call listen() once before the accept loop, not on every iteration. */
  if (listen(sock, 5) == SOCKET_ERROR) {
    sprintf(strTemp, "Error: listen failed: %d", WSAGetLastError());
    WriteToLog(strTemp);
    printf("%s\n", strTemp);
    closesocket(sock);
    return 0;
  }

  sockaddr_in client;
  int clientSize = sizeof(client);

  for (;;) {
    memset(&client, 0, sizeof(client));
    clientSize = sizeof(client);
    SOCKET sock2 = accept(sock, (struct sockaddr *)&client, &clientSize);
    if (sock2 == INVALID_SOCKET)
      break;

    HANDLE printer = NULL;

    /* Read the current printer name from the registry (allows hot-reconfiguration). */
    HKEY hdlKey = NULL;
    sprintf(strTemp, regKey, strServiceName);
    if (RegOpenKeyEx(RootKey(), strTemp, 0, KEY_READ, &hdlKey) == ERROR_SUCCESS) {
      valueSize = sizeof(printerName);
      RegQueryValueEx(hdlKey, "Printer", NULL, NULL, (BYTE *)printerName,
                      &valueSize);
      RegCloseKey(hdlKey);
    }
    /* If registry read failed, printerName retains the value set at startup. */

    sprintf(
        strTemp, "Accept print job for %s from %d.%d.%d.%d", printerName,
        client.sin_addr.S_un.S_un_b.s_b1, client.sin_addr.S_un.S_un_b.s_b2,
        client.sin_addr.S_un.S_un_b.s_b3, client.sin_addr.S_un.S_un_b.s_b4);
    WriteToLog(strTemp);
    printf("%s\n", strTemp);

    /* Fix 8: Use char arrays for DOC_INFO_1 string fields to satisfy the
       non-const LPWSTR/LPSTR requirement without undefined behaviour. */
    char docName[]  = "Forwarded Job";
    char dataType[] = "RAW";
    DOC_INFO_1 info;
    info.pDocName    = docName;
    info.pOutputFile = NULL;
    info.pDatatype   = dataType;

    DWORD jobId = 0;
    if (!OpenPrinter(printerName, &printer, NULL)) {
      sprintf(strTemp, "Error opening printer \"%s\": %lu", printerName,
              (unsigned long)GetLastError());
      WriteToLog(strTemp);
      printf("%s\n", strTemp);
    } else {
      jobId = StartDocPrinter(printer, 1, (LPBYTE)&info);
      if (jobId == 0) {
        sprintf(strTemp, "Error starting print job: %lu",
                (unsigned long)GetLastError());
        WriteToLog(strTemp);
        printf("%s\n", strTemp);
        ClosePrinter(printer);
        printer = NULL;
      }
    }

    if (printer != NULL && jobId != 0) {
      char buffer[4096];
      DWORD wrote;
      BOOL printOk = TRUE;
      while (printOk) {
        int result = recv(sock2, buffer, sizeof(buffer), 0);
        if (result <= 0)
          break;
        if (!WritePrinter(printer, buffer, (DWORD)result, &wrote) ||
            wrote != (DWORD)result) {
          WriteToLog("Couldn't print all data.");
          printf("Couldn't print all data.\n");
          printOk = FALSE;
        }
      }
      /* Fix 9: EndDocPrinter was missing ¡ª without it the spooler never
         marks the job as complete and the page may never be ejected. */
      EndDocPrinter(printer);
      ClosePrinter(printer);
      WriteToLog("Print job finished.");
      printf("Print job finished.\n");
    }

    closesocket(sock2);
  }

  if (service) {
    ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    ServiceStatus.dwWin32ExitCode = 5;
    SetServiceStatus(hStatus, &ServiceStatus);
  }
  closesocket(sock);
  return 1;
}

void ServiceMain(int argc, char **argv) {
  int error;
  char strTemp[256];

  ServiceStatus.dwServiceType = SERVICE_WIN32;
  ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
  ServiceStatus.dwControlsAccepted =
      SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  ServiceStatus.dwWin32ExitCode = 1;
  ServiceStatus.dwServiceSpecificExitCode = 0;
  ServiceStatus.dwCheckPoint = 0;
  ServiceStatus.dwWaitHint = 0;

  hStatus = RegisterServiceCtrlHandler(strServiceName,
                                       (LPHANDLER_FUNCTION)ControlHandler);
  if (hStatus == (SERVICE_STATUS_HANDLE)0) {
    // Registering Control Handler failed
    sprintf(strTemp, "Registering Control Handler failed %d", GetLastError());
    WriteToLog(strTemp);
    return;
  }

  // Initialize Service
  error = InitService();
  if (error) {
    // Initialization failed
    sprintf(strTemp, "Initialization failed %d", GetLastError());
    WriteToLog(strTemp);
    ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    ServiceStatus.dwWin32ExitCode = 2;
    SetServiceStatus(hStatus, &ServiceStatus);
    return;
  }

  // We report the running status to SCM.
  ServiceStatus.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(hStatus, &ServiceStatus);

  WORD wVersionRequested;
  WSADATA wsaData;
  int err;

  wVersionRequested = MAKEWORD(2, 2);
  err = WSAStartup(wVersionRequested, &wsaData);

  HKEY hdlKey = NULL;
  DWORD valueSize;
  sprintf(strTemp, regKey, strServiceName);
  if (RegOpenKeyEx(RootKey(), strTemp, 0, KEY_READ, &hdlKey) == ERROR_SUCCESS) {
    valueSize = sizeof(printerName);
    RegQueryValueEx(hdlKey, "Printer", NULL, NULL, (BYTE *)printerName,
                    &valueSize);
    valueSize = sizeof(serverPort);
    RegQueryValueEx(hdlKey, "Port", NULL, NULL, (BYTE *)&serverPort, &valueSize);
    RegCloseKey(hdlKey);
  }

  sprintf(strTemp, "%s on %d (%d)", printerName, serverPort, startPort);
  WriteToLog(strTemp);

  // The worker loop of a service
  while (ServiceStatus.dwCurrentState == SERVICE_RUNNING &&
         InnerLoop(serverPort, 1))
    ;

  WSACleanup();

  return;
}

// Service initialization
int InitService() { return 0; }

// Control handler function
void ControlHandler(DWORD request) {
  switch (request) {
  case SERVICE_CONTROL_STOP:
    WriteToLog("PrintServer stopped.");

    ServiceStatus.dwWin32ExitCode = 0;
    ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(hStatus, &ServiceStatus);
    return;

  case SERVICE_CONTROL_SHUTDOWN:
    WriteToLog("PrintServer stopped.");

    ServiceStatus.dwWin32ExitCode = 0;
    ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(hStatus, &ServiceStatus);
    return;

  default:
    break;
  }

  // Report current status
  SetServiceStatus(hStatus, &ServiceStatus);

  return;
}
