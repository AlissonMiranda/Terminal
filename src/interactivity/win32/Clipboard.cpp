/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "clipboard.hpp"

#include "..\..\host\dbcs.h"
#include "..\..\host\scrolling.hpp"
#include "..\..\host\output.h"

#include "..\..\types\inc\convert.hpp"
#include "..\..\types\inc\viewport.hpp"

#include "..\inc\ServiceLocator.hpp"

#pragma hdrstop

using namespace Microsoft::Console::Interactivity::Win32;
using namespace Microsoft::Console::Types;

#pragma region Public Methods

void Clipboard::Copy()
{
    StoreSelectionToClipboard();   // store selection in clipboard
    Selection::Instance().ClearSelection();   // clear selection in console
}

/*++

Perform paste request into old app by pulling out clipboard
contents and writing them to the console's input buffer

--*/
void Clipboard::Paste()
{
    HANDLE ClipboardDataHandle;

    // Clear any selection or scrolling that may be active.
    Selection::Instance().ClearSelection();
    Scrolling::s_ClearScroll();

    // Get paste data from clipboard
    if (!OpenClipboard(ServiceLocator::LocateConsoleWindow()->GetWindowHandle()))
    {
        return;
    }

    ClipboardDataHandle = GetClipboardData(CF_UNICODETEXT);
    if (ClipboardDataHandle == nullptr)
    {
        CloseClipboard();
        return;
    }

    PWCHAR pwstr = (PWCHAR)GlobalLock(ClipboardDataHandle);
    StringPaste(pwstr, (ULONG)GlobalSize(ClipboardDataHandle) / sizeof(WCHAR));
    GlobalUnlock(ClipboardDataHandle);

    CloseClipboard();
}

Clipboard& Clipboard::Instance()
{
    static Clipboard clipboard;
    return clipboard;
}

// Routine Description:
// - This routine pastes given Unicode string into the console window.
// Arguments:
// - pData - Unicode string that is pasted to the console window
// - cchData - Size of the Unicode String in characters
// Return Value:
// - None
void Clipboard::StringPaste(_In_reads_(cchData) const wchar_t* const pData,
                            const size_t cchData)
{
    if (pData == nullptr)
    {
        return;
    }

    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    try
    {
        std::deque<std::unique_ptr<IInputEvent>> inEvents = TextToKeyEvents(pData, cchData);
        gci.pInputBuffer->Write(inEvents);
    }
    catch (...)
    {
        LOG_HR(wil::ResultFromCaughtException());
    }
}

#pragma endregion

#pragma region Private Methods

// Routine Description:
// - converts a wchar_t* into a series of KeyEvents as if it was typed
// from the keyboard
// Arguments:
// - pData - the text to convert
// - cchData - the size of pData, in wchars
// Return Value:
// - deque of KeyEvents that represent the string passed in
// Note:
// - will throw exception on error
std::deque<std::unique_ptr<IInputEvent>> Clipboard::TextToKeyEvents(_In_reads_(cchData) const wchar_t* const pData,
                                                                    const size_t cchData)
{
    THROW_IF_NULL_ALLOC(pData);

    std::deque<std::unique_ptr<IInputEvent>> keyEvents;

    for (size_t i = 0; i < cchData; ++i)
    {
        wchar_t currentChar = pData[i];

        const bool charAllowed = FilterCharacterOnPaste(&currentChar);
        // filter out linefeed if it's not the first char and preceded
        // by a carriage return
        const bool skipLinefeed = (i != 0  &&
                                   currentChar == UNICODE_LINEFEED &&
                                   pData[i -1] == UNICODE_CARRIAGERETURN);

        if (!charAllowed || skipLinefeed)
        {
            continue;
        }

        if (currentChar == 0)
        {
            break;
        }

        // MSFT:12123975 / WSL GH#2006
        // If you paste text with ONLY linefeed line endings (unix style) in wsl,
        //      then we faithfully pass those along, which the underlying terminal
        //      interprets as C-j. In nano, C-j is mapped to "Justify text", which
        //      causes the pasted text to get broken at the width of the terminal.
        // This behavior doesn't occur in gnome-terminal, and nothing like it occurs
        //      in vi or emacs.
        // This change doesn't break pasting text into any of those applications
        //      with CR/LF (Windows) line endings either. That apparently always
        //      worked right.
        if (IsInVirtualTerminalInputMode() && currentChar == UNICODE_LINEFEED)
        {
            currentChar = UNICODE_CARRIAGERETURN;
        }

        const UINT codepage = ServiceLocator::LocateGlobals().getConsoleInformation().OutputCP;
        std::deque<std::unique_ptr<KeyEvent>> convertedEvents = CharToKeyEvents(currentChar, codepage);
        while (!convertedEvents.empty())
        {
            keyEvents.push_back(std::move(convertedEvents.front()));
            convertedEvents.pop_front();
        }
    }
    return keyEvents;
}

// Routine Description:
// - Copies the selected area onto the global system clipboard.
// Arguments:
//  <none>
// Return Value:
//  <none>
void Clipboard::StoreSelectionToClipboard()
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    Selection* pSelection = &Selection::Instance();

    // See if there is a selection to get
    if (!pSelection->IsAreaSelected())
    {
        return;
    }

    // read selection area.
    const SCREEN_INFORMATION& screenInfo = gci.GetActiveOutputBuffer();

    SMALL_RECT* rgsrSelection;
    UINT cRectsSelected;

    NTSTATUS status = pSelection->GetSelectionRects(&rgsrSelection, &cRectsSelected);

    if (NT_SUCCESS(status))
    {
        PWCHAR* rgTempRows = new PWCHAR[cRectsSelected];
        status = NT_TESTNULL(rgTempRows);

        if (NT_SUCCESS(status))
        {
            size_t* rgTempRowLengths = new size_t[cRectsSelected];
            status = NT_TESTNULL(rgTempRowLengths);

            if (NT_SUCCESS(status))
            {
                const bool fLineSelection = Selection::Instance().IsLineSelection();

                status = RetrieveTextFromBuffer(screenInfo,
                                                fLineSelection,
                                                cRectsSelected,
                                                rgsrSelection,
                                                rgTempRows,
                                                rgTempRowLengths);

                if (NT_SUCCESS(status))
                {
                    status = CopyTextToSystemClipboard(cRectsSelected, rgTempRows, rgTempRowLengths);

                    for (UINT iRow = 0; iRow < cRectsSelected; iRow++)
                    {
                        if (rgTempRows[iRow] != nullptr)
                        {
                            delete rgTempRows[iRow];
                        }
                    }
                }

                delete[] rgTempRowLengths;
            }

            delete[] rgTempRows;
        }

        delete[] rgsrSelection;
    }
}

