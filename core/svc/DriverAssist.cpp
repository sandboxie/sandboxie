/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
// Driver Assistant
//---------------------------------------------------------------------------

#include "stdafx.h"

#include <sddl.h>
#include <stdio.h>
#include <psapi.h>

#include "misc.h"
#include "DriverAssist.h"
#include "common/defines.h"
#include "common/my_version.h"
#include "core/dll/sbiedll.h"
#include "core/drv/api_defs.h"


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------

typedef struct _MSG_DATA
{
    void *ClassContext;
    UCHAR msg[MAX_PORTMSG_LENGTH];
} MSG_DATA;


DriverAssist *DriverAssist::m_instance = NULL;


//---------------------------------------------------------------------------
// Constructor
//---------------------------------------------------------------------------


DriverAssist::DriverAssist()
{
    m_PortHandle = NULL;
    m_Threads = NULL;
    m_DriverReady = false;

	m_last_message_number = 0;

    InitializeCriticalSection(&m_LogMessage_CritSec);
    InitializeCriticalSection(&m_critSecHostInjectedSvcs);
}


//---------------------------------------------------------------------------
// Initialize
//---------------------------------------------------------------------------


bool DriverAssist::Initialize()
{
    m_instance = new DriverAssist();
    ULONG tid;
    HANDLE hThread;

    if (!m_instance) {
        return false;
    }

    if (!m_instance->InjectLow_Init()) {
        return false;
    }
    if (!m_instance->InitializePortAndThreads()) {
        return false;
    }

    hThread = CreateThread(NULL, 0,
        (LPTHREAD_START_ROUTINE)StartDriverAsync, m_instance, 0, &tid);

    return true;
}


//---------------------------------------------------------------------------
// InitializePortAndThreads
//---------------------------------------------------------------------------


bool DriverAssist::InitializePortAndThreads()
{
    NTSTATUS status;
    UNICODE_STRING objname;
    OBJECT_ATTRIBUTES objattrs;
    WCHAR PortName[64];
    PSECURITY_DESCRIPTOR sd;
    ULONG i, n;

    //
    // create a security descriptor with a limited dacl
    // owner:system, group:system, dacl(allow;generic_all;system)
    //

    if (! ConvertStringSecurityDescriptorToSecurityDescriptor(
            L"O:SYG:SYD:(A;;GA;;;SY)", SDDL_REVISION_1, &sd, NULL)) {
        LogEvent(MSG_9234, 0x9244, GetLastError());
        return false;
    }

    //
    // create LPC port which the driver will use to send us messages
    // the port must have a name, or LpcRequestPort in SbieDrv will fail
    //

    wsprintf(PortName, L"%s-internal-%d",
             SbieDll_PortName(), GetTickCount());
    RtlInitUnicodeString(&objname, PortName);

    InitializeObjectAttributes(
        &objattrs, &objname, OBJ_CASE_INSENSITIVE, NULL, sd);

    status = NtCreatePort(
        (HANDLE *)&m_PortHandle, &objattrs, 0, MAX_PORTMSG_LENGTH, NULL);

    if (! NT_SUCCESS(status)) {
        LogEvent(MSG_9234, 0x9254, status);
        return false;
    }

    LocalFree(sd);

    //
    // make sure threads on other CPUs will see the port
    //

    InterlockedExchangePointer(&m_PortHandle, m_PortHandle);

    //
    // create the worker threads
    //

    n = (NUMBER_OF_THREADS) * sizeof(HANDLE);
    m_Threads = (HANDLE *)HeapAlloc(GetProcessHeap(), 0, n);
    if (! m_Threads) {
        LogEvent(MSG_9234, 0x9251, GetLastError());
        return false;
    }
    memzero(m_Threads, n);

    for (i = 0; i < NUMBER_OF_THREADS; ++i) {

        m_Threads[i] = CreateThread(
            NULL, 0, (LPTHREAD_START_ROUTINE)ThreadStub, this, 0, &n);
        if (! m_Threads[i]) {
            LogEvent(MSG_9234, 0x9253, GetLastError());
            return false;
        }
    }

    return true;
}


//---------------------------------------------------------------------------
// Shutdown
//---------------------------------------------------------------------------


void DriverAssist::Shutdown()
{
    if (m_instance) {

        m_instance->ShutdownPortAndThreads();

        delete m_instance;
        m_instance = NULL;
    }
}


//---------------------------------------------------------------------------
// ShutdownPortAndThreads
//---------------------------------------------------------------------------


void DriverAssist::ShutdownPortAndThreads()
{
    ULONG i;

    HANDLE PortHandle = InterlockedExchangePointer(&m_PortHandle, NULL);

    if (PortHandle) {

        UCHAR space[MAX_PORTMSG_LENGTH];

        for (i = 0; i < NUMBER_OF_THREADS; ++i) {
            PORT_MESSAGE *msg = (PORT_MESSAGE *)space;
            memzero(msg, MAX_PORTMSG_LENGTH);
            msg->u1.s1.TotalLength = (USHORT)sizeof(PORT_MESSAGE);
            NtRequestPort(PortHandle, msg);
        }
    }

    if (m_Threads) {

        if (WAIT_TIMEOUT == WaitForMultipleObjects(
                                NUMBER_OF_THREADS, m_Threads, TRUE, 5000)) {

            for (i = 0; i < NUMBER_OF_THREADS; ++i)
                TerminateThread(m_Threads[i], 0);
            WaitForMultipleObjects(NUMBER_OF_THREADS, m_Threads, TRUE, 5000);
        }
    }

    if (PortHandle)
        NtClose(PortHandle);
}


//---------------------------------------------------------------------------
// ThreadStub
//---------------------------------------------------------------------------


void DriverAssist::ThreadStub(void *parm)
{
    ((DriverAssist *)parm)->Thread();
}


//---------------------------------------------------------------------------
// Thread
//---------------------------------------------------------------------------

void DriverAssist::MsgWorkerThread(void *MyMsg)
{
    PORT_MESSAGE *msg = (PORT_MESSAGE *)MyMsg;
    //Null pointer checked by caller
    if (msg->u2.s2.Type != LPC_DATAGRAM) {
        return;
    }
    ULONG data_len = msg->u1.s1.DataLength;
    if (data_len < sizeof(ULONG)) {
        return;
    }
    data_len -= sizeof(ULONG);

    ULONG *data_ptr = (ULONG *)((UCHAR *)msg + sizeof(PORT_MESSAGE));
    ULONG msgid = *data_ptr;
    ++data_ptr;

    if (msgid == SVC_LOOKUP_SID) {

        LookupSid(data_ptr);

    }
    else if (msgid == SVC_INJECT_PROCESS) {

        InjectLow(data_ptr);

    }
    else if (msgid == SVC_CANCEL_PROCESS) {

        CancelProcess(data_ptr);

    }
    else if (msgid == SVC_UNMOUNT_HIVE) {

        UnmountHive(data_ptr);

    }
    else if (msgid == SVC_LOG_MESSAGE) {

        LogMessage();

    }
    else if (msgid == SVC_RESTART_HOST_INJECTED_SVCS) {

        RestartHostInjectedSvcs();
    }
}

DWORD DriverAssist::MsgWorkerThreadStub(void *MyMsg)
{
    if (!MyMsg) {
        return -1;
    }

    MSG_DATA* MsgData = (MSG_DATA*)MyMsg;
    ((DriverAssist *)(MsgData->ClassContext))->MsgWorkerThread(&MsgData->msg[0]);
    //Memory allocated in parent thread
    VirtualFree(MyMsg, 0, MEM_RELEASE);

    return NO_ERROR;
}

void DriverAssist::Thread()
{
    NTSTATUS status;
    DWORD threadId;
    MSG_DATA *MsgData;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (1) {

        MsgData = (MSG_DATA*)VirtualAlloc(0, sizeof(MSG_DATA), MEM_COMMIT, PAGE_READWRITE);
        if (!MsgData) {
            break;  // out of memory
        }

        status = NtReplyWaitReceivePort(m_PortHandle, NULL, NULL, (PORT_MESSAGE *)MsgData->msg);

        if (!m_PortHandle) {    // service is shutting down
            VirtualFree(MsgData, 0, MEM_RELEASE);
            break;
        }

        MsgData->ClassContext = this;
        CreateThread(NULL, 0, MsgWorkerThreadStub, (void *)MsgData, 0, &threadId);
    }
}


//---------------------------------------------------------------------------
// LookupSid
//---------------------------------------------------------------------------


void DriverAssist::LookupSid(void *_msg)
{
    SVC_LOOKUP_SID_MSG *msg = (SVC_LOOKUP_SID_MSG *)_msg;

    PSID pSid;
    BOOL b = ConvertStringSidToSid(msg->sid_string, &pSid);
    if (! b) {
        SbieApi_LogEx(msg->session_id, 2209, L"[11 / %d]", GetLastError());
        return;
    }

    WCHAR username[256];
    ULONG username_len = sizeof(username) / sizeof(WCHAR) - 4;
    WCHAR domain[256];
    ULONG domain_len = sizeof(domain) / sizeof(WCHAR) - 4;
    SID_NAME_USE use;

    username[0] = L'\0';

    b = LookupAccountSid(
        NULL, pSid, username, &username_len, domain, &domain_len, &use);

    if ((! b) && GetLastError() == ERROR_NONE_MAPPED) {

        username_len = sizeof(username) / sizeof(WCHAR) - 4;
        username[0] = L'\0';
        LookupSid2(msg->sid_string, username, username_len);
        if (username[0])
            b = TRUE;
        else
            SetLastError(ERROR_NONE_MAPPED);
    }

    if ((! b) || (! username[0])) {
        //SbieApi_LogEx(msg->session_id, 2209, L": %S [22 / %d]", msg->sid_string, GetLastError());
        wcscpy(username, L"*?*?*?*");
    }

    LocalFree(pSid);

    username[sizeof(username) / sizeof(WCHAR) - 4] = L'\0';

    LONG rc = SbieApi_SetUserName(msg->sid_string, username);
    if (rc != 0)
        SbieApi_LogEx(msg->session_id, 2209, L"[33 / %08X]", rc);
}


//---------------------------------------------------------------------------
// LookupSid2
//---------------------------------------------------------------------------


void DriverAssist::LookupSid2(
    const WCHAR *SidString, WCHAR *UserName, ULONG UserNameLen)
{
    WCHAR *KeyPath = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, 1024);
    if (! KeyPath)
        return;
    wcscpy(KeyPath, SidString);
    wcscat(KeyPath,
                L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer");

    HKEY hKey;
    LONG rc = RegOpenKeyEx(HKEY_USERS, KeyPath, 0, KEY_READ, &hKey);
    if (rc == 0) {

        ULONG type, len = UserNameLen;
        rc = RegQueryValueEx(hKey, L"Logon User Name", NULL,
                             &type, (LPBYTE)UserName, &len);
        if (rc != 0 || type != REG_SZ)
            UserName[0] = L'\0';

        RegCloseKey(hKey);
    }

    HeapFree(GetProcessHeap(), HEAP_GENERATE_EXCEPTIONS, KeyPath);
}


//---------------------------------------------------------------------------
// CancelProcess
//---------------------------------------------------------------------------


void DriverAssist::CancelProcess(void *_msg)
{
    //
    // cancel process in response to request from driver
    //

    SVC_PROCESS_MSG *msg = (SVC_PROCESS_MSG *)_msg;

    const ULONG _DesiredAccess = PROCESS_TERMINATE
                               | PROCESS_QUERY_INFORMATION;

    HANDLE hProcess = OpenProcess(_DesiredAccess, FALSE, msg->process_id);

    if (hProcess) {

        FILETIME time, time1, time2, time3;
        BOOL ok = GetProcessTimes(hProcess, &time, &time1, &time2, &time3);
        if (ok && *(ULONG64 *)&time.dwLowDateTime == msg->create_time) {

            TerminateProcess(hProcess, 1);
        }

        CloseHandle(hProcess);
    }

    if (msg->reason != 0)
        SbieApi_LogEx(msg->session_id, 2314, L"%S [%d / %d]", msg->process_name, msg->process_id, msg->reason);
    else
        SbieApi_LogEx(msg->session_id, 2314, msg->process_name);
}


extern void RestartHostInjectedSvcs();

void DriverAssist::RestartHostInjectedSvcs()
{
    EnterCriticalSection(&m_critSecHostInjectedSvcs);
    ::RestartHostInjectedSvcs();
    LeaveCriticalSection(&m_critSecHostInjectedSvcs);
}


//---------------------------------------------------------------------------
// UnmountHive
//---------------------------------------------------------------------------


void DriverAssist::UnmountHive(void *_msg)
{
    SVC_UNMOUNT_MSG *msg = (SVC_UNMOUNT_MSG *)_msg;
    ULONG rc, retries;

    //
    // we got a message that specifies the pid of the last process in
    // a box, we're going to wait until that process disappears
    //

    bool ended = false;

    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, msg->process_id);
    if (hProcess) {

        if (WaitForSingleObject(hProcess, 2 * 1000) == STATUS_SUCCESS) {

            ended = true;
        }

        CloseHandle(hProcess);
    }

    if (! ended) {

        for (retries = 0; retries < 20; ++retries) {

            rc = SbieApi_QueryProcess((HANDLE)(ULONG_PTR)msg->process_id,
                                      NULL, NULL, NULL, NULL);
            if (rc != 0)
                break;

            Sleep(100);
        }
    }

    //
    // it could be that we are invoked because Start.exe terminated, but
    // its spawned child process has not yet asked to mount the registry
    // for it.  I.e.,  the registry use count has dropped to zero, even
    // though another process is going to ask for that registry very soon.
    //
    // to make sure we don't unmount in this case, only to re-mount,
    // we check that the sandbox is empty, before issuing the unmount
    //

    bool ShouldUnmount = false;

    ULONG *pids = (ULONG *)HeapAlloc(GetProcessHeap(), 0, PAGE_SIZE);

    if (pids) {

        for (retries = 0; retries < 20; ++retries) {

            rc = SbieApi_EnumProcessEx(
                                msg->boxname, FALSE, msg->session_id, pids);
            if (rc == 0 && *pids == 0) {

                ShouldUnmount = true;
                break;
            }

            Sleep(100);
        }

        HeapFree(GetProcessHeap(), 0, pids);
    }

    //
    // unmount.  on Windows 2000, the process may appear to disappear
    // even before its handles were all closed (in particular, registry
    // handles), which could lead to SBIE2208 being reported.  so we
    // retry the operation
    //

    while (ShouldUnmount) {

        WCHAR root_path[256];
        UNICODE_STRING root_uni;
        OBJECT_ATTRIBUTES root_objattrs;
        HANDLE root_key;

        SbieApi_GetUnmountHive(root_path);
        if (! root_path[0])
            break;

        RtlInitUnicodeString(&root_uni, root_path);
        InitializeObjectAttributes(&root_objattrs,
            &root_uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

        for (retries = 0; retries < 25; ++retries) {

            rc = NtUnloadKey(&root_objattrs);
            if (rc == 0)
                break;

            Sleep(100);

            rc = NtOpenKey(&root_key, KEY_READ, &root_objattrs);
            if (rc == STATUS_OBJECT_NAME_NOT_FOUND ||
                rc == STATUS_OBJECT_PATH_NOT_FOUND)
                break;
            if (rc == 0)
                NtClose(root_key);
        }

        if (rc != 0)
            SbieApi_LogEx(msg->session_id, 2208, L"[%08X]", rc);

        break;
    }
}


//---------------------------------------------------------------------------
// SbieLow Injection
//---------------------------------------------------------------------------


#include "DriverAssistStart.cpp"
#include "DriverAssistInject.cpp"
#include "DriverAssistLog.cpp"
