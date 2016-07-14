// OpenConsoleExe.cpp : Defines the entry point for the application.
//

#include "stdafx.h"

#include "..\ServerBaseApi\Entrypoints.h"
// #include "..\ServerSample\ApiResponderEmpty.h"
// #include "..\ServerApp\ConsoleHost.h"
#include "..\ServerApp\DriverResponder.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR    lpCmdLine,
                      _In_ int       nCmdShow)
{
    DriverResponder* const pResponder = new DriverResponder();
    return Entrypoints::StartConsoleForCmdLine(lpCmdLine, pResponder);
}