#include "stdafx.h"

#include "resource.h"
#include "Console.h"

#include "../shared/SharedMemNames.h"
#include "ConsoleException.h"
#include "ConsoleHandler.h"

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<Mutex>	ConsoleHandler::s_parentProcessWatchdog;
std::shared_ptr<void>	ConsoleHandler::s_environmentBlock;

//////////////////////////////////////////////////////////////////////////////

ConsoleHandler::ConsoleHandler()
: m_hConsoleProcess()
, m_consoleParams()
, m_consoleInfo()
, m_consoleBuffer()
, m_consoleCopyInfo()
, m_consoleMouseEvent()
, m_newConsoleSize()
, m_newScrollPos()
, m_hMonitorThread()
, m_hMonitorThreadExit(std::shared_ptr<void>(::CreateEvent(NULL, FALSE, FALSE, NULL), ::CloseHandle))
, m_bufferMutex(NULL, FALSE, NULL)
, m_dwConsolePid(0)
, m_boolIsElevated(false)
{
}

ConsoleHandler::~ConsoleHandler()
{
	if( m_hMonitorThread.get() )
		StopMonitorThread();

	if ((m_consoleParams.Get() != NULL) && 
		(m_consoleParams->hwndConsoleWindow))
	{
		SendMessage(WM_CLOSE, 0, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::SetupDelegates(ConsoleChangeDelegate consoleChangeDelegate, ConsoleCloseDelegate consoleCloseDelegate)
{
	m_consoleChangeDelegate	= consoleChangeDelegate;
	m_consoleCloseDelegate	= consoleCloseDelegate;
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::RunAsAdministrator
(
	const wstring& strSyncName,
	const wstring& strTitle,
	const wstring& strInitialDir,
	const wstring& strInitialCmd
)
{
	std::wstring strFile = Helpers::GetModuleFileName(nullptr);

	std::wstring strParams;
	// admin sync name
	strParams += L"-a ";
	strParams += Helpers::EscapeCommandLineArg(strSyncName);
	// config file
	strParams += L" -c ";
	strParams += Helpers::EscapeCommandLineArg(g_settingsHandler->GetSettingsFileName());
	// tab name
	strParams += L" -t ";
	strParams += Helpers::EscapeCommandLineArg(strTitle);
	// directory
	if (!strInitialDir.empty())
	{
		strParams += L" -d ";
		strParams += Helpers::EscapeCommandLineArg(strInitialDir);
	}
	// startup shell command
	if (!strInitialCmd.empty())
	{
		strParams += L" -r ";
		strParams += Helpers::EscapeCommandLineArg(strInitialCmd);
	}

	SHELLEXECUTEINFO sei = {sizeof(sei)};

	sei.hwnd = nullptr;
	sei.fMask = /*SEE_MASK_NOCLOSEPROCESS|*/SEE_MASK_NOASYNC;
	sei.lpVerb = L"runas";
	sei.lpFile = strFile.c_str();
	sei.lpParameters = strParams.length() > 0 ? strParams.c_str() : nullptr;
	sei.lpDirectory = nullptr,
	sei.nShow = SW_SHOWMINIMIZED;

	if(!::ShellExecuteEx(&sei))
	{
		Win32Exception err(::GetLastError());
		throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_CANT_START_SHELL_AS_ADMIN)) % strFile % strParams % err.what()));
	}
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::CreateShellProcess
(
	const wstring& strShell,
	const wstring& strInitialDir,
	const UserCredentials& userCredentials,
	const wstring& strInitialCmd,
	const wstring& strConsoleTitle,
	PROCESS_INFORMATION& pi
)
{
	std::unique_ptr<void, DestroyEnvironmentBlockHelper> userEnvironment;
	std::unique_ptr<void, CloseHandleHelper>             userToken;

	if (userCredentials.strUsername.length() > 0)
	{
		if (!userCredentials.netOnly)
		{
			// logon user
			HANDLE hUserToken = NULL;
			if( !::LogonUser(
				userCredentials.strUsername.c_str(),
				userCredentials.strDomain.length() > 0 ? userCredentials.strDomain.c_str() : NULL,
				userCredentials.password.c_str(),
				LOGON32_LOGON_INTERACTIVE,
				LOGON32_PROVIDER_DEFAULT,
				&hUserToken) || !::ImpersonateLoggedOnUser(hUserToken) )
			{
				Win32Exception err(::GetLastError());
				throw ConsoleException(boost::str(boost::wformat(Helpers::LoadStringW(IDS_ERR_CANT_START_SHELL_AS_USER)) % L"?" % userCredentials.user % err.what()));
			}
			userToken.reset(hUserToken);

			/*
			// load user's profile
			// seems to be necessary on WinXP for environment strings' expainsion to work properly
			PROFILEINFO userProfile;
			::ZeroMemory(&userProfile, sizeof(PROFILEINFO));
			userProfile.dwSize = sizeof(PROFILEINFO);
			userProfile.lpUserName = const_cast<wchar_t*>(userCredentials.strUsername.c_str());

			::LoadUserProfile(userToken.get(), &userProfile);
			userProfileKey.reset(userProfile.hProfile, bind<BOOL>(::UnloadUserProfile, userToken.get(), _1));
			*/

			// load user's environment
			void*	pEnvironment = nullptr;
			if( !::CreateEnvironmentBlock(&pEnvironment, userToken.get(), FALSE) )
			{
				Win32Exception err(::GetLastError());
				::RevertToSelf();
				throw ConsoleException(boost::str(boost::wformat(Helpers::LoadStringW(IDS_ERR_CANT_START_SHELL_AS_USER)) % L"?" % userCredentials.user % err.what()));
			}
			userEnvironment.reset(pEnvironment);
		}
	}

	wstring	strShellCmdLine(strShell);

	if (strShellCmdLine.length() == 0)
	{
		wchar_t	szComspec[MAX_PATH];

		::ZeroMemory(szComspec, MAX_PATH*sizeof(wchar_t));

		if (userEnvironment.get())
		{
			// resolve comspec when running as another user
			wchar_t* pszComspec = reinterpret_cast<wchar_t*>(userEnvironment.get());

			while ((pszComspec[0] != L'\x00') && (_wcsnicmp(pszComspec, L"comspec", 7) != 0)) pszComspec += wcslen(pszComspec)+1;

			if (pszComspec[0] != L'\x00')
			{
				strShellCmdLine = (pszComspec + 8);
			}

			if (strShellCmdLine.length() == 0) strShellCmdLine = L"cmd.exe";
		}
		else
		{
			if (::GetEnvironmentVariable(L"COMSPEC", szComspec, MAX_PATH) > 0)
			{
				strShellCmdLine = szComspec;
			}

			if (strShellCmdLine.length() == 0) strShellCmdLine = L"cmd.exe";
		}
	}

	if (strInitialCmd.length() > 0)
	{
		strShellCmdLine += L" ";
		strShellCmdLine += strInitialCmd;
	}

	wstring	strStartupTitle(strConsoleTitle);

	if (strStartupTitle.length() == 0)
	{
		strStartupTitle = L"ConsoleZ command window";
		//		strStartupTitle = boost::str(boost::wformat(L"Console2 command window 0x%08X") % this);
	}

	wstring strStartupDir(
		userToken.get() ?
		Helpers::ExpandEnvironmentStringsForUser(userToken.get(), strInitialDir) :
		Helpers::ExpandEnvironmentStrings(strInitialDir));

	if (strStartupDir.length() > 0)
	{
		if ((*(strStartupDir.end() - 1) == L'\"') && (*strStartupDir.begin() != L'\"'))
		{
			// startup dir name ends with ", but doesn't start with ", the user passed
			// something like "C:\" as the parameter, it got parsed to C:", remove the trailing "
			//
			// This is a common mistake, thus the check...
			strStartupDir = strStartupDir.substr(0, strStartupDir.length()-1);
		}

		// startup dir doesn't end with \, add it
		if (*(strStartupDir.end() - 1) != L'\\') strStartupDir += L'\\';

		// check if startup directory exists
		DWORD dwDirAttributes = ::GetFileAttributes(strStartupDir.c_str());

		if ((dwDirAttributes == INVALID_FILE_ATTRIBUTES) ||
			(dwDirAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			// no directory, use Console.exe directory
			strStartupDir = Helpers::GetModulePath(NULL);
		}
	}

	wstring strCmdLine(
		userToken.get() ?
		Helpers::ExpandEnvironmentStringsForUser(userToken.get(), strShellCmdLine) :
		Helpers::ExpandEnvironmentStrings(strShellCmdLine));

	if( userToken.get() )
		::RevertToSelf();

	// setup the startup info struct
	STARTUPINFO si;
	::ZeroMemory(&si, sizeof(STARTUPINFO));

	si.cb			= sizeof(STARTUPINFO);
	si.lpTitle		= const_cast<wchar_t*>(strStartupTitle.c_str());

	if (g_settingsHandler->GetConsoleSettings().bStartHidden)
	{
		// Starting Windows console window hidden causes problems with 
		// some GUI apps started from Console that use SW_SHOWDEFAULT to 
		// initially show their main window (i.e. the window inherits our 
		// SW_HIDE flag and remains invisible :-)
		si.dwFlags		= STARTF_USESHOWWINDOW;
		si.wShowWindow	= SW_HIDE;
	}
	else
	{
		// To avoid invisible GUI windows, default settings will create
		// a Windows console window far offscreen and hide the window
		// after it has been created.
		//
		// This approach can flash console window's taskbar button and
		// taskbar button can sometimes remain visible, but nothing is perfect :)
		si.dwFlags		= STARTF_USEPOSITION;
		si.dwX			= 0x7FFF;
		si.dwY			= 0x7FFF;
	}

	// we must use CREATE_UNICODE_ENVIRONMENT here, since s_environmentBlock contains Unicode strings
	DWORD dwStartupFlags = CREATE_NEW_CONSOLE|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT;

	// TODO: not supported yet
	//if (bDebugFlag) dwStartupFlags |= DEBUG_PROCESS;

	if (userCredentials.strUsername.length() > 0)
	{
		if( !::CreateProcessWithLogonW(
			userCredentials.strUsername.c_str(),
			userCredentials.strDomain.length() > 0 ? userCredentials.strDomain.c_str() : NULL,
			userCredentials.password.c_str(), 
			userCredentials.netOnly? LOGON_NETCREDENTIALS_ONLY : LOGON_WITH_PROFILE,
			NULL,
			const_cast<wchar_t*>(strCmdLine.c_str()),
			dwStartupFlags,
			s_environmentBlock.get(),
			(strStartupDir.length() > 0) ? const_cast<wchar_t*>(strStartupDir.c_str()) : NULL,
			&si,
			&pi))
		{
			Win32Exception err(::GetLastError());
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadStringW(IDS_ERR_CANT_START_SHELL_AS_USER)) % strShellCmdLine % userCredentials.user % err.what()));
		}
	}
	else
	{
		if (!::CreateProcess(
			NULL,
			const_cast<wchar_t*>(strCmdLine.c_str()),
			NULL,
			NULL,
			FALSE,
			dwStartupFlags,
			s_environmentBlock.get(),
			(strStartupDir.length() > 0) ? const_cast<wchar_t*>(strStartupDir.c_str()) : NULL,
			&si,
			&pi))
		{
			Win32Exception err(::GetLastError());
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_CANT_START_SHELL)) % strShellCmdLine % err.what()));
		}
	}
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::StartShellProcess
(
	const wstring& strTitle,
	const wstring& strShell,
	const wstring& strInitialDir,
	const UserCredentials& userCredentials,
	const wstring& strInitialCmd,
	const wstring& strConsoleTitle,
	DWORD dwStartupRows,
	DWORD dwStartupColumns
)
{
	PROCESS_INFORMATION pi = {0, 0, 0, 0};

	bool runAsAdministrator = userCredentials.runAsAdministrator;
	bool isElevated = false;

	try
	{
		if (Helpers::CheckOSVersion(6, 0))
		{
			if( Helpers::IsElevated() )
			{
				// process already running in elevated mode or UAC disabled
				runAsAdministrator = false;
				isElevated = true;
			}
		}
		else
		{
			// UAC doesn't exist in current OS
			runAsAdministrator = false;
		}
	}
	catch(std::exception& err)
	{
		if (runAsAdministrator)
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_CANT_GET_ELEVATION_TYPE)) % err.what()));
	}

	m_boolIsElevated = isElevated || runAsAdministrator;

	SharedMemory<DWORD> pid;

	if (runAsAdministrator)
	{
		std::wstring strSyncName = (SharedMemNames::formatAdmin % ::GetCurrentProcessId()).str();

		pid.Create(strSyncName, 1, syncObjBoth, L"");

		RunAsAdministrator(
			strSyncName,
			strTitle,
			strInitialDir,
			strInitialCmd
		);

		// wait for PID of shell launched in admin ConsoleZ
		if (::WaitForSingleObject(pid.GetReqEvent(), 10000) == WAIT_TIMEOUT)
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % L"timeout"));

		pi.dwProcessId = *pid.Get();
		pi.hProcess = ::OpenProcess(SYNCHRONIZE, FALSE, pi.dwProcessId);
		if( pi.hProcess == NULL )
		{
			Win32Exception err(::GetLastError());
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % err.what()));
		}
	}
	else
	{
		CreateShellProcess(
			strShell,
			strInitialDir,
			userCredentials,
			strInitialCmd,
			strConsoleTitle,
			pi
		);
	}

	// create shared memory objects
	try
	{
		CreateSharedObjects(pi.dwProcessId, userCredentials.netOnly? L"" : userCredentials.strAccountName);
		CreateWatchdog();
	}
	catch(Win32Exception& err)
	{
		throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_CREATE_SHARED_OBJECTS_FAILED)) % err.what()));
	}

	// write startup params
	m_consoleParams->dwParentProcessId     = ::GetCurrentProcessId();
	m_consoleParams->dwNotificationTimeout = g_settingsHandler->GetConsoleSettings().dwChangeRefreshInterval;
	m_consoleParams->dwRefreshInterval     = g_settingsHandler->GetConsoleSettings().dwRefreshInterval;
	m_consoleParams->dwRows                = dwStartupRows;
	m_consoleParams->dwColumns             = dwStartupColumns;
	m_consoleParams->dwBufferRows          = g_settingsHandler->GetConsoleSettings().dwBufferRows;
	m_consoleParams->dwBufferColumns       = g_settingsHandler->GetConsoleSettings().dwBufferColumns;

	m_hConsoleProcess = std::shared_ptr<void>(pi.hProcess, ::CloseHandle);
	m_dwConsolePid    = pi.dwProcessId;

	if (runAsAdministrator)
	{
		::SetEvent(pid.GetRespEvent());
	}
	else
	{
		// inject our hook DLL into console process
		if (!InjectHookDLL(pi))
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % L"?"));

		// resume the console process
		::ResumeThread(pi.hThread);
		::CloseHandle(pi.hThread);
	}

	try
	{
		m_consoleMsgPipe.WaitConnect();
	}
	catch(Win32Exception& err)
	{
		throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % err.what()));
	}

	// wait for hook DLL to set console handle
	if (::WaitForSingleObject(m_consoleParams.GetReqEvent(), 10000) == WAIT_TIMEOUT)
		throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % L"timeout"));

	ShowWindow(SW_HIDE);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::StartShellProcessAsAdministrator
(
	const wstring& strSyncName,
	const wstring& strShell,
	const wstring& strInitialDir,
	const wstring& strInitialCmd
)
{
	SharedMemory<DWORD> pid;
	pid.Open(strSyncName, syncObjBoth);

	UserCredentials userCredentials;
	PROCESS_INFORMATION pi = {0, 0, 0, 0};

	CreateShellProcess(
		strShell,
		strInitialDir,
		userCredentials,
		strInitialCmd,
		L"",
		pi
	);

	*pid.Get() = pi.dwProcessId;
	::SetEvent(pid.GetReqEvent());

	// wait for shared objects creation
	if (::WaitForSingleObject(pid.GetRespEvent(), 10000) == WAIT_TIMEOUT)
		throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % L"timeout"));

	// inject our hook DLL into console process
	if (!InjectHookDLL(pi))
		throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % L"?"));

	// resume the console process
	::ResumeThread(pi.hThread);
	::CloseHandle(pi.hThread);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

DWORD ConsoleHandler::StartMonitorThread()
{
	DWORD dwThreadId = 0;
	m_hMonitorThread = std::shared_ptr<void>(
		::CreateThread(
		NULL,
		0, 
		MonitorThreadStatic, 
		reinterpret_cast<void*>(this), 
		0, 
		&dwThreadId),
		::CloseHandle);

	return dwThreadId;
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::StopMonitorThread()
{
	::SetEvent(m_hMonitorThreadExit.get());
	::WaitForSingleObject(m_hMonitorThread.get(), 10000);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::SendMouseEvent(const COORD& mousePos, DWORD dwMouseButtonState, DWORD dwControlKeyState, DWORD dwEventFlags)
{
	{
		SharedMemoryLock	memLock(m_consoleMouseEvent);

		// TODO: implement
		m_consoleMouseEvent->dwMousePosition	= mousePos;
		m_consoleMouseEvent->dwButtonState		= dwMouseButtonState;
		m_consoleMouseEvent->dwControlKeyState	= dwControlKeyState;
		m_consoleMouseEvent->dwEventFlags		= dwEventFlags;

		m_consoleMouseEvent.SetReqEvent();
	}

	::WaitForSingleObject(m_consoleMouseEvent.GetRespEvent(), INFINITE);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::StopScrolling()
{
	// emulate 'Mark' sysmenu item click in Windows console window (will stop scrolling until the user presses ESC)
	// or a selection is cleared (copied or not)
	SendMessage(WM_SYSCOMMAND, SC_CONSOLE_MARK, 0);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::ResumeScrolling()
{
	// emulate ESC keypress to end 'mark' command (we send a mark command just in case 
	// a user has already pressed ESC as I don't know an easy way to detect if the mark
	// command is active or not)
	SendMessage(WM_SYSCOMMAND, SC_CONSOLE_MARK, 0);
	SendMessage(WM_KEYDOWN,    VK_ESCAPE,       0x00010001);
	SendMessage(WM_KEYUP,      VK_ESCAPE,       0xC0010001);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::UpdateEnvironmentBlock()
{
	void*	pEnvironment	= NULL;
	HANDLE	hProcessToken	= NULL;

	::OpenProcessToken(::GetCurrentProcess(), TOKEN_ALL_ACCESS, &hProcessToken);
	::CreateEnvironmentBlock(&pEnvironment, hProcessToken, FALSE);
	::CloseHandle(hProcessToken);

	s_environmentBlock.reset(pEnvironment, ::DestroyEnvironmentBlock);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

bool ConsoleHandler::CreateSharedObjects(DWORD dwConsoleProcessId, const wstring& strUser)
{
	// create startup params shared memory
	m_consoleParams.Create((SharedMemNames::formatConsoleParams % dwConsoleProcessId).str(), 1, syncObjBoth, strUser);

	// create console info shared memory
	m_consoleInfo.Create((SharedMemNames::formatInfo % dwConsoleProcessId).str(), 1, syncObjRequest, strUser);

	// create console info shared memory
	m_cursorInfo.Create((SharedMemNames::formatCursorInfo % dwConsoleProcessId).str(), 1, syncObjRequest, strUser);

	// TODO: max console size
	m_consoleBuffer.Create((SharedMemNames::formatBuffer % dwConsoleProcessId).str(), 200*200, syncObjRequest, strUser);

	// initialize buffer with spaces
	CHAR_INFO ci;
	ci.Attributes		= 0;
	ci.Char.UnicodeChar	= L' ';
	for (int i = 0; i < 200*200; ++i) ::CopyMemory(&m_consoleBuffer[i], &ci, sizeof(CHAR_INFO));

	// copy info
	m_consoleCopyInfo.Create((SharedMemNames::formatCopyInfo % dwConsoleProcessId).str(), 1, syncObjBoth, strUser);

	// mouse event
	m_consoleMouseEvent.Create((SharedMemNames::formatMouseEvent % dwConsoleProcessId).str(), 1, syncObjBoth, strUser);

	// new console size
	m_newConsoleSize.Create((SharedMemNames::formatNewConsoleSize % dwConsoleProcessId).str(), 1, syncObjRequest, strUser);

	// new scroll position
	m_newScrollPos.Create((SharedMemNames::formatNewScrollPos % dwConsoleProcessId).str(), 1, syncObjRequest, strUser);

	// message pipe (workaround for User Interface Privilege Isolation messages filtering)
	m_consoleMsgPipe.Create((SharedMemNames::formatPipeName % dwConsoleProcessId).str(), strUser);

	// TODO: separate function for default settings
	m_consoleParams->dwRows		= 25;
	m_consoleParams->dwColumns	= 80;

	return true;
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::CreateWatchdog()
{
	if (!s_parentProcessWatchdog)
	{
		std::shared_ptr<void>	sd;	// PSECURITY_DESCRIPTOR

		sd.reset(::LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH), ::LocalFree);
		if (::InitializeSecurityDescriptor(sd.get(), SECURITY_DESCRIPTOR_REVISION))
		{
			::SetSecurityDescriptorDacl(
				sd.get(), 
				TRUE,		// bDaclPresent flag   
				NULL,		// full access to everyone
				FALSE);		// not a default DACL 
		}

		SECURITY_ATTRIBUTES	sa;

		::ZeroMemory(&sa, sizeof(SECURITY_ATTRIBUTES));
		sa.nLength				= sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle		= FALSE;
		sa.lpSecurityDescriptor	= sd.get();

		s_parentProcessWatchdog.reset(new Mutex(&sa, TRUE, (LPCTSTR)((SharedMemNames::formatWatchdog % ::GetCurrentProcessId()).str().c_str())));
	}
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

bool ConsoleHandler::InjectHookDLL(PROCESS_INFORMATION& pi)
{
	// allocate memory for parameter in the remote process
	wstring				strHookDllPath(GetModulePath(NULL));

	CONTEXT		context;
	
	void*		mem				= NULL;
	size_t		memLen			= 0;
	UINT_PTR	fnLoadLibrary	= NULL;

	size_t		codeSize;
	BOOL		isWow64Process	= FALSE;

#ifdef _WIN64
	WOW64_CONTEXT 	wow64Context;
	DWORD			fnWow64LoadLibrary	= 0;

	::ZeroMemory(&wow64Context, sizeof(WOW64_CONTEXT));
	::IsWow64Process(pi.hProcess, &isWow64Process);
	codeSize = isWow64Process ? 20 : 91;
#else
	codeSize = 20;
#endif

	if (isWow64Process)
	{
		// starting a 32-bit process from a 64-bit console
		strHookDllPath += wstring(L"\\ConsoleHook32.dll");
	}
	else
	{
		// same bitness :-)
		strHookDllPath += wstring(L"\\ConsoleHook.dll");
	}

  if (::GetFileAttributes(strHookDllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_HOOK_MISSING)) % strHookDllPath.c_str()));

	::ZeroMemory(&context, sizeof(CONTEXT));

	memLen = (strHookDllPath.length()+1)*sizeof(wchar_t);
	std::unique_ptr<BYTE[]> code(new BYTE[codeSize + memLen]);

	::CopyMemory(code.get() + codeSize, strHookDllPath.c_str(), memLen);
	memLen += codeSize;

#ifdef _WIN64

	if (isWow64Process)
	{
		wow64Context.ContextFlags = CONTEXT_FULL;
		::Wow64GetThreadContext(pi.hThread, &wow64Context);

		mem = ::VirtualAllocEx(pi.hProcess, NULL, memLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

		// get 32-bit kernel32
		wstring strConsoleWowPath(GetModulePath(NULL) + wstring(L"\\ConsoleWow.exe"));

		STARTUPINFO siWow;
		::ZeroMemory(&siWow, sizeof(STARTUPINFO));

		siWow.cb			= sizeof(STARTUPINFO);
		siWow.dwFlags		= STARTF_USESHOWWINDOW;
		siWow.wShowWindow	= SW_HIDE;
		
		PROCESS_INFORMATION piWow;

		if (!::CreateProcess(
				NULL,
				const_cast<wchar_t*>(strConsoleWowPath.c_str()),
				NULL,
				NULL,
				FALSE,
				0,
				NULL,
				NULL,
				&siWow,
				&piWow))
		{
			Win32Exception err(::GetLastError());
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_CANT_START_SHELL)) % strConsoleWowPath.c_str() % err.what()));
		}

		std::shared_ptr<void> wowProcess(piWow.hProcess, ::CloseHandle);
		std::shared_ptr<void> wowThread(piWow.hThread, ::CloseHandle);

		if (::WaitForSingleObject(wowProcess.get(), 5000) == WAIT_TIMEOUT)
		{
			throw ConsoleException(boost::str(boost::wformat(Helpers::LoadString(IDS_ERR_DLL_INJECTION_FAILED)) % L"timeout"));
		}

		::GetExitCodeProcess(wowProcess.get(), reinterpret_cast<DWORD*>(&fnWow64LoadLibrary));
	}
	else
	{
		context.ContextFlags = CONTEXT_FULL;
		::GetThreadContext(pi.hThread, &context);

		mem = ::VirtualAllocEx(pi.hProcess, NULL, memLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		fnLoadLibrary = (UINT_PTR)::GetProcAddress(::GetModuleHandle(L"kernel32.dll"), "LoadLibraryW");
	}


#else
	context.ContextFlags = CONTEXT_FULL;
	::GetThreadContext(pi.hThread, &context);

	mem = ::VirtualAllocEx(pi.hProcess, NULL, memLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	fnLoadLibrary = (UINT_PTR)::GetProcAddress(::GetModuleHandle(L"kernel32.dll"), "LoadLibraryW");
#endif

	union
	{
		PBYTE  pB;
		PINT   pI;
		PULONGLONG pL;
	} ip;

	ip.pB = code.get();

#ifdef _WIN64

	if (isWow64Process)
	{
		*ip.pB++ = 0x68;			// push  eip
		*ip.pI++ = wow64Context.Eip;
		*ip.pB++ = 0x9c;			// pushf
		*ip.pB++ = 0x60;			// pusha
		*ip.pB++ = 0x68;			// push  "path\to\our.dll"
		*ip.pI++ = static_cast<INT>(reinterpret_cast<UINT_PTR>(mem) + codeSize);
		*ip.pB++ = 0xe8;			// call  LoadLibraryW
		*ip.pI++ =  static_cast<INT>(fnWow64LoadLibrary - (reinterpret_cast<UINT_PTR>(mem) + (ip.pB+4 - code.get())));
		*ip.pB++ = 0x61;			// popa
		*ip.pB++ = 0x9d;			// popf
		*ip.pB++ = 0xc3;			// ret

		::WriteProcessMemory(pi.hProcess, mem, code.get(), memLen, NULL);
		::FlushInstructionCache(pi.hProcess, mem, memLen);
		wow64Context.Eip = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(mem));
		::Wow64SetThreadContext(pi.hThread, &wow64Context);
	}
	else
	{
		*ip.pL++ = context.Rip;
		*ip.pL++ = fnLoadLibrary;
		*ip.pB++ = 0x9C;					// pushfq
		*ip.pB++ = 0x50;					// push  rax
		*ip.pB++ = 0x51;					// push  rcx
		*ip.pB++ = 0x52;					// push  rdx
		*ip.pB++ = 0x53;					// push  rbx
		*ip.pB++ = 0x55;					// push  rbp
		*ip.pB++ = 0x56;					// push  rsi
		*ip.pB++ = 0x57;					// push  rdi
		*ip.pB++ = 0x41; *ip.pB++ = 0x50;	// push  r8
		*ip.pB++ = 0x41; *ip.pB++ = 0x51;	// push  r9
		*ip.pB++ = 0x41; *ip.pB++ = 0x52;	// push  r10
		*ip.pB++ = 0x41; *ip.pB++ = 0x53;	// push  r11
		*ip.pB++ = 0x41; *ip.pB++ = 0x54;	// push  r12
		*ip.pB++ = 0x41; *ip.pB++ = 0x55;	// push  r13
		*ip.pB++ = 0x41; *ip.pB++ = 0x56;	// push  r14
		*ip.pB++ = 0x41; *ip.pB++ = 0x57;	// push  r15
		*ip.pB++ = 0x48;					// sub   rsp, 40
		*ip.pB++ = 0x83;
		*ip.pB++ = 0xEC;
		*ip.pB++ = 0x28;

		*ip.pB++ = 0x48;					// lea	 ecx, "path\to\our.dll"
		*ip.pB++ = 0x8D;
		*ip.pB++ = 0x0D;
		*ip.pI++ = 40;

		*ip.pB++ = 0xFF;					// call  LoadLibraryW
		*ip.pB++ = 0x15;
		*ip.pI++ = -49;
		
		*ip.pB++ = 0x48;					// add   rsp, 40
		*ip.pB++ = 0x83;
		*ip.pB++ = 0xC4;
		*ip.pB++ = 0x28;

		*ip.pB++ = 0x41; *ip.pB++ = 0x5F;	// pop   r15
		*ip.pB++ = 0x41; *ip.pB++ = 0x5E;	// pop   r14
		*ip.pB++ = 0x41; *ip.pB++ = 0x5D;	// pop   r13
		*ip.pB++ = 0x41; *ip.pB++ = 0x5C;	// pop   r12
		*ip.pB++ = 0x41; *ip.pB++ = 0x5B;	// pop   r11
		*ip.pB++ = 0x41; *ip.pB++ = 0x5A;	// pop   r10
		*ip.pB++ = 0x41; *ip.pB++ = 0x59;	// pop   r9
		*ip.pB++ = 0x41; *ip.pB++ = 0x58;	// pop   r8
		*ip.pB++ = 0x5F;					// pop	 rdi
		*ip.pB++ = 0x5E;					// pop	 rsi
		*ip.pB++ = 0x5D;					// pop	 rbp
		*ip.pB++ = 0x5B;					// pop	 rbx
		*ip.pB++ = 0x5A;					// pop	 rdx
		*ip.pB++ = 0x59;					// pop	 rcx
		*ip.pB++ = 0x58;					// pop	 rax
		*ip.pB++ = 0x9D;					// popfq
		*ip.pB++ = 0xff;					// jmp	 Rip
		*ip.pB++ = 0x25;
		*ip.pI++ = -91;

		::WriteProcessMemory(pi.hProcess, mem, code.get(), memLen, NULL);
		::FlushInstructionCache(pi.hProcess, mem, memLen);
		context.Rip = reinterpret_cast<UINT_PTR>(mem) + 16;
		::SetThreadContext(pi.hThread, &context);
	}

#else

	*ip.pB++ = 0x68;			// push  eip
	*ip.pI++ = context.Eip;
	*ip.pB++ = 0x9c;			// pushf
	*ip.pB++ = 0x60;			// pusha
	*ip.pB++ = 0x68;			// push  "path\to\our.dll"
	*ip.pI++ = reinterpret_cast<UINT_PTR>(mem) + codeSize;
	*ip.pB++ = 0xe8;			// call  LoadLibraryW
	*ip.pI++ = fnLoadLibrary - (reinterpret_cast<UINT_PTR>(mem) + (ip.pB+4 - code.get()));
	*ip.pB++ = 0x61;			// popa
	*ip.pB++ = 0x9d;			// popf
	*ip.pB++ = 0xc3;			// ret

	::WriteProcessMemory(pi.hProcess, mem, code.get(), memLen, NULL);
	::FlushInstructionCache(pi.hProcess, mem, memLen);
	context.Eip = reinterpret_cast<UINT_PTR>(mem);
	::SetThreadContext(pi.hThread, &context);
#endif

	return true;
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

DWORD WINAPI ConsoleHandler::MonitorThreadStatic(LPVOID lpParameter)
{
	ConsoleHandler* pConsoleHandler = reinterpret_cast<ConsoleHandler*>(lpParameter);
	return pConsoleHandler->MonitorThread();
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

DWORD ConsoleHandler::MonitorThread()
{
	// resume ConsoleHook's thread
	m_consoleParams.SetRespEvent();

	HANDLE arrWaitHandles[] = { m_hConsoleProcess.get(), m_hMonitorThreadExit.get(), m_consoleBuffer.GetReqEvent() };
	while (::WaitForMultipleObjects(sizeof(arrWaitHandles)/sizeof(arrWaitHandles[0]), arrWaitHandles, FALSE, INFINITE) > WAIT_OBJECT_0 + 1)
	{
		DWORD				dwColumns	= m_consoleInfo->csbi.srWindow.Right - m_consoleInfo->csbi.srWindow.Left + 1;
		DWORD				dwRows		= m_consoleInfo->csbi.srWindow.Bottom - m_consoleInfo->csbi.srWindow.Top + 1;
		DWORD				dwBufferColumns	= m_consoleInfo->csbi.dwSize.X;
		DWORD				dwBufferRows	= m_consoleInfo->csbi.dwSize.Y;
		bool				bResize		= false;

		if ((m_consoleParams->dwColumns != dwColumns) ||
			(m_consoleParams->dwRows != dwRows) ||
			((m_consoleParams->dwBufferColumns != 0) && (m_consoleParams->dwBufferColumns != dwBufferColumns)) ||
			((m_consoleParams->dwBufferRows != 0) && (m_consoleParams->dwBufferRows != dwBufferRows)))
		{
			MutexLock handlerLock(m_bufferMutex);

			m_consoleParams->dwColumns	= dwColumns;
			m_consoleParams->dwRows		= dwRows;

			// TODO: improve this
			// this will handle console applications that change console buffer 
			// size (like Far manager).
			// This is not a perfect solution, but it's the best one I have
			// for now
			if (m_consoleParams->dwBufferColumns != 0)	m_consoleParams->dwBufferColumns= dwBufferColumns;
			if (m_consoleParams->dwBufferRows != 0)		m_consoleParams->dwBufferRows	= dwBufferRows;
			bResize = true;
		}

		m_consoleChangeDelegate(bResize);
	}

	TRACE(L"exiting thread\n");
	// exiting thread
	m_consoleCloseDelegate();
	return 0;
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void ConsoleHandler::PostMessage(UINT Msg, WPARAM wParam, LPARAM lParam)
{
#ifdef _DEBUG
	const wchar_t * strMsg = L"???";
	switch( Msg )
	{
	case WM_INPUTLANGCHANGEREQUEST: strMsg = L"WM_INPUTLANGCHANGEREQUEST"; break;
	case WM_INPUTLANGCHANGE:        strMsg = L"WM_INPUTLANGCHANGE";        break;
	case WM_KEYDOWN :               strMsg = L"WM_KEYDOWN";                break;
	};

	TRACE(
		L"PostMessage Msg = 0x%08lx (%s) WPARAM = %p LPARAM = %p\n",
		Msg, strMsg,
		wParam, lParam);
#endif

	NamedPipeMessage npmsg;
	npmsg.type = NamedPipeMessage::POSTMESSAGE;
	npmsg.data.winmsg.msg = Msg;
	npmsg.data.winmsg.wparam = static_cast<DWORD>(wParam);
	npmsg.data.winmsg.lparam = static_cast<DWORD>(lParam);

	try
	{
		m_consoleMsgPipe.Write(&npmsg, sizeof(npmsg));
	}
#ifdef _DEBUG
	catch(std::exception& e)
	{
		TRACE(
			L"PostMessage(pipe) Msg = 0x%08lx (%s) WPARAM = %p LPARAM = %p fails (reason: %S)\n",
			Msg, strMsg,
			wParam, lParam,
			e.what());
	}
#else
	catch(std::exception&) { }
#endif
}

void ConsoleHandler::WriteConsoleInput(KEY_EVENT_RECORD* pkeyEvent)
{
	NamedPipeMessage npmsg;
	npmsg.type = NamedPipeMessage::WRITECONSOLEINPUT;
	npmsg.data.keyEvent = *pkeyEvent;

	try
	{
		m_consoleMsgPipe.Write(&npmsg, sizeof(npmsg));
	}
#ifdef _DEBUG
	catch(std::exception& e)
	{
		TRACE(
			L"WriteConsoleInput(pipe) fails (reason: %S)\n",
			L"  bKeyDown          = %s\n"
			L"  dwControlKeyState = 0x%08lx\n"
			L"  UnicodeChar       = 0x%04hx\n"
			L"  wRepeatCount      = %hu\n"
			L"  wVirtualKeyCode   = 0x%04hx\n"
			L"  wVirtualScanCode  = 0x%04hx\n",
			e.what(),
			npmsg.data.keyEvent.bKeyDown?"TRUE":"FALSE",
			npmsg.data.keyEvent.dwControlKeyState,
			npmsg.data.keyEvent.uChar.UnicodeChar,
			npmsg.data.keyEvent.wRepeatCount,
			npmsg.data.keyEvent.wVirtualKeyCode,
			npmsg.data.keyEvent.wVirtualScanCode);
	}
#else
	catch(std::exception&) { }
#endif


}

void ConsoleHandler::SendMessage(UINT Msg, WPARAM wParam, LPARAM lParam)
{
#ifdef _DEBUG
	const wchar_t * strMsg = L"???";
	switch( Msg )
	{
	case WM_CLOSE:      strMsg = L"WM_CLOSE";      break;
	case WM_KEYDOWN:    strMsg = L"WM_KEYDOWN";    break;
	case WM_KEYUP:      strMsg = L"WM_KEYUP";      break;
	case WM_SYSCOMMAND: strMsg = L"WM_SYSCOMMAND"; break;
	}

	TRACE(
		L"SendMessage Msg = 0x%08lx (%s) WPARAM = %p LPARAM = %p\n",
		Msg, strMsg,
		wParam, lParam);
#endif

	if( (Msg >= WM_KEYFIRST && Msg <= WM_KEYLAST) || Msg == WM_CLOSE || Msg == WM_SYSCOMMAND )
	{
		NamedPipeMessage npmsg;
		npmsg.type = NamedPipeMessage::SENDMESSAGE;
		npmsg.data.winmsg.msg = Msg;
		npmsg.data.winmsg.wparam = static_cast<DWORD>(wParam);
		npmsg.data.winmsg.lparam = static_cast<DWORD>(lParam);

		try
		{
			m_consoleMsgPipe.Write(&npmsg, sizeof(npmsg));
		}
#ifdef _DEBUG
		catch(std::exception& e)
		{
			TRACE(
				L"SendMessage(pipe) Msg = 0x%08lx (%s) WPARAM = %p LPARAM = %p fails (reason: %S)\n",
				Msg, strMsg,
				wParam, lParam,
				e.what());
		}
#else
		catch(std::exception&) { }
#endif
	}
	else
	{
#ifdef _DEBUG
		LRESULT res = ::SendMessage(m_consoleParams->hwndConsoleWindow, Msg, wParam, lParam);
		TRACE(
			L"SendMessage Msg = 0x%08lx (%s) WPARAM = %p LPARAM = %p returns %p (last error 0x%08lx)\n",
			Msg, strMsg,
			wParam, lParam,
			res,
			GetLastError());
#else
		::SendMessage(m_consoleParams->hwndConsoleWindow, Msg, wParam, lParam);
#endif
	}
}

void ConsoleHandler::SetWindowPos(int X, int Y, int cx, int cy, UINT uFlags)
{
	NamedPipeMessage npmsg;
	npmsg.type = NamedPipeMessage::SETWINDOWPOS;
	npmsg.data.windowpos.X               = X;
	npmsg.data.windowpos.Y               = Y;
	npmsg.data.windowpos.cx              = cx;
	npmsg.data.windowpos.cy              = cy;
	npmsg.data.windowpos.uFlags          = uFlags;

	try
	{
		m_consoleMsgPipe.Write(&npmsg, sizeof(npmsg));
	}
#ifdef _DEBUG
	catch(std::exception& e)
	{
		TRACE(
			L"SetWindowPos(pipe) X = %d Y = %d cx = %d cy = %d uFlags = 0x%08lx fails (reason: %S)\n",
			X, Y,
			cx, cy,
			uFlags,
			e.what());
	}
#else
	catch(std::exception&) { }
#endif
}

void ConsoleHandler::ShowWindow(int nCmdShow)
{
	NamedPipeMessage npmsg;
	npmsg.type = NamedPipeMessage::SHOWWINDOW;
	npmsg.data.show.nCmdShow = nCmdShow;

	try
	{
		m_consoleMsgPipe.Write(&npmsg, sizeof(npmsg));
	}
#ifdef _DEBUG
	catch(std::exception& e)
	{
		TRACE(
			L"ShowWindow(pipe) nCmdShow = 0x%08lx fails (reason: %S)\n",
			nCmdShow,
			e.what());
	}
#else
	catch(std::exception&) { }
#endif
}

void ConsoleHandler::SendTextToConsole(const wchar_t* pszText)
{
	if (pszText == NULL) return;

	size_t textLen = wcslen(pszText);

	if (textLen == 0) return;

	NamedPipeMessage npmsg;
	npmsg.type = NamedPipeMessage::SENDTEXT;
	npmsg.data.text.dwTextLen = static_cast<DWORD>(textLen);

	try
	{
		m_consoleMsgPipe.Write(&npmsg, sizeof(npmsg));
		m_consoleMsgPipe.Write(pszText, textLen * sizeof(wchar_t));
	}
#ifdef _DEBUG
	catch(std::exception& e)
	{
		TRACE(
			L"SendTextToConsole(pipe) %s fails (reason: %S)\n",
			pszText,
			e.what());
	}
#else
	catch(std::exception&) { }
#endif
}

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

wstring ConsoleHandler::GetModulePath(HMODULE hModule)
{
	wchar_t szModulePath[MAX_PATH+1];
	::ZeroMemory(szModulePath, (MAX_PATH+1)*sizeof(wchar_t));

	::GetModuleFileName(hModule, szModulePath, MAX_PATH);

	wstring strPath(szModulePath);

	return strPath.substr(0, strPath.rfind(L'\\'));
}

//////////////////////////////////////////////////////////////////////////////

