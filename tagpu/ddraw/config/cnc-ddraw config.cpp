//---------------------------------------------------------------------------

#include <vcl.h>
#pragma hdrstop
#include <IniFiles.hpp>
#include <IOUtils.hpp>
#include <System.Hash.hpp>
#include <tchar.h>
//---------------------------------------------------------------------------
#include <Vcl.Styles.hpp>
#include <Vcl.Themes.hpp>
USEFORM("ConfigFormUnit.cpp", ConfigForm);
//---------------------------------------------------------------------------
#define GAME_PATH (TPath::GetDirectoryName(Application->ExeName) + "\\")

int WINAPI _tWinMain(HINSTANCE, HINSTANCE, LPTSTR, int)
{
	try
	{
		Application->Initialize();
		Application->MainFormOnTaskBar = true;

		HWND hwnd =
			FindWindow(
				THashSHA1::GetHashString(Application->ExeName).w_str(), NULL);

		if (hwnd && ParamStr(1) != L"-restart") {

			if (IsIconic(hwnd)) {
				ShowWindow(hwnd, SW_RESTORE);
			}

			SetForegroundWindow(hwnd);
			return 0;
		}

		auto iniPath = System::Sysutils::GetEnvironmentVariable(
			"CNC_DDRAW_CONFIG_FILE");

		auto *ini = 
			new TIniFile(iniPath.Length() ? iniPath : GAME_PATH + "ddraw.ini");
			
		auto theme = ini->ReadString("ddraw", "configtheme", "Windows10");

		TStyleManager::TrySetStyle(
			theme == "Cobalt XEMedia" ? "Cobalt XEMedia" : "Windows10");

		delete ini;

		Application->CreateForm(__classid(TConfigForm), &ConfigForm);
		Application->Run();
	}
	catch (Exception &exception)
	{
		Application->ShowException(&exception);
	}
	catch (...)
	{
		try
		{
			throw Exception("");
		}
		catch (Exception &exception)
		{
			Application->ShowException(&exception);
		}
	}
	return 0;
}

// dummy functions to avoid Wtsapi32.dll imports
EXTERN_C BOOL WINAPI WTSRegisterSessionNotification(HWND hWnd, DWORD dwFlags)
{
	return TRUE;
}

EXTERN_C BOOL WINAPI WTSUnRegisterSessionNotification(HWND hWnd)
{
	return TRUE;
}
//---------------------------------------------------------------------------