[[nodiscard]]
NTSTATUS Clipboard::RetrieveTextFromBuffer(const SCREEN_INFORMATION& screenInfo,
                                           const bool fLineSelection,
                                           const UINT cRectsSelected,
                                           _In_reads_(cRectsSelected) const SMALL_RECT* const rgsrSelection,
                                           _Out_writes_(cRectsSelected) PWCHAR* const rgpwszTempText,
                                           _Out_writes_(cRectsSelected) size_t* const rgTempTextLengths)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (rgpwszTempText == nullptr)
    {
        status = STATUS_INVALID_PARAMETER_5;
    }
    else if (rgTempTextLengths == nullptr)
    {
        status = STATUS_INVALID_PARAMETER_6;
    }

    if (NT_SUCCESS(status))
    {
        UINT iRect = 0;

        // for each row in the selection
        for (iRect = 0; iRect < cRectsSelected; iRect++)
        {
            const SMALL_RECT srHighlightRow = rgsrSelection[iRect];

            const SMALL_RECT srSelection = Selection::Instance().GetSelectionRectangle();

            const UINT iRow = srSelection.Top + iRect;

            // recalculate string length again as the width of the highlight row might have been reduced in the bisect call
            const short sStringLength = srHighlightRow.Right - srHighlightRow.Left + 1;

            // this is the source location X/Y coordinates within the active screen buffer to start copying from
            const COORD coordSourcePoint{ srHighlightRow.Left, srHighlightRow.Top };

            // our output buffer is 1 dimensional and is just as long as the string, so the "rectangle" should
            // specify just a line.
            // length of 80 runs from left 0 to right 79. therefore -1.
            const SMALL_RECT srTargetRect{ 0, 0, sStringLength - 1, 0 };

            // retrieve the data from the screen buffer
            std::vector<OutputCell> outputCells;
            std::wstring selectionText;
            try
            {
                std::vector<std::vector<OutputCell>> cells = ReadRectFromScreenBuffer(screenInfo,
                                                                                      coordSourcePoint,
                                                                                      Viewport::FromInclusive(srTargetRect));
                // we only care about one row so reduce it here
                outputCells = cells.at(0);
                // allocate a string buffer
                selectionText.reserve(outputCells.size() + 2); // + 2 for \r\n if we munged it
            }
            catch (...)
            {
                return NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
            }


            // copy char data into the string buffer, skipping trailing bytes
            for (auto& cell : outputCells)
            {
                if (!cell.DbcsAttr().IsTrailing())
                {
                    for (const wchar_t wch : cell.Chars())
                    {
                        selectionText.push_back(wch);
                    }
                }
            }

            // trim trailing spaces
            const bool mungeData = (GetKeyState(VK_SHIFT) & KEY_PRESSED) == 0;
            if (mungeData)
            {
                const ROW& Row = screenInfo.GetTextBuffer().GetRowByOffset(iRow);

                // FOR LINE SELECTION ONLY: if the row was wrapped, don't remove the spaces at the end.
                if (!fLineSelection || !Row.GetCharRow().WasWrapForced())
                {
                    while (!selectionText.empty() && selectionText.back() == UNICODE_SPACE)
                    {
                        selectionText.pop_back();
                    }
                }

                // apply CR/LF to the end of the final string, unless we're the last line.
                // a.k.a if we're earlier than the bottom, then apply CR/LF.
                if (iRect < cRectsSelected - 1)
                {
                    // FOR LINE SELECTION ONLY: if the row was wrapped, do not apply CR/LF.
                    // a.k.a. if the row was NOT wrapped, then we can assume a CR/LF is proper
                    // always apply \r\n for box selection
                    if (!fLineSelection || !screenInfo.GetTextBuffer().GetRowByOffset(iRow).GetCharRow().WasWrapForced())
                    {
                        selectionText.push_back(UNICODE_CARRIAGERETURN);
                        selectionText.push_back(UNICODE_LINEFEED);
                    }
                }
            }

            // save the formatted string and its length for later
            // +1 for null character termination
            std::unique_ptr<wchar_t[]> wstrBuffer = std::make_unique<wchar_t[]>(selectionText.size() + 1);
            if (!wstrBuffer.get())
            {
                return STATUS_NO_MEMORY;
            }
            std::copy(selectionText.begin(), selectionText.end(), wstrBuffer.get());
            wstrBuffer[selectionText.size()] = UNICODE_NULL;
            rgpwszTempText[iRect] = wstrBuffer.release();
            rgTempTextLengths[iRect] = selectionText.size();


        }

        if (!NT_SUCCESS(status))
        {
            for (UINT iDelRect = 0; iDelRect < iRect; iDelRect++)
            {
                if (rgpwszTempText[iDelRect] != nullptr)
                {
                    delete[] rgpwszTempText[iDelRect];
                }
            }
        }
    }

    return status;
}


// Routine Description:
// - Copies the text given onto the global system clipboard.
// Arguments:
// - cTotalRows
// Return Value:
//  <none>
[[nodiscard]]
NTSTATUS Clipboard::CopyTextToSystemClipboard(const UINT cTotalRows,
                                              _In_reads_(cTotalRows) const PWCHAR* const rgTempRows,
                                              _In_reads_(cTotalRows) const size_t* const rgTempRowLengths)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (rgTempRows == nullptr)
    {
        status = STATUS_INVALID_PARAMETER_2;
    }
    else if (rgTempRowLengths == nullptr)
    {
        status = STATUS_INVALID_PARAMETER_3;
    }

    if (NT_SUCCESS(status))
    {

        // calculate number of characters in the rows we created
        size_t cRowCharCount = 0;

        for (UINT iRow = 0; iRow < cTotalRows; iRow++)
        {
            size_t cLength;

            if (FAILED(StringCchLengthW(rgTempRows[iRow], rgTempRowLengths[iRow] + 1, &cLength)))
            {
                status = STATUS_UNSUCCESSFUL;
                break;
            }

            cRowCharCount += cLength;
        }

        if (NT_SUCCESS(status))
        {
            // +1 for null terminator at end of clipboard data
            cRowCharCount++;

            ASSERT(cRowCharCount > 0);

            // allocate the final clipboard data
            HANDLE hClipboardDataHandle = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, cRowCharCount * sizeof(WCHAR));

            if (hClipboardDataHandle == nullptr)
            {
                status = NTSTATUS_FROM_WIN32(GetLastError());

                // if for some reason Get Last Error returned successful, set generic memory error.
                if (NT_SUCCESS(status))
                {
                    status = STATUS_NO_MEMORY;
                }
            }

            if (NT_SUCCESS(status))
            {
                PWCHAR pwszClipboardPos = (PWCHAR)GlobalLock(hClipboardDataHandle);

                if (pwszClipboardPos == nullptr)
                {
                    status = NTSTATUS_FROM_WIN32(GetLastError());

                    // if Get Last Error was successful for some reason, set generic memory error
                    if (NT_SUCCESS(status))
                    {
                        status = STATUS_NO_MEMORY;
                    }
                }

                if (NT_SUCCESS(status))
                {
                    // copy all text into the final clipboard data handle. There should be no nulls between rows of
                    // characters, but there should be a \0 at the end.
                    for (UINT iRow = 0; iRow < cTotalRows; iRow++)
                    {
                        if (FAILED(StringCchCopyW(pwszClipboardPos, rgTempRowLengths[iRow] + 1, rgTempRows[iRow])))
                        {
                            status = STATUS_UNSUCCESSFUL;
                            break;
                        }

                        pwszClipboardPos += rgTempRowLengths[iRow];
                    }

                    *pwszClipboardPos = UNICODE_NULL;

                    GlobalUnlock(hClipboardDataHandle);

                    if (NT_SUCCESS(status))
                    {
                        BOOL fSuccess = OpenClipboard(ServiceLocator::LocateConsoleWindow()->GetWindowHandle());
                        if (!fSuccess)
                        {
                            status = NTSTATUS_FROM_WIN32(GetLastError());
                        }

                        if (NT_SUCCESS(status))
                        {
                            fSuccess = EmptyClipboard();
                            if (!fSuccess)
                            {
                                status = NTSTATUS_FROM_WIN32(GetLastError());
                            }

                            if (NT_SUCCESS(status))
                            {
                                if (SetClipboardData(CF_UNICODETEXT, hClipboardDataHandle) == nullptr)
                                {
                                    status = NTSTATUS_FROM_WIN32(GetLastError());
                                }
                            }

                            if (!CloseClipboard() && NT_SUCCESS(status))
                            {
                                status = NTSTATUS_FROM_WIN32(GetLastError());
                            }
                        }
                    }
                }

                if (!NT_SUCCESS(status))
                {
                    // only free if we failed.
                    // the memory has to remain allocated if we successfully placed it on the clipboard.
                    GlobalFree(hClipboardDataHandle);
                }
            }
        }
    }

    return status;
}

// Returns true if the character should be emitted to the paste stream
// -- in some cases, we will change what character should be emitted, as in the case of "smart quotes"
// Returns false if the character should not be emitted (e.g. <TAB>)
bool Clipboard::FilterCharacterOnPaste(_Inout_ WCHAR * const pwch)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    bool fAllowChar = true;
    if (gci.GetFilterOnPaste() &&
        (IsFlagSet(gci.pInputBuffer->InputMode, ENABLE_PROCESSED_INPUT)))
    {
        switch (*pwch)
        {
            // swallow tabs to prevent inadvertant tab expansion
            case UNICODE_TAB:
            {
                fAllowChar = false;
                break;
            }

            // Replace Unicode space with standard space
            case UNICODE_NBSP:
            case UNICODE_NARROW_NBSP:
            {
                *pwch = UNICODE_SPACE;
                break;
            }

            // Replace "smart quotes" with "dumb ones"
            case UNICODE_LEFT_SMARTQUOTE:
            case UNICODE_RIGHT_SMARTQUOTE:
            {
                *pwch = UNICODE_QUOTE;
                break;
            }

            // Replace Unicode dashes with a standard hypen
            case UNICODE_EM_DASH:
            case UNICODE_EN_DASH:
            {
                *pwch = UNICODE_HYPHEN;
                break;
            }
        }
    }

    return fAllowChar;
}

#pragma endregion
