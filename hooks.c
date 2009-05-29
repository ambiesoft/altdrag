/*
	AltDrag - Drag windows with the mouse when pressing the alt key
	Copyright (C) 2009  Stefan Sundin (recover89@gmail.com)
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#define UNICODE
#define _UNICODE

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define _WIN32_WINNT 0x0500
#include <windows.h>
#include <shlwapi.h>

//App
#define APP_NAME L"AltDrag"
#define STICKY_THRESHOLD 20

//Some variables must be shared so the CallWndProc hooks can access them
#define shareattr __attribute__((section ("shared"), shared))

int alt=0;
int shift shareattr=0;
int move shareattr=0;
int resize shareattr=0;
HWND hwnd=NULL;
unsigned int clicktime=0;
POINT offset;
POINT resize_offset;
enum {TOP, RIGHT, BOTTOM, LEFT, CENTER} resize_x, resize_y;
HWND *wnds=NULL;
int numwnds=0;
struct {
	int Cursor;
	int AutoStick;
	int RMBMinimize;
} sharedsettings shareattr={0,0,0};
int sharedsettings_loaded shareattr=0;
wchar_t inipath[MAX_PATH] shareattr;
wchar_t txt[1000] shareattr;

//Blacklist
struct blacklistitem {
	wchar_t *title;
	wchar_t *classname;
};
struct blacklist {
	struct blacklistitem *items;
	int numitems;
};
struct {
	struct blacklist Blacklist;
	struct blacklist Blacklist_Sticky;
} settings={{NULL,0},{NULL,0}};

//Roll-up data
#define NUMROLLUP 8
struct rollupdata {
	HWND hwnd;
	int width;
	int height;
};
struct rollupdata rollup[NUMROLLUP];
int rolluppos=0;

//Cursor data
HWND cursorwnd shareattr=NULL;
HCURSOR cursor[6] shareattr;
enum {HAND, SIZENWSE, SIZENESW, SIZENS, SIZEWE, SIZEALL} resizecursor;

//Mousehook data
HINSTANCE hinstDLL=NULL;
HHOOK mousehook=NULL;

//Msghook data
WNDPROC oldwndproc=NULL;
enum {MOVE, RESIZE, NONE} msgaction=NONE;

//Error message handling
LRESULT CALLBACK ErrorMsgProc(INT nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HCBT_ACTIVATE) {
		//Edit the caption of the buttons
		SetDlgItemText((HWND)wParam,IDYES,L"Copy error");
		SetDlgItemText((HWND)wParam,IDNO,L"OK");
	}
	return 0;
}

void Error(wchar_t *func, wchar_t *info, int errorcode, int line) {
	#ifdef DEBUG
	//Format message
	wchar_t errormsg[100];
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,NULL,errorcode,0,errormsg,sizeof(errormsg)/sizeof(wchar_t),NULL);
	errormsg[wcslen(errormsg)-2]='\0'; //Remove that damn newline at the end of the formatted error message
	swprintf(txt,L"%s failed in file %s, line %d.\nError: %s (%d)\n\n%s", func, TEXT(__FILE__), line, errormsg, errorcode, info);
	//Display message
	HHOOK hhk=SetWindowsHookEx(WH_CBT, &ErrorMsgProc, 0, GetCurrentThreadId());
	int response=MessageBox(NULL, txt, APP_NAME L" hook error", MB_ICONERROR|MB_YESNO|MB_DEFBUTTON2);
	UnhookWindowsHookEx(hhk);
	if (response == IDYES) {
		//Copy message to clipboard
		OpenClipboard(NULL);
		EmptyClipboard();
		wchar_t *data=LocalAlloc(LMEM_FIXED,(wcslen(txt)+1)*sizeof(wchar_t));
		memcpy(data,txt,(wcslen(txt)+1)*sizeof(wchar_t));
		SetClipboardData(CF_UNICODETEXT,data);
		CloseClipboard();
	}
	#endif
}

int blacklisted(HWND hwnd, struct blacklist *list) {
	wchar_t title[256];
	wchar_t classname[256];
	GetWindowText(hwnd,title,sizeof(title)/sizeof(wchar_t));
	GetClassName(hwnd,classname,sizeof(classname)/sizeof(wchar_t));
	int i;
	for (i=0; i < list->numitems; i++) {
		if ((list->items[i].title == NULL && !wcscmp(classname,list->items[i].classname))
		 || (list->items[i].classname == NULL && !wcscmp(title,list->items[i].title))
		 || (list->items[i].title != NULL && list->items[i].classname != NULL && !wcscmp(title,list->items[i].title) && !wcscmp(classname,list->items[i].classname))) {
			return 1;
		}
	}
	return 0;
}

int wnds_alloc=0;
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
	//Make sure we have enough space allocated
	if (numwnds == wnds_alloc) {
		wnds_alloc+=20;
		if ((wnds=realloc(wnds,wnds_alloc*sizeof(HWND))) == NULL) {
			Error(L"realloc(wnds)",L"Out of memory?",0,__LINE__);
			return FALSE;
		}
	}
	//Only store window if it's visible, not minimized to taskbar, not maximized, not the window we are dragging and not blacklisted
	if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd)
	 && hwnd != (HWND)lParam && !blacklisted(hwnd,&settings.Blacklist_Sticky)) {
		//Return if the window is in the roll-up database (I want to get used to the roll-ups before deciding if I want this)
		/*for (i=0; i < NUMROLLUP; i++) {
			if (rollup[i].hwnd == hwnd) {
				return TRUE;
			}
		}*/
		//Add window to wnds
		wnds[numwnds++]=hwnd;
	}
	//If this window is maximized we don't want to stick to any windows that might be under it
	if (IsZoomed(hwnd)) {
		return FALSE;
	}
	return TRUE;
}

int MoveStick(int *posx, int *posy, int wndwidth, int wndheight) {
	//Reset wnds
	numwnds=0;
	HWND progman;
	if ((progman=FindWindow(L"Progman",L"Program Manager")) != NULL) {
		wnds[numwnds++]=progman;
	}
	//Populate wnds
	if (shift || sharedsettings.AutoStick >= 2) {
		EnumWindows(EnumWindowsProc,(LPARAM)hwnd);
	}
	else if (sharedsettings.AutoStick == 1) {
		HWND taskbar;
		if ((taskbar=FindWindow(L"Shell_TrayWnd",NULL)) != NULL) {
			wnds[numwnds++]=taskbar;
		}
	}
	
	/*//Use this to print the windows in wnds
	FILE *f=fopen("C:\\altdrag-log.txt","wb");
	fprintf(f,"numwnds: %d\n",numwnds);
	char title[100];
	char classname[100];
	int j;
	for (j=0; j < numwnds; j++) {
		GetWindowTextA(wnds[j],title,100);
		GetClassNameA(wnds[j],classname,100);
		RECT wndsize;
		GetWindowRect(wnds[j],&wndsize);
		fprintf(f,"wnd #%03d: %s [%s] (%dx%d@%dx%d)\n",j,title,classname,wndsize.right-wndsize.left,wndsize.bottom-wndsize.top,wndsize.left,wndsize.top);
	}
	fclose(f);*/
	
	//thresholdx and thresholdy will shrink to make sure the dragged window will stick to the closest windows
	int i, thresholdx, thresholdy, stuckx=0, stucky=0, stickx=0, sticky=0;
	thresholdx=thresholdy=STICKY_THRESHOLD;
	//Loop windows
	for (i=0; i < numwnds; i++) {
		RECT stickywnd;
		if (GetWindowRect(wnds[i],&stickywnd) == 0) {
			//Error(L"GetWindowRect()",L"MoveStick()",GetLastError(),__LINE__);
			continue;
		}
		
		//Decide if this window should stick inside
		int stickinside=(shift || sharedsettings.AutoStick != 2 || wnds[i] == progman);
		
		//Check if posx sticks
		if ((stickywnd.top-thresholdx < *posy && *posy < stickywnd.bottom+thresholdx)
		 || (*posy-thresholdx < stickywnd.top && stickywnd.top < *posy+wndheight+thresholdx)) {
			int stickinsidecond=(stickinside || *posy+wndheight-thresholdx < stickywnd.top || stickywnd.bottom < *posy+thresholdx);
			if (*posx-thresholdx < stickywnd.right && stickywnd.right < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's right edge
				stuckx=1;
				stickx=stickywnd.right;
				thresholdx=stickywnd.right-*posx;
			}
			else if (stickinsidecond && *posx+wndwidth-thresholdx < stickywnd.right && stickywnd.right < *posx+wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's right edge
				stuckx=1;
				stickx=stickywnd.right-wndwidth;
				thresholdx=stickywnd.right-(*posx+wndwidth);
			}
			else if (stickinsidecond && *posx-thresholdx < stickywnd.left && stickywnd.left < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's left edge
				stuckx=1;
				stickx=stickywnd.left;
				thresholdx=stickywnd.left-*posx;
			}
			else if (*posx+wndwidth-thresholdx < stickywnd.left && stickywnd.left < *posx+wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's left edge
				stuckx=1;
				stickx=stickywnd.left-wndwidth;
				thresholdx=stickywnd.left-(*posx+wndwidth);
			}
		}
		
		//Check if posy sticks
		if ((stickywnd.left-thresholdy < *posx && *posx < stickywnd.right+thresholdy)
		 || (*posx-thresholdy < stickywnd.left && stickywnd.left < *posx+wndwidth+thresholdy)) {
			int stickinsidecond=(stickinside || *posx+wndwidth-thresholdy < stickywnd.left || stickywnd.right < *posx+thresholdy);
			if (*posy-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's bottom edge
				stucky=1;
				sticky=stickywnd.bottom;
				thresholdy=stickywnd.bottom-*posy;
			}
			else if (stickinsidecond && *posy+wndheight-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's bottom edge
				stucky=1;
				sticky=stickywnd.bottom-wndheight;
				thresholdy=stickywnd.bottom-(*posy+wndheight);
			}
			else if (stickinsidecond && *posy-thresholdy < stickywnd.top && stickywnd.top < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's top edge
				stucky=1;
				sticky=stickywnd.top;
				thresholdy=stickywnd.top-*posy;
			}
			else if (*posy+wndheight-thresholdy < stickywnd.top && stickywnd.top < *posy+wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's top edge
				stucky=1;
				sticky=stickywnd.top-wndheight;
				thresholdy=stickywnd.top-(*posy+wndheight);
			}
		}
	}
	
	//Update posx and posy
	if (stuckx) {
		*posx=stickx;
	}
	if (stucky) {
		*posy=sticky;
	}
	
	//Return
	return (stuckx || stucky);
}

void MoveWnd() {
	//Check if window still exists
	if (!IsWindow(hwnd)) {
		move=0;
		UnhookMouse();
		return;
	}
	
	//Get window size
	RECT wnd;
	if (GetWindowRect(hwnd,&wnd) == 0) {
		Error(L"GetWindowRect()",L"MoveWnd()",GetLastError(),__LINE__);
	}
	
	//Get new position for window
	POINT pt;
	GetCursorPos(&pt);
	int posx=pt.x-offset.x;
	int posy=pt.y-offset.y;
	int wndwidth=wnd.right-wnd.left;
	int wndheight=wnd.bottom-wnd.top;
	
	//Double check if any of the shift keys are being pressed
	if (shift && !(GetAsyncKeyState(VK_SHIFT)&0x8000)) {
		shift=0;
	}
	
	//Check if window will stick anywhere
	if (shift || sharedsettings.AutoStick) {
		MoveStick(&posx, &posy, wndwidth, wndheight);
	}
	
	//Move
	if (MoveWindow(hwnd,posx,posy,wndwidth,wndheight,TRUE) == 0) {
		Error(L"MoveWindow()",L"MoveWnd()",GetLastError(),__LINE__);
	}
}

int ResizeStick(int *posx, int *posy, int *wndwidth, int *wndheight) {
	//Reset wnds
	numwnds=0;
	HWND progman;
	if ((progman=FindWindow(L"Progman",L"Program Manager")) != NULL) {
		wnds[numwnds++]=progman;
	}
	//Populate wnds
	if (shift || sharedsettings.AutoStick >= 2) {
		EnumWindows(EnumWindowsProc,(LPARAM)hwnd);
	}
	else if (sharedsettings.AutoStick == 1) {
		HWND taskbar;
		if ((taskbar=FindWindow(L"Shell_TrayWnd",NULL)) != NULL) {
			wnds[numwnds++]=taskbar;
		}
	}
	
	//thresholdx and thresholdy will shrink to make sure the dragged window will stick to the closest windows
	int i, thresholdx, thresholdy, stuckleft=0, stucktop=0, stuckright=0, stuckbottom=0, stickleft=0, sticktop=0, stickright=0, stickbottom=0;
	thresholdx=thresholdy=STICKY_THRESHOLD;
	//How should the windows stick?
	int stickinside=(shift || sharedsettings.AutoStick != 2);
	//Loop windows
	for (i=0; i < numwnds; i++) {
		RECT stickywnd;
		if (GetWindowRect(wnds[i],&stickywnd) == 0) {
			//Error(L"GetWindowRect()",L"ResizeStick()",GetLastError(),__LINE__);
			continue;
		}
		
		//Decide if this window should stick inside
		int stickinside=(shift || sharedsettings.AutoStick != 2 || wnds[i] == progman);
		
		//Check if posx sticks
		if ((stickywnd.top-thresholdx < *posy && *posy < stickywnd.bottom+thresholdx)
		 || (*posy-thresholdx < stickywnd.top && stickywnd.top < *posy+*wndheight+thresholdx)) {
			int stickinsidecond=(stickinside || *posy+*wndheight-thresholdx < stickywnd.top || stickywnd.bottom < *posy+thresholdx);
			if (resize_x == LEFT && *posx-thresholdx < stickywnd.right && stickywnd.right < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's right edge
				stuckleft=1;
				stickleft=stickywnd.right;
				thresholdx=stickywnd.right-*posx;
			}
			else if (stickinsidecond && resize_x == RIGHT && *posx+*wndwidth-thresholdx < stickywnd.right && stickywnd.right < *posx+*wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's right edge
				stuckright=1;
				stickright=stickywnd.right;
				thresholdx=stickywnd.right-(*posx+*wndwidth);
			}
			else if (stickinsidecond && resize_x == LEFT && *posx-thresholdx < stickywnd.left && stickywnd.left < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's left edge
				stuckleft=1;
				stickleft=stickywnd.left;
				thresholdx=stickywnd.left-*posx;
			}
			else if (resize_x == RIGHT && *posx+*wndwidth-thresholdx < stickywnd.left && stickywnd.left < *posx+*wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's left edge
				stuckright=1;
				stickright=stickywnd.left;
				thresholdx=stickywnd.left-(*posx+*wndwidth);
			}
		}
		
		//Check if posy sticks
		if ((stickywnd.left-thresholdy < *posx && *posx < stickywnd.right+thresholdy)
		 || (*posx-thresholdy < stickywnd.left && stickywnd.left < *posx+*wndwidth+thresholdy)) {
			int stickinsidecond=(stickinside || *posx+*wndwidth-thresholdy < stickywnd.left || stickywnd.right < *posx+thresholdy);
			if (resize_y == TOP && *posy-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's bottom edge
				stucktop=1;
				sticktop=stickywnd.bottom;
				thresholdy=stickywnd.bottom-*posy;
			}
			else if (stickinsidecond && resize_y == BOTTOM && *posy+*wndheight-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+*wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's bottom edge
				stuckbottom=1;
				stickbottom=stickywnd.bottom;
				thresholdy=stickywnd.bottom-(*posy+*wndheight);
			}
			else if (stickinsidecond && resize_y == TOP && *posy-thresholdy < stickywnd.top && stickywnd.top < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's top edge
				stucktop=1;
				sticktop=stickywnd.top;
				thresholdy=stickywnd.top-*posy;
			}
			else if (resize_y == BOTTOM && *posy+*wndheight-thresholdy < stickywnd.top && stickywnd.top < *posy+*wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's top edge
				stuckbottom=1;
				stickbottom=stickywnd.top;
				thresholdy=stickywnd.top-(*posy+*wndheight);
			}
		}
	}
	
	//Update pos, posy, wndwidth and widthheight
	if (stuckleft) {
		*wndwidth=*wndwidth+*posx-stickleft;
		*posx=stickleft;
	}
	if (stucktop) {
		*wndheight=*wndheight+*posy-sticktop;
		*posy=sticktop;
	}
	if (stuckright) {
		*wndwidth=stickright-*posx;
	}
	if (stuckbottom) {
		*wndheight=stickbottom-*posy;
	}
	
	//Return
	return (stuckleft || stucktop || stuckright || stuckbottom);
}

void ResizeWnd() {
	//Check if window still exists
	if (!IsWindow(hwnd)) {
		resize=0;
		UnhookMouse();
		return;
	}
	
	//Get window size
	RECT wnd;
	if (GetWindowRect(hwnd,&wnd) == 0) {
		Error(L"GetWindowRect()",L"ResizeWnd()",GetLastError(),__LINE__);
	}
	
	//Get new pos and size for window
	POINT pt;
	GetCursorPos(&pt);
	int posx,posy,wndwidth,wndheight;
	if (resize_x == CENTER && resize_y == CENTER) {
		posx=wnd.left-(pt.x-resize_offset.x);
		posy=wnd.top-(pt.y-resize_offset.y);
		wndwidth=wnd.right-wnd.left+2*(pt.x-resize_offset.x);
		wndheight=wnd.bottom-wnd.top+2*(pt.y-resize_offset.y);
		resize_offset.x=pt.x;
		resize_offset.y=pt.y;
	}
	else {
		if (resize_y == TOP) {
			posy=pt.y-resize_offset.y;
			wndheight=wnd.bottom-pt.y+resize_offset.y;
		}
		else if (resize_y == CENTER) {
			posy=wnd.top;
			wndheight=wnd.bottom-wnd.top;
		}
		else if (resize_y == BOTTOM) {
			posy=wnd.top;
			wndheight=pt.y-wnd.top+resize_offset.y;
		}
		if (resize_x == LEFT) {
			posx=pt.x-resize_offset.x;
			wndwidth=wnd.right-pt.x+resize_offset.x;
		}
		else if (resize_x == CENTER) {
			posx=wnd.left;
			wndwidth=wnd.right-wnd.left;
		}
		else if (resize_x == RIGHT) {
			posx=wnd.left;
			wndwidth=pt.x-wnd.left+resize_offset.x;
		}
	}
	
	//Double check if any of the shift keys are being pressed
	if (shift && !(GetAsyncKeyState(VK_SHIFT)&0x8000)) {
		shift=0;
	}
	
	//Check if window will stick anywhere
	if ((shift || sharedsettings.AutoStick) && (resize_x != CENTER || resize_y != CENTER)) {
		ResizeStick(&posx, &posy, &wndwidth, &wndheight);
	}
	
	//Resize
	if (MoveWindow(hwnd,posx,posy,wndwidth,wndheight,TRUE) == 0) {
		Error(L"MoveWindow()",L"ResizeWnd()",GetLastError(),__LINE__);
	}
}

_declspec(dllexport) LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		int vkey=((PKBDLLHOOKSTRUCT)lParam)->vkCode;
		
		if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
			if (!alt && (vkey == VK_LMENU || vkey == VK_RMENU)) {
				alt=1;
				clicktime=0; //Reset double-click time
				//Get window and desktop size and return if the window is fullscreen, if the window is not the desktop itself
				HWND window=GetForegroundWindow();
				HWND progman;
				if ((progman=FindWindow(L"Progman",L"Program Manager")) == NULL || window != progman) {
					RECT win, desk;
					GetWindowRect(window,&win);
					GetWindowRect(GetDesktopWindow(),&desk);
					if (win.left == desk.left && win.top == desk.top && win.right == desk.right && win.bottom == desk.bottom) {
						return CallNextHookEx(NULL, nCode, wParam, lParam);
					}
				}
				//Hook mouse
				HookMouse();
			}
			else if (vkey == VK_LSHIFT || vkey == VK_RSHIFT) {
				if (!shift) {
					shift=1;
					if (move) {
						MoveWnd();
					}
					if (resize) {
						ResizeWnd();
					}
				}
				if (move) {
					//Block keypress to prevent Windows from changing keyboard layout
					return 1;
				}
			}
			else if (move && (vkey == VK_LCONTROL || vkey == VK_RCONTROL)) {
				//This doesn't always work since the menu is activated by the alt keypress (read msdn)
				SetForegroundWindow(hwnd);
			}
		}
		else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
			if ((vkey == VK_LMENU || vkey == VK_RMENU) && !(GetAsyncKeyState(VK_MENU)&0x8000)) {
				alt=0;
				if (!move && !resize) {
					UnhookMouse();
				}
			}
			else if (vkey == VK_LSHIFT || vkey == VK_RSHIFT) {
				shift=0;
				if (move) {
					MoveWnd();
				}
				if (resize) {
					ResizeWnd();
				}
			}
		}
	}
	
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

_declspec(dllexport) LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		if (wParam == WM_LBUTTONDOWN && alt && !move) {
			//Double check if the left alt key is being pressed
			if (!(GetAsyncKeyState(VK_MENU)&0x8000)) {
				alt=0;
				UnhookMouse();
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Alt key is still being pressed
			POINT pt=((PMSLLHOOKSTRUCT)lParam)->pt;
			
			//Make sure cursorwnd isn't in the way
			if (sharedsettings.Cursor) {
				ShowWindow(cursorwnd,SW_HIDE);
			}
			
			//Get window
			if ((hwnd=WindowFromPoint(pt)) == NULL) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			hwnd=GetAncestor(hwnd,GA_ROOT);
			
			//Return if window is blacklisted
			if (blacklisted(hwnd,&settings.Blacklist)) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Get window and desktop size
			RECT window, desktop;
			if (GetWindowRect(hwnd,&window) == 0) {
				Error(L"GetWindowRect(&window)",L"LowLevelMouseProc()",GetLastError(),__LINE__);
			}
			if (GetWindowRect(GetDesktopWindow(),&desktop) == 0) {
				Error(L"GetWindowRect(&desktop)",L"LowLevelMouseProc()",GetLastError(),__LINE__);
			}
			//Return if the window is fullscreen
			if (window.left == desktop.left && window.top == desktop.top && window.right == desktop.right && window.bottom == desktop.bottom) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Check if this is a double-click
			if (GetTickCount()-clicktime <= GetDoubleClickTime()) {
				//Alt+double-clicking a window maximizes it
				//Maximize window
				WINDOWPLACEMENT wndpl;
				wndpl.length=sizeof(WINDOWPLACEMENT);
				GetWindowPlacement(hwnd,&wndpl);
				wndpl.showCmd=SW_MAXIMIZE;
				//Also roll-down the window if it's in the roll-up database
				int i;
				for (i=0; i < NUMROLLUP; i++) {
					if (rollup[i].hwnd == hwnd) {
						//Roll-down window
						RECT normalpos={window.left, window.top, window.left+rollup[i].width, window.top+rollup[i].height};
						wndpl.rcNormalPosition=normalpos;
						//Remove window from database
						rollup[i].hwnd=NULL;
					}
				}
				SetWindowPlacement(hwnd,&wndpl);
				//Stop move action
				move=0;
				//Hide cursorwnd
				if (sharedsettings.Cursor) {
					ShowWindow(cursorwnd,SW_HIDE);
					SetWindowLongPtr(cursorwnd,GWL_EXSTYLE,WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
					//Maybe show IDC_SIZEALL cursor here really quick somehow?
				}
				//Prevent mousedown from propagating
				return 1;
			}
			else {
				//Restore the window if it's maximized
				if (IsZoomed(hwnd)) {
					//Restore window
					WINDOWPLACEMENT wndpl;
					wndpl.length=sizeof(WINDOWPLACEMENT);
					GetWindowPlacement(hwnd,&wndpl);
					wndpl.showCmd=SW_RESTORE;
					SetWindowPlacement(hwnd,&wndpl);
					
					//Get new pos and size
					RECT newwindow;
					if (GetWindowRect(hwnd,&newwindow) == 0) {
						Error(L"GetWindowRect()",L"LowLevelMouseProc()",GetLastError(),__LINE__);
					}
					
					//Set offset
					offset.x=(float)(pt.x-window.left)/(window.right-window.left)*(newwindow.right-newwindow.left);
					offset.y=(float)(pt.y-window.top)/(window.bottom-window.top)*(newwindow.bottom-newwindow.top);
					
					//Move
					MoveWnd();
				}
				else {
					//Remember time of this click so we can check for double-click
					clicktime=GetTickCount();
					
					//Set offset
					offset.x=pt.x-window.left;
					offset.y=pt.y-window.top;
				}
				//Show cursorwnd
				if (sharedsettings.Cursor) {
					SetClassLongPtr(cursorwnd,GCLP_HCURSOR,(LONG_PTR)cursor[HAND]);
					if (!resize) {
						MoveWindow(cursorwnd,pt.x-20,pt.y-20,41,41,FALSE);
						SetWindowLongPtr(cursorwnd,GWL_EXSTYLE,WS_EX_LAYERED|WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
						SetLayeredWindowAttributes(cursorwnd,0,1,LWA_ALPHA); //Almost transparent
					}
					ShowWindowAsync(cursorwnd,SW_SHOWNA);
				}
				//Ready to move window
				move=1;
				//Prevent mousedown from propagating
				return 1;
			}
		}
		else if (wParam == WM_LBUTTONUP && move) {
			move=0;
			if (!alt) {
				UnhookMouse();
			}
			//Hide cursorwnd
			if (sharedsettings.Cursor) {
				if (resize) {
					SetClassLongPtr(cursorwnd,GCLP_HCURSOR,(LONG_PTR)cursor[resizecursor]);
				}
				else {
					ShowWindow(cursorwnd,SW_HIDE);
					SetWindowLongPtr(cursorwnd,GWL_EXSTYLE,WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
				}
			}
			//Prevent mouseup from propagating
			return 1;
		}
		else if (sharedsettings.RMBMinimize && wParam == WM_RBUTTONDOWN && alt) {
			//Double check if the left alt key is being pressed
			if (!(GetAsyncKeyState(VK_MENU)&0x8000)) {
				alt=0;
				UnhookMouse();
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Alt key is still being pressed
			POINT pt=((PMSLLHOOKSTRUCT)lParam)->pt;
			
			//Make sure cursorwnd isn't in the way
			if (sharedsettings.Cursor) {
				ShowWindow(cursorwnd,SW_HIDE);
			}
			
			//Get window
			if ((hwnd=WindowFromPoint(pt)) == NULL) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			hwnd=GetAncestor(hwnd,GA_ROOT);

			//Return if window is blacklisted
			if (blacklisted(hwnd,&settings.Blacklist)) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Minimize window
			WINDOWPLACEMENT wndpl;
			wndpl.length=sizeof(WINDOWPLACEMENT);
			GetWindowPlacement(hwnd,&wndpl);
			wndpl.showCmd=SW_MINIMIZE;
			SetWindowPlacement(hwnd,&wndpl);
			
			//Prevent mousedown from propagating
			return 1;
		}
		else if (sharedsettings.RMBMinimize && wParam == WM_RBUTTONUP && alt) {
			//Prevent mouseup from propagating
			return 1;
		}
		else if ((wParam == WM_MBUTTONDOWN || wParam == WM_RBUTTONDOWN) && alt) {
			//Double check if the left alt key is being pressed
			if (!(GetAsyncKeyState(VK_MENU)&0x8000)) {
				alt=0;
				UnhookMouse();
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Alt key is still being pressed
			POINT pt=((PMSLLHOOKSTRUCT)lParam)->pt;
			
			//Make sure cursorwnd isn't in the way
			if (sharedsettings.Cursor) {
				ShowWindow(cursorwnd,SW_HIDE);
			}
			
			//Get window
			if ((hwnd=WindowFromPoint(pt)) == NULL) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			hwnd=GetAncestor(hwnd,GA_ROOT);

			//Return if window is blacklisted
			if (blacklisted(hwnd,&settings.Blacklist)) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Get window and desktop size
			RECT window, desktop;
			if (GetWindowRect(hwnd,&window) == 0) {
				Error(L"GetWindowRect()",L"LowLevelMouseProc()",GetLastError(),__LINE__);
			}
			if (GetWindowRect(GetDesktopWindow(),&desktop) == 0) {
				Error(L"GetWindowRect()",L"LowLevelMouseProc()",GetLastError(),__LINE__);
			}
			//Return if the window is fullscreen
			if (window.left == desktop.left && window.top == desktop.top && window.right == desktop.right && window.bottom == desktop.bottom) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Roll-down the window if it's in the roll-up database
			int i;
			for (i=0; i < NUMROLLUP; i++) {
				if (rollup[i].hwnd == hwnd) {
					//Roll-down window
					if (MoveWindow(hwnd, window.left, window.top, rollup[i].width, rollup[i].height, TRUE) == 0) {
						Error(L"MoveWindow()",L"When rolling down window",GetLastError(),__LINE__);
					}
					//Remove window from database
					rollup[i].hwnd=NULL;
					//Prevent mousedown from propagating
					return 1;
				}
			}
			
			//Check if this is a double-click (or if both middle and right mouse button is pressed)
			if (GetTickCount()-clicktime <= GetDoubleClickTime() || resize) {
				//Alt+middle-double-clicking a window makes it roll-up
				//Store window size
				rollup[rolluppos].hwnd=hwnd;
				rollup[rolluppos].width=window.right-window.left;
				rollup[rolluppos].height=window.bottom-window.top;
				rolluppos=(rolluppos+1)%NUMROLLUP;
				//Roll-up window
				if (MoveWindow(hwnd, window.left, window.top, window.right-window.left, 30, TRUE) == 0) {
					Error(L"MoveWindow()",L"Roll-up",GetLastError(),__LINE__);
				}
				//Stop resize action
				resize=0;
				//Hide cursorwnd
				if (sharedsettings.Cursor) {
					ShowWindow(cursorwnd,SW_HIDE);
					SetWindowLongPtr(cursorwnd,GWL_EXSTYLE,WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
				}
				//Prevent mousedown from propagating
				return 1;
			}
			else {
				//Restore the window if it's maximized
				if (IsZoomed(hwnd)) {
					//Restore window
					WINDOWPLACEMENT wndpl;
					wndpl.length=sizeof(WINDOWPLACEMENT);
					GetWindowPlacement(hwnd,&wndpl);
					wndpl.showCmd=SW_RESTORE;
					//New size
					RECT normalpos={desktop.left,desktop.top,desktop.right,desktop.bottom};
					//Compensate for taskbar
					RECT taskbar;
					if (GetWindowRect(FindWindow(L"Shell_TrayWnd",NULL),&taskbar)) {
						if (taskbar.left == desktop.left && taskbar.right == desktop.right) {
							//Taskbar is on the bottom or top position, adjust height
							normalpos.bottom-=taskbar.bottom-taskbar.top;
						}
						else if (taskbar.top == desktop.top && taskbar.bottom == desktop.bottom) {
							//Taskbar is on the left or right position, adjust width
							normalpos.right-=taskbar.right-taskbar.left;
						}
					}
					wndpl.rcNormalPosition=normalpos;
					SetWindowPlacement(hwnd,&wndpl);
					//Get new window size
					GetWindowRect(hwnd,&window);
				}
				//Set edge and offset
				if (window.bottom-window.top < 60) {
					//This is a very thin window
					resize_y=BOTTOM;
					resize_offset.y=window.bottom-pt.y;
					if (pt.x-window.left < (window.right-window.left)/3) {
						resize_x=LEFT;
						resize_offset.x=pt.x-window.left;
					}
					else {
						resize_x=RIGHT;
						resize_offset.x=window.right-pt.x;
					}
				}
				else {
					//Think of the window as nine boxes
					if (pt.y-window.top < (window.bottom-window.top)/3) {
						resize_y=TOP;
						resize_offset.y=pt.y-window.top;
					}
					else if (pt.y-window.top < (window.bottom-window.top)*2/3) {
						resize_y=CENTER;
						resize_offset.y=pt.y; //Used only if both x and y are CENTER
					}
					else {
						resize_y=BOTTOM;
						resize_offset.y=window.bottom-pt.y;
					}
					if (pt.x-window.left < (window.right-window.left)/3) {
						resize_x=LEFT;
						resize_offset.x=pt.x-window.left;
					}
					else if (pt.x-window.left < (window.right-window.left)*2/3) {
						resize_x=CENTER;
						resize_offset.x=pt.x; //Used only if both x and y are CENTER
					}
					else {
						resize_x=RIGHT;
						resize_offset.x=window.right-pt.x;
					}
				}
				//Show cursorwnd
				if (sharedsettings.Cursor) {
					//Determine shape of cursor
					if ((resize_y == TOP && resize_x == LEFT)
					 || (resize_y == BOTTOM && resize_x == RIGHT)) {
						resizecursor=SIZENWSE;
					}
					else if ((resize_y == TOP && resize_x == RIGHT)
					 || (resize_y == BOTTOM && resize_x == LEFT)) {
						resizecursor=SIZENESW;
					}
					else if ((resize_y == TOP && resize_x == CENTER)
					 || (resize_y == BOTTOM && resize_x == CENTER)) {
						resizecursor=SIZENS;
					}
					else if ((resize_y == CENTER && resize_x == LEFT)
					 || (resize_y == CENTER && resize_x == RIGHT)) {
						resizecursor=SIZEWE;
					}
					else {
						resizecursor=SIZEALL;
					}
					//Change cursor
					if (!move) {
						MoveWindow(cursorwnd,pt.x-20,pt.y-20,41,41,FALSE);
						SetClassLongPtr(cursorwnd,GCLP_HCURSOR,(LONG_PTR)cursor[resizecursor]);
						SetWindowLongPtr(cursorwnd,GWL_EXSTYLE,WS_EX_LAYERED|WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
						SetLayeredWindowAttributes(cursorwnd,0,1,LWA_ALPHA); //Almost transparent
					}
					ShowWindowAsync(cursorwnd,SW_SHOWNA);
				}
				//Remember time of this click so we can check for double-click
				clicktime=GetTickCount();
				//Ready to resize window
				resize=1;
				//Prevent mousedown from propagating
				return 1;
			}
		}
		else if ((wParam == WM_MBUTTONUP || wParam == WM_RBUTTONUP) && resize) {
			resize=0;
			if (!alt) {
				UnhookMouse();
			}
			//Hide cursorwnd
			if (sharedsettings.Cursor && !move) {
				ShowWindow(cursorwnd,SW_HIDE);
				SetWindowLongPtr(cursorwnd,GWL_EXSTYLE,WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
			}
			//Prevent mouseup from propagating
			return 1;
		}
		else if (wParam == WM_MOUSEMOVE) {
			//Reset double-click time
			clicktime=0; //This prevents me from double-clicking in Windows 7 (running virtualized). Apparently double-click still works on non-virtualized Windows 7.
			if (move || resize) {
				//Move cursorwnd
				if (sharedsettings.Cursor) {
					POINT pt=((PMSLLHOOKSTRUCT)lParam)->pt;
					MoveWindow(cursorwnd,pt.x-20,pt.y-20,41,41,TRUE);
					//MoveWindow(cursorwnd,(prevpt.x<pt.x?prevpt.x:pt.x)-3,(prevpt.y<pt.y?prevpt.y:pt.y)-3,(pt.x>prevpt.x?pt.x-prevpt.x:prevpt.x-pt.x)+7,(pt.y>prevpt.y?pt.y-prevpt.y:prevpt.y-pt.y)+7,FALSE);
				}
				//Move or resize
				if (move) {
					//Move window
					MoveWnd();
				}
				else if (resize) {
					//Resize window
					ResizeWnd();
				}
			}
		}
	}
	
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int HookMouse() {
	if (mousehook) {
		//Mouse already hooked
		return 1;
	}
	
	//Set up the mouse hook
	if ((mousehook=SetWindowsHookEx(WH_MOUSE_LL,LowLevelMouseProc,hinstDLL,0)) == NULL) {
		Error(L"SetWindowsHookEx(WH_MOUSE_LL)",L"",GetLastError(),__LINE__);
		return 1;
	}
	
	//Success
	return 0;
}

int UnhookMouse() {
	if (!mousehook) {
		//Mouse not hooked
		return 1;
	}
	
	//Remove mouse hook
	if (UnhookWindowsHookEx(mousehook) == 0) {
		Error(L"UnhookWindowsHookEx(mousehook)",L"",GetLastError(),__LINE__);
		return 1;
	}
	
	//Success
	mousehook=NULL;
	clicktime=0;
	return 0;
}

//Msghook
_declspec(dllexport) LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_WINDOWPOSCHANGING && (shift || sharedsettings.AutoStick)) {
		WINDOWPOS *wndpos=(WINDOWPOS*)lParam;
		if (msgaction == MOVE && !(wndpos->flags&SWP_NOMOVE)) {
			MoveStick(&wndpos->x, &wndpos->y, wndpos->cx, wndpos->cy);
		}
		else if (msgaction == RESIZE && !(wndpos->flags&SWP_NOSIZE)) {
			ResizeStick(&wndpos->x, &wndpos->y, &wndpos->cx, &wndpos->cy);
		}
	}
	
	/* //Fun code to trap window on screen
	if (msg == WM_WINDOWPOSCHANGING) {
		WINDOWPOS *wndpos=(WINDOWPOS*)lParam;
		if (wndpos->x < 0) {
			wndpos->x=0;
		}
		if (wndpos->y < 0) {
			wndpos->y=0;
		}
		if (wndpos->x+wndpos->cx > 1920) {
			wndpos->x=1920-wndpos->cx;
		}
		if (wndpos->y+wndpos->cy > 1140) {
			wndpos->y=1140-wndpos->cy;
		}
	}
	*/
	
	return CallWindowProc(oldwndproc, hwnd, msg, wParam, lParam);
}

//CallWndProc is called in the context of the thread that calls SendMessage, not the thread that receives the message.
//Thus we have to explicitly share the memory we want CallWndProc to be able to access (shift, move, resize and hwnd)
_declspec(dllexport) LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION && !move && !resize) {
		CWPSTRUCT *msg=(CWPSTRUCT*)lParam;
		
		if (hwnd != msg->hwnd
		 && !move && !resize
		 && (msg->message == WM_ENTERSIZEMOVE || msg->message == WM_WINDOWPOSCHANGING)
		 && (shift || sharedsettings.AutoStick)
		 && IsWindowVisible(msg->hwnd)
		 && !(GetWindowLongPtr(msg->hwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)
		 && !IsIconic(msg->hwnd) && !IsZoomed(msg->hwnd)
		 && msg->hwnd == GetAncestor(msg->hwnd,GA_ROOT)) {
			//Double check if any of the shift keys are being pressed
			if (shift && !(GetAsyncKeyState(VK_SHIFT)&0x8000)) {
				shift=0;
				if (!sharedsettings.AutoStick) {
					return CallNextHookEx(NULL, nCode, wParam, lParam);
				}
			}
			
			//Return if window is blacklisted
			if (blacklisted(msg->hwnd,&settings.Blacklist)) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Restore old WndProc if another window has already been subclassed
			if (oldwndproc != NULL) {
				if (SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)oldwndproc) == 0) {
					Error(L"SetWindowLongPtr(hwnd, GWLP_WNDPROC, oldwndproc)",L"Failed to restore subclassed window to its old wndproc.",GetLastError(),__LINE__);
				}
				oldwndproc=NULL;
			}
			
			//Set hwnd
			hwnd=msg->hwnd;
			//Subclass window
			if ((oldwndproc=(WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc)) == 0) {
				Error(L"SetWindowLongPtr(hwnd, GWLP_WNDPROC, CustomWndProc)",L"Failed to subclass window.",GetLastError(),__LINE__);
			}
		}
		
		if (msg->message == WM_SYSCOMMAND) {
			WPARAM action=msg->wParam&0xFFF0;
			if (action == SC_MOVE) {
				msgaction=MOVE;
			}
			else if (action == SC_SIZE) {
				msgaction=RESIZE;
				int edge=msg->wParam&0x000F; //These are the undocumented bits (compatible with WMSZ_*)
				//Set offset
				//resize_x
				if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
					resize_x=CENTER;
				}
				if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
					resize_x=LEFT;
				}
				else if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT) {
					resize_x=RIGHT;
				}
				//resize_y
				if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
					resize_y=CENTER;
				}
				if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
					resize_y=TOP;
				}
				else if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) {
					resize_y=BOTTOM;
				}
				resize_offset.x=0;
				resize_offset.y=0;
			}
		}
		
		if ((msg->message == WM_EXITSIZEMOVE || msg->message == WM_DESTROY)
		 && msg->hwnd == hwnd && oldwndproc != NULL) {
			//Restore old WndProc
			if (SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)oldwndproc) == 0) {
				Error(L"SetWindowLongPtr(hwnd, GWLP_WNDPROC, oldwndproc)",L"Failed to restore subclassed window to its old wndproc.",GetLastError(),__LINE__);
			}
			oldwndproc=NULL;
			hwnd=NULL;
		}
	}
	
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

_declspec(dllexport) void ClearSharedSettingsLoaded() {
	sharedsettings_loaded=0;
}

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		hinstDLL=hInstance;
		//Load settings
		//Settings shared with CallWndProc hook
		if (!sharedsettings_loaded) {
			sharedsettings_loaded=1;
			//Have to store path to ini file at initial load so CallWndProc hooks can find it
			GetModuleFileName(NULL,inipath,sizeof(inipath)/sizeof(wchar_t));
			PathRenameExtension(inipath,L".ini");
			//Cursor
			GetPrivateProfileString(APP_NAME,L"Cursor",L"0",txt,sizeof(txt)/sizeof(wchar_t),inipath);
			swscanf(txt,L"%d",&sharedsettings.Cursor);
			if (sharedsettings.Cursor) {
				cursorwnd=FindWindow(APP_NAME,NULL);
				cursor[HAND]=    LoadImage(NULL, IDC_HAND,     IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZENWSE]=LoadImage(NULL, IDC_SIZENWSE, IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZENESW]=LoadImage(NULL, IDC_SIZENESW, IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZENS]=  LoadImage(NULL, IDC_SIZENS,   IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZEWE]=  LoadImage(NULL, IDC_SIZEWE,   IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZEALL]= LoadImage(NULL, IDC_SIZEALL,  IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
			}
			//AutoStick
			GetPrivateProfileString(APP_NAME,L"AutoStick",L"0",txt,sizeof(txt)/sizeof(wchar_t),inipath);
			swscanf(txt,L"%d",&sharedsettings.AutoStick);
			//RMBMinimize
			GetPrivateProfileString(APP_NAME,L"RMBMinimize",L"0",txt,sizeof(txt)/sizeof(wchar_t),inipath);
			swscanf(txt,L"%d",&sharedsettings.RMBMinimize);
			//Zero-out roll-up hwnds
			int i;
			for (i=0; i < NUMROLLUP; i++) {
				rollup[i].hwnd=NULL;
			}
		}
		//Blacklist
		GetPrivateProfileString(APP_NAME,L"Blacklist",L"",txt,sizeof(txt)/sizeof(wchar_t),inipath);
		int blacklist_alloc=0;
		wchar_t *pos=txt;
		struct blacklist *add_blacklist=&settings.Blacklist;
		while (pos != NULL) {
			wchar_t *title=pos;
			wchar_t *classname=wcsstr(pos,L"|");
			pos=wcsstr(pos,L",");
			if (pos != NULL) {
				*pos='\0';
				pos++;
			}
			if (classname != NULL) {
				*classname='\0';
				classname++;
			}
			//Make sure we have enough space
			if (add_blacklist->numitems == blacklist_alloc) {
				blacklist_alloc+=10;
				add_blacklist->items=realloc(add_blacklist->items,blacklist_alloc*sizeof(struct blacklistitem));
			}
			//Allocate memory for title and classname
			wchar_t *item_title, *item_classname;
			if (wcslen(title) > 0) {
				item_title=malloc((wcslen(title)+1)*sizeof(wchar_t));
				wcscpy(item_title,title);
			}
			else {
				item_title=NULL;
			}
			if (classname != NULL && wcslen(classname) > 0) {
				item_classname=malloc((wcslen(classname)+1)*sizeof(wchar_t));
				wcscpy(item_classname,classname);
			}
			else {
				item_classname=NULL;
			}
			//Only store item if it's not empty
			if (item_title != NULL || item_classname != NULL) {
				add_blacklist->items[add_blacklist->numitems].title=item_title;
				add_blacklist->items[add_blacklist->numitems].classname=item_classname;
				add_blacklist->numitems++;
			}
			//Switch gears to Blacklist_Sticky?
			if (pos == NULL && add_blacklist == &settings.Blacklist) {
				add_blacklist=&settings.Blacklist_Sticky;
				blacklist_alloc=0;
				GetPrivateProfileString(APP_NAME,L"Blacklist_Sticky",L"",txt,sizeof(txt)/sizeof(wchar_t),inipath);
				pos=txt;
			}
		}
		//Allocate space for wnds
		wnds_alloc+=5;
		if ((wnds=realloc(wnds,wnds_alloc*sizeof(HWND))) == NULL) {
			Error(L"realloc(wnds)",L"Out of memory?",0,__LINE__);
			return FALSE;
		}
	}
	else if (reason == DLL_PROCESS_DETACH) {
		//Restore old WndProc
		if (oldwndproc != NULL) {
			SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)oldwndproc);
		}
		//Free memory
		int i;
		for (i=0; i < settings.Blacklist.numitems; i++) {
			free(settings.Blacklist.items[i].title);
			free(settings.Blacklist.items[i].classname);
		}
		settings.Blacklist.numitems=0;
		free(settings.Blacklist.items);
		for (i=0; i < settings.Blacklist_Sticky.numitems; i++) {
			free(settings.Blacklist_Sticky.items[i].title);
			free(settings.Blacklist_Sticky.items[i].classname);
		}
		settings.Blacklist_Sticky.numitems=0;
		free(settings.Blacklist_Sticky.items);
		free(wnds);
	}
	return TRUE;
}