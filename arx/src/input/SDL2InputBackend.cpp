/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "input/SDL2InputBackend.h"

#include <algorithm>
#include <glm/glm.hpp>

#include "Configure.h"
#include "io/log/Logger.h"
#include "math/Rectangle.h"
#include "platform/Platform.h"
#include "platform/PlatformConfig.h"

#if ARX_HAVE_RTX_REMIX
#include "graphics/remix/RemixScene.h"
#if ARX_PLATFORM == ARX_PLATFORM_WIN32
#include <windows.h>
#endif
#endif

static Keyboard::Key sdlToArxKey[SDL_NUM_SCANCODES];

static Mouse::Button sdlToArxButton[10];

SDL2InputBackend::SDL2InputBackend(SDL2Window * window)
	: m_window(window)
	, m_textHandler(nullptr)
	, m_editCursorPos(0)
	, m_editCursorLength(0)
	, wheel(0)
	, cursorAbs(0)
	, cursorRel(0)
	, cursorRelAccum(0)
	, cursorInWindow(false)
	, currentWheel(0)
{
	
	arx_assert(window != nullptr);
	
	SDL_EventState(SDL_WINDOWEVENT, SDL_ENABLE);
	SDL_EventState(SDL_KEYDOWN, SDL_ENABLE);
	SDL_EventState(SDL_KEYUP, SDL_ENABLE);
	SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
	SDL_EventState(SDL_TEXTEDITING, SDL_ENABLE);
	#if SDL_VERSION_ATLEAST(2, 0, 5)
	SDL_EventState(SDL_DROPTEXT, SDL_ENABLE);
	#endif
	SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
	
	std::fill_n(sdlToArxKey, std::size(sdlToArxKey), Keyboard::Key_Invalid);
	
	sdlToArxKey[SDL_SCANCODE_BACKSPACE] = Keyboard::Key_Backspace;
	sdlToArxKey[SDL_SCANCODE_TAB] = Keyboard::Key_Tab;
	sdlToArxKey[SDL_SCANCODE_RETURN] = Keyboard::Key_Enter;
	sdlToArxKey[SDL_SCANCODE_PAUSE] = Keyboard::Key_Pause;
	sdlToArxKey[SDL_SCANCODE_ESCAPE] = Keyboard::Key_Escape;
	sdlToArxKey[SDL_SCANCODE_SPACE] = Keyboard::Key_Spacebar;
	sdlToArxKey[SDL_SCANCODE_COMMA] = Keyboard::Key_Comma;
	sdlToArxKey[SDL_SCANCODE_MINUS] = Keyboard::Key_Minus;
	sdlToArxKey[SDL_SCANCODE_PERIOD] = Keyboard::Key_Period;
	sdlToArxKey[SDL_SCANCODE_SLASH] = Keyboard::Key_Slash;
	sdlToArxKey[SDL_SCANCODE_APOSTROPHE] = Keyboard::Key_Apostrophe;
	sdlToArxKey[SDL_SCANCODE_GRAVE] = Keyboard::Key_Grave;
	sdlToArxKey[SDL_SCANCODE_0] = Keyboard::Key_0;
	sdlToArxKey[SDL_SCANCODE_1] = Keyboard::Key_1;
	sdlToArxKey[SDL_SCANCODE_2] = Keyboard::Key_2;
	sdlToArxKey[SDL_SCANCODE_3] = Keyboard::Key_3;
	sdlToArxKey[SDL_SCANCODE_4] = Keyboard::Key_4;
	sdlToArxKey[SDL_SCANCODE_5] = Keyboard::Key_5;
	sdlToArxKey[SDL_SCANCODE_6] = Keyboard::Key_6;
	sdlToArxKey[SDL_SCANCODE_7] = Keyboard::Key_7;
	sdlToArxKey[SDL_SCANCODE_8] = Keyboard::Key_8;
	sdlToArxKey[SDL_SCANCODE_9] = Keyboard::Key_9;
	sdlToArxKey[SDL_SCANCODE_SEMICOLON] = Keyboard::Key_Semicolon;
	sdlToArxKey[SDL_SCANCODE_EQUALS] = Keyboard::Key_Equals;
	sdlToArxKey[SDL_SCANCODE_LEFTBRACKET] = Keyboard::Key_LeftBracket;
	sdlToArxKey[SDL_SCANCODE_BACKSLASH] = Keyboard::Key_Backslash;
	sdlToArxKey[SDL_SCANCODE_NONUSHASH] = Keyboard::Key_Backslash;
	sdlToArxKey[SDL_SCANCODE_NONUSBACKSLASH] = Keyboard::Key_Backslash;
	sdlToArxKey[SDL_SCANCODE_RIGHTBRACKET] = Keyboard::Key_RightBracket;
	sdlToArxKey[SDL_SCANCODE_A] = Keyboard::Key_A;
	sdlToArxKey[SDL_SCANCODE_B] = Keyboard::Key_B;
	sdlToArxKey[SDL_SCANCODE_C] = Keyboard::Key_C;
	sdlToArxKey[SDL_SCANCODE_D] = Keyboard::Key_D;
	sdlToArxKey[SDL_SCANCODE_E] = Keyboard::Key_E;
	sdlToArxKey[SDL_SCANCODE_F] = Keyboard::Key_F;
	sdlToArxKey[SDL_SCANCODE_G] = Keyboard::Key_G;
	sdlToArxKey[SDL_SCANCODE_H] = Keyboard::Key_H;
	sdlToArxKey[SDL_SCANCODE_I] = Keyboard::Key_I;
	sdlToArxKey[SDL_SCANCODE_J] = Keyboard::Key_J;
	sdlToArxKey[SDL_SCANCODE_K] = Keyboard::Key_K;
	sdlToArxKey[SDL_SCANCODE_L] = Keyboard::Key_L;
	sdlToArxKey[SDL_SCANCODE_M] = Keyboard::Key_M;
	sdlToArxKey[SDL_SCANCODE_N] = Keyboard::Key_N;
	sdlToArxKey[SDL_SCANCODE_O] = Keyboard::Key_O;
	sdlToArxKey[SDL_SCANCODE_P] = Keyboard::Key_P;
	sdlToArxKey[SDL_SCANCODE_Q] = Keyboard::Key_Q;
	sdlToArxKey[SDL_SCANCODE_R] = Keyboard::Key_R;
	sdlToArxKey[SDL_SCANCODE_S] = Keyboard::Key_S;
	sdlToArxKey[SDL_SCANCODE_T] = Keyboard::Key_T;
	sdlToArxKey[SDL_SCANCODE_U] = Keyboard::Key_U;
	sdlToArxKey[SDL_SCANCODE_V] = Keyboard::Key_V;
	sdlToArxKey[SDL_SCANCODE_W] = Keyboard::Key_W;
	sdlToArxKey[SDL_SCANCODE_X] = Keyboard::Key_X;
	sdlToArxKey[SDL_SCANCODE_Y] = Keyboard::Key_Y;
	sdlToArxKey[SDL_SCANCODE_Z] = Keyboard::Key_Z;
	sdlToArxKey[SDL_SCANCODE_DELETE] = Keyboard::Key_Delete;
	sdlToArxKey[SDL_SCANCODE_KP_0] = Keyboard::Key_NumPad0;
	sdlToArxKey[SDL_SCANCODE_KP_1] = Keyboard::Key_NumPad1;
	sdlToArxKey[SDL_SCANCODE_KP_2] = Keyboard::Key_NumPad2;
	sdlToArxKey[SDL_SCANCODE_KP_3] = Keyboard::Key_NumPad3;
	sdlToArxKey[SDL_SCANCODE_KP_4] = Keyboard::Key_NumPad4;
	sdlToArxKey[SDL_SCANCODE_KP_5] = Keyboard::Key_NumPad5;
	sdlToArxKey[SDL_SCANCODE_KP_6] = Keyboard::Key_NumPad6;
	sdlToArxKey[SDL_SCANCODE_KP_7] = Keyboard::Key_NumPad7;
	sdlToArxKey[SDL_SCANCODE_KP_8] = Keyboard::Key_NumPad8;
	sdlToArxKey[SDL_SCANCODE_KP_9] = Keyboard::Key_NumPad9;
	sdlToArxKey[SDL_SCANCODE_KP_PERIOD] = Keyboard::Key_NumPoint;
	sdlToArxKey[SDL_SCANCODE_KP_DIVIDE] = Keyboard::Key_NumDivide;
	sdlToArxKey[SDL_SCANCODE_KP_MULTIPLY] = Keyboard::Key_NumMultiply;
	sdlToArxKey[SDL_SCANCODE_KP_MINUS] = Keyboard::Key_NumSubtract;
	sdlToArxKey[SDL_SCANCODE_KP_PLUS] = Keyboard::Key_NumAdd;
	sdlToArxKey[SDL_SCANCODE_KP_ENTER] = Keyboard::Key_NumPadEnter;
	sdlToArxKey[SDL_SCANCODE_KP_EQUALS] = Keyboard::Key_NumPadEnter;
	sdlToArxKey[SDL_SCANCODE_KP_COMMA] = Keyboard::Key_NumComma;
	sdlToArxKey[SDL_SCANCODE_KP_00] = Keyboard::Key_Num00;
	sdlToArxKey[SDL_SCANCODE_KP_000] = Keyboard::Key_Num000;
	sdlToArxKey[SDL_SCANCODE_KP_LEFTPAREN] = Keyboard::Key_NumLeftParen;
	sdlToArxKey[SDL_SCANCODE_KP_RIGHTPAREN] = Keyboard::Key_NumRightParen;
	sdlToArxKey[SDL_SCANCODE_KP_LEFTBRACE] = Keyboard::Key_NumLeftBrace;
	sdlToArxKey[SDL_SCANCODE_KP_RIGHTBRACE] = Keyboard::Key_NumRightBrace;
	sdlToArxKey[SDL_SCANCODE_KP_TAB] = Keyboard::Key_NumTab;
	sdlToArxKey[SDL_SCANCODE_KP_BACKSPACE] = Keyboard::Key_NumBackspace;
	sdlToArxKey[SDL_SCANCODE_KP_A] = Keyboard::Key_NumA;
	sdlToArxKey[SDL_SCANCODE_KP_B] = Keyboard::Key_NumB;
	sdlToArxKey[SDL_SCANCODE_KP_C] = Keyboard::Key_NumC;
	sdlToArxKey[SDL_SCANCODE_KP_D] = Keyboard::Key_NumD;
	sdlToArxKey[SDL_SCANCODE_KP_E] = Keyboard::Key_NumE;
	sdlToArxKey[SDL_SCANCODE_KP_F] = Keyboard::Key_NumF;
	sdlToArxKey[SDL_SCANCODE_KP_XOR] = Keyboard::Key_NumXor;
	sdlToArxKey[SDL_SCANCODE_KP_POWER] = Keyboard::Key_NumPower;
	sdlToArxKey[SDL_SCANCODE_KP_PERCENT] = Keyboard::Key_NumPercent;
	sdlToArxKey[SDL_SCANCODE_KP_LESS] = Keyboard::Key_NumLess;
	sdlToArxKey[SDL_SCANCODE_KP_GREATER] = Keyboard::Key_NumGreater;
	sdlToArxKey[SDL_SCANCODE_KP_AMPERSAND] = Keyboard::Key_NumAmpersand;
	sdlToArxKey[SDL_SCANCODE_KP_DBLAMPERSAND] = Keyboard::Key_NumDblAmpersand;
	sdlToArxKey[SDL_SCANCODE_KP_VERTICALBAR] = Keyboard::Key_NumVerticalBar;
	sdlToArxKey[SDL_SCANCODE_KP_DBLVERTICALBAR] = Keyboard::Key_NumDblVerticalBar;
	sdlToArxKey[SDL_SCANCODE_KP_COLON] = Keyboard::Key_NumColon;
	sdlToArxKey[SDL_SCANCODE_KP_HASH] = Keyboard::Key_NumHash;
	sdlToArxKey[SDL_SCANCODE_KP_SPACE] = Keyboard::Key_NumSpace;
	sdlToArxKey[SDL_SCANCODE_KP_AT] = Keyboard::Key_NumAt;
	sdlToArxKey[SDL_SCANCODE_KP_EXCLAM] = Keyboard::Key_NumExclam;
	sdlToArxKey[SDL_SCANCODE_KP_MEMSTORE] = Keyboard::Key_NumMemStore;
	sdlToArxKey[SDL_SCANCODE_KP_MEMRECALL] = Keyboard::Key_NumMemRecall;
	sdlToArxKey[SDL_SCANCODE_KP_MEMCLEAR] = Keyboard::Key_NumMemClear;
	sdlToArxKey[SDL_SCANCODE_KP_MEMADD] = Keyboard::Key_NumMemAdd;
	sdlToArxKey[SDL_SCANCODE_KP_MEMSUBTRACT] = Keyboard::Key_NumMemSubtract;
	sdlToArxKey[SDL_SCANCODE_KP_MEMMULTIPLY] = Keyboard::Key_NumMemMultiply;
	sdlToArxKey[SDL_SCANCODE_KP_MEMDIVIDE] = Keyboard::Key_NumMemDivide;
	sdlToArxKey[SDL_SCANCODE_KP_PLUSMINUS] = Keyboard::Key_NumPlusMinus;
	sdlToArxKey[SDL_SCANCODE_KP_CLEAR] = Keyboard::Key_NumClear;
	sdlToArxKey[SDL_SCANCODE_KP_CLEARENTRY] = Keyboard::Key_NumClearEntry;
	sdlToArxKey[SDL_SCANCODE_KP_BINARY] = Keyboard::Key_NumBinary;
	sdlToArxKey[SDL_SCANCODE_KP_OCTAL] = Keyboard::Key_NumOctal;
	sdlToArxKey[SDL_SCANCODE_KP_DECIMAL] = Keyboard::Key_NumDecimal;
	sdlToArxKey[SDL_SCANCODE_KP_HEXADECIMAL] = Keyboard::Key_NumHexadecimal;
	sdlToArxKey[SDL_SCANCODE_UP] = Keyboard::Key_UpArrow;
	sdlToArxKey[SDL_SCANCODE_DOWN] = Keyboard::Key_DownArrow;
	sdlToArxKey[SDL_SCANCODE_RIGHT] = Keyboard::Key_RightArrow;
	sdlToArxKey[SDL_SCANCODE_LEFT] = Keyboard::Key_LeftArrow;
	sdlToArxKey[SDL_SCANCODE_INSERT] = Keyboard::Key_Insert;
	sdlToArxKey[SDL_SCANCODE_HOME] = Keyboard::Key_Home;
	sdlToArxKey[SDL_SCANCODE_AC_HOME] = Keyboard::Key_Home;
	sdlToArxKey[SDL_SCANCODE_END] = Keyboard::Key_End;
	sdlToArxKey[SDL_SCANCODE_PAGEUP] = Keyboard::Key_PageUp;
	sdlToArxKey[SDL_SCANCODE_PAGEDOWN] = Keyboard::Key_PageDown;
	sdlToArxKey[SDL_SCANCODE_F1] = Keyboard::Key_F1;
	sdlToArxKey[SDL_SCANCODE_F2] = Keyboard::Key_F2;
	sdlToArxKey[SDL_SCANCODE_F3] = Keyboard::Key_F3;
	sdlToArxKey[SDL_SCANCODE_F4] = Keyboard::Key_F4;
	sdlToArxKey[SDL_SCANCODE_F5] = Keyboard::Key_F5;
	sdlToArxKey[SDL_SCANCODE_F6] = Keyboard::Key_F6;
	sdlToArxKey[SDL_SCANCODE_F7] = Keyboard::Key_F7;
	sdlToArxKey[SDL_SCANCODE_F8] = Keyboard::Key_F8;
	sdlToArxKey[SDL_SCANCODE_F9] = Keyboard::Key_F9;
	sdlToArxKey[SDL_SCANCODE_F10] = Keyboard::Key_F10;
	sdlToArxKey[SDL_SCANCODE_F11] = Keyboard::Key_F11;
	sdlToArxKey[SDL_SCANCODE_F12] = Keyboard::Key_F12;
	sdlToArxKey[SDL_SCANCODE_F13] = Keyboard::Key_F13;
	sdlToArxKey[SDL_SCANCODE_F14] = Keyboard::Key_F14;
	sdlToArxKey[SDL_SCANCODE_F15] = Keyboard::Key_F15;
	sdlToArxKey[SDL_SCANCODE_F16] = Keyboard::Key_F16;
	sdlToArxKey[SDL_SCANCODE_F17] = Keyboard::Key_F17;
	sdlToArxKey[SDL_SCANCODE_F18] = Keyboard::Key_F18;
	sdlToArxKey[SDL_SCANCODE_F19] = Keyboard::Key_F19;
	sdlToArxKey[SDL_SCANCODE_F20] = Keyboard::Key_F20;
	sdlToArxKey[SDL_SCANCODE_F21] = Keyboard::Key_F21;
	sdlToArxKey[SDL_SCANCODE_F22] = Keyboard::Key_F22;
	sdlToArxKey[SDL_SCANCODE_F23] = Keyboard::Key_F23;
	sdlToArxKey[SDL_SCANCODE_F24] = Keyboard::Key_F24;
	sdlToArxKey[SDL_SCANCODE_NUMLOCKCLEAR] = Keyboard::Key_NumLock;
	sdlToArxKey[SDL_SCANCODE_CAPSLOCK] = Keyboard::Key_CapsLock;
	sdlToArxKey[SDL_SCANCODE_SCROLLLOCK] = Keyboard::Key_ScrollLock;
	sdlToArxKey[SDL_SCANCODE_RSHIFT] = Keyboard::Key_RightShift;
	sdlToArxKey[SDL_SCANCODE_LSHIFT] = Keyboard::Key_LeftShift;
	sdlToArxKey[SDL_SCANCODE_RCTRL] = Keyboard::Key_RightCtrl;
	sdlToArxKey[SDL_SCANCODE_LCTRL] = Keyboard::Key_LeftCtrl;
	sdlToArxKey[SDL_SCANCODE_RALT] = Keyboard::Key_RightAlt;
	sdlToArxKey[SDL_SCANCODE_LALT] = Keyboard::Key_LeftAlt;
	sdlToArxKey[SDL_SCANCODE_RGUI] = Keyboard::Key_RightWin;
	sdlToArxKey[SDL_SCANCODE_LGUI] = Keyboard::Key_LeftWin;
	sdlToArxKey[SDL_SCANCODE_MODE] = Keyboard::Key_RightAlt;
	sdlToArxKey[SDL_SCANCODE_APPLICATION] = Keyboard::Key_Apps;
	sdlToArxKey[SDL_SCANCODE_PRINTSCREEN] = Keyboard::Key_PrintScreen;
	sdlToArxKey[SDL_SCANCODE_EXECUTE] = Keyboard::Key_Execute;
	sdlToArxKey[SDL_SCANCODE_HELP] = Keyboard::Key_Help;
	sdlToArxKey[SDL_SCANCODE_MENU] = Keyboard::Key_Menu;
	sdlToArxKey[SDL_SCANCODE_SELECT] = Keyboard::Key_Select;
	sdlToArxKey[SDL_SCANCODE_STOP] = Keyboard::Key_Stop;
	sdlToArxKey[SDL_SCANCODE_AGAIN] = Keyboard::Key_Redo;
	sdlToArxKey[SDL_SCANCODE_UNDO] = Keyboard::Key_Undo;
	sdlToArxKey[SDL_SCANCODE_CUT] = Keyboard::Key_Cut;
	sdlToArxKey[SDL_SCANCODE_COPY] = Keyboard::Key_Copy;
	sdlToArxKey[SDL_SCANCODE_PASTE] = Keyboard::Key_Paste;
	sdlToArxKey[SDL_SCANCODE_FIND] = Keyboard::Key_Find;
	sdlToArxKey[SDL_SCANCODE_MUTE] = Keyboard::Key_Mute;
	sdlToArxKey[SDL_SCANCODE_VOLUMEUP] = Keyboard::Key_VolumeUp;
	sdlToArxKey[SDL_SCANCODE_VOLUMEDOWN] = Keyboard::Key_VolumeDown;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL1] = Keyboard::Key_International1;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL2] = Keyboard::Key_International2;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL3] = Keyboard::Key_International3;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL4] = Keyboard::Key_International4;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL5] = Keyboard::Key_International5;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL6] = Keyboard::Key_International6;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL7] = Keyboard::Key_International7;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL8] = Keyboard::Key_International8;
	sdlToArxKey[SDL_SCANCODE_INTERNATIONAL9] = Keyboard::Key_International9;
	sdlToArxKey[SDL_SCANCODE_LANG1] = Keyboard::Key_Lang1;
	sdlToArxKey[SDL_SCANCODE_LANG2] = Keyboard::Key_Lang2;
	sdlToArxKey[SDL_SCANCODE_LANG3] = Keyboard::Key_Lang3;
	sdlToArxKey[SDL_SCANCODE_LANG4] = Keyboard::Key_Lang4;
	sdlToArxKey[SDL_SCANCODE_LANG5] = Keyboard::Key_Lang5;
	sdlToArxKey[SDL_SCANCODE_LANG6] = Keyboard::Key_Lang6;
	sdlToArxKey[SDL_SCANCODE_LANG7] = Keyboard::Key_Lang7;
	sdlToArxKey[SDL_SCANCODE_LANG8] = Keyboard::Key_Lang8;
	sdlToArxKey[SDL_SCANCODE_LANG9] = Keyboard::Key_Lang9;
	sdlToArxKey[SDL_SCANCODE_ALTERASE] = Keyboard::Key_AltErase;
	sdlToArxKey[SDL_SCANCODE_SYSREQ] = Keyboard::Key_SysReq;
	sdlToArxKey[SDL_SCANCODE_CANCEL] = Keyboard::Key_Cancel;
	sdlToArxKey[SDL_SCANCODE_CLEAR] = Keyboard::Key_Clear;
	sdlToArxKey[SDL_SCANCODE_PRIOR] = Keyboard::Key_Prior;
	sdlToArxKey[SDL_SCANCODE_RETURN2] = Keyboard::Key_Return2;
	sdlToArxKey[SDL_SCANCODE_SEPARATOR] = Keyboard::Key_Separator;
	sdlToArxKey[SDL_SCANCODE_OUT] = Keyboard::Key_Out;
	sdlToArxKey[SDL_SCANCODE_OPER] = Keyboard::Key_Oper;
	sdlToArxKey[SDL_SCANCODE_CLEARAGAIN] = Keyboard::Key_ClearAgain;
	sdlToArxKey[SDL_SCANCODE_CRSEL] = Keyboard::Key_CrSel;
	sdlToArxKey[SDL_SCANCODE_EXSEL] = Keyboard::Key_ExSel;
	sdlToArxKey[SDL_SCANCODE_THOUSANDSSEPARATOR] = Keyboard::Key_ThousandsSeparator;
	sdlToArxKey[SDL_SCANCODE_DECIMALSEPARATOR] = Keyboard::Key_DecimalSeparator;
	sdlToArxKey[SDL_SCANCODE_CURRENCYUNIT] = Keyboard::Key_CurrencyUnit;
	sdlToArxKey[SDL_SCANCODE_CURRENCYSUBUNIT] = Keyboard::Key_CurrencySubUnit;
	sdlToArxKey[SDL_SCANCODE_AUDIONEXT] = Keyboard::Key_AudioNext;
	sdlToArxKey[SDL_SCANCODE_AUDIOPREV] = Keyboard::Key_AudioPrev;
	sdlToArxKey[SDL_SCANCODE_AUDIOSTOP] = Keyboard::Key_AudioStop;
	sdlToArxKey[SDL_SCANCODE_AUDIOPLAY] = Keyboard::Key_AudioPlay;
	sdlToArxKey[SDL_SCANCODE_AUDIOMUTE] = Keyboard::Key_AudioMute;
	sdlToArxKey[SDL_SCANCODE_MEDIASELECT] = Keyboard::Key_Media;
	sdlToArxKey[SDL_SCANCODE_WWW] = Keyboard::Key_WWW;
	sdlToArxKey[SDL_SCANCODE_MAIL] = Keyboard::Key_Mail;
	sdlToArxKey[SDL_SCANCODE_CALCULATOR] = Keyboard::Key_Calculator;
	sdlToArxKey[SDL_SCANCODE_COMPUTER] = Keyboard::Key_Computer;
	sdlToArxKey[SDL_SCANCODE_AC_SEARCH] = Keyboard::Key_ACSearch;
	sdlToArxKey[SDL_SCANCODE_AC_HOME] = Keyboard::Key_ACHome;
	sdlToArxKey[SDL_SCANCODE_AC_BACK] = Keyboard::Key_ACBack;
	sdlToArxKey[SDL_SCANCODE_AC_FORWARD] = Keyboard::Key_ACForward;
	sdlToArxKey[SDL_SCANCODE_AC_STOP] = Keyboard::Key_ACStop;
	sdlToArxKey[SDL_SCANCODE_AC_REFRESH] = Keyboard::Key_ACRefresh;
	sdlToArxKey[SDL_SCANCODE_AC_BOOKMARKS] = Keyboard::Key_ACBookmarks;
	
	std::fill_n(sdlToArxButton, std::size(sdlToArxButton), Mouse::Button_Invalid);
	
	static_assert(9 < ARRAY_SIZE(sdlToArxButton), "array size mismatch");
	sdlToArxButton[8] = Mouse::Button_5;
	sdlToArxButton[9] = Mouse::Button_6;
	
	static_assert(SDL_BUTTON_LEFT < ARRAY_SIZE(sdlToArxButton), "array size mismatch");
	sdlToArxButton[SDL_BUTTON_LEFT] = Mouse::Button_0;
	static_assert(SDL_BUTTON_MIDDLE < ARRAY_SIZE(sdlToArxButton), "array size mismatch");
	sdlToArxButton[SDL_BUTTON_MIDDLE] = Mouse::Button_2;
	static_assert(SDL_BUTTON_RIGHT < ARRAY_SIZE(sdlToArxButton), "array size mismatch");
	sdlToArxButton[SDL_BUTTON_RIGHT] = Mouse::Button_1;
	static_assert(SDL_BUTTON_X1 < ARRAY_SIZE(sdlToArxButton), "array size mismatch");
	sdlToArxButton[SDL_BUTTON_X1] = Mouse::Button_3;
	static_assert(SDL_BUTTON_X2 < ARRAY_SIZE(sdlToArxButton), "array size mismatch");
	sdlToArxButton[SDL_BUTTON_X2] = Mouse::Button_4;
	
	std::fill_n(keyStates, std::size(keyStates), false);
	std::fill_n(clickCount, std::size(clickCount), 0);
	std::fill_n(unclickCount, std::size(unclickCount), 0);
	
}

#if ARX_HAVE_RTX_REMIX && ARX_PLATFORM == ARX_PLATFORM_WIN32
/*
 * Remix subclasses the HWND and swallows WM_KEYDOWN/KEYUP. SDL mouse motion
 * still arrives; the keyboard does not. Poll the OS key state and write it
 * into keyStates. This is the source of truth while Remix is live — do not
 * merge with SDL, or a KEYDOWN that never got a KEYUP sticks forever.
 */
static Keyboard::Key vkToArxKey(int vk) {
	
	if(vk >= 'A' && vk <= 'Z') {
		return Keyboard::Key(Keyboard::Key_A + (vk - 'A'));
	}
	if(vk >= '0' && vk <= '9') {
		return Keyboard::Key(Keyboard::Key_0 + (vk - '0'));
	}
	if(vk >= VK_F1 && vk <= VK_F24) {
		return Keyboard::Key(Keyboard::Key_F1 + (vk - VK_F1));
	}
	if(vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
		return Keyboard::Key(Keyboard::Key_NumPad0 + (vk - VK_NUMPAD0));
	}
	
	switch(vk) {
		case VK_ESCAPE:     return Keyboard::Key_Escape;
		case VK_SPACE:      return Keyboard::Key_Spacebar;
		case VK_TAB:        return Keyboard::Key_Tab;
		case VK_RETURN:     return Keyboard::Key_Enter;
		case VK_BACK:       return Keyboard::Key_Backspace;
		case VK_LSHIFT:     return Keyboard::Key_LeftShift;
		case VK_RSHIFT:     return Keyboard::Key_RightShift;
		case VK_LCONTROL:   return Keyboard::Key_LeftCtrl;
		case VK_RCONTROL:   return Keyboard::Key_RightCtrl;
		case VK_LMENU:      return Keyboard::Key_LeftAlt;
		case VK_RMENU:      return Keyboard::Key_RightAlt;
		case VK_LEFT:       return Keyboard::Key_LeftArrow;
		case VK_RIGHT:      return Keyboard::Key_RightArrow;
		case VK_UP:         return Keyboard::Key_UpArrow;
		case VK_DOWN:       return Keyboard::Key_DownArrow;
		case VK_INSERT:     return Keyboard::Key_Insert;
		case VK_DELETE:     return Keyboard::Key_Delete;
		case VK_HOME:       return Keyboard::Key_Home;
		case VK_END:        return Keyboard::Key_End;
		case VK_PRIOR:      return Keyboard::Key_PageUp;
		case VK_NEXT:       return Keyboard::Key_PageDown;
		case VK_ADD:        return Keyboard::Key_NumAdd;
		case VK_SUBTRACT:   return Keyboard::Key_NumSubtract;
		case VK_MULTIPLY:   return Keyboard::Key_NumMultiply;
		case VK_DIVIDE:     return Keyboard::Key_NumDivide;
		case VK_DECIMAL:    return Keyboard::Key_NumPoint;
		case VK_OEM_1:      return Keyboard::Key_Semicolon;
		case VK_OEM_PLUS:   return Keyboard::Key_Equals;
		case VK_OEM_COMMA:  return Keyboard::Key_Comma;
		case VK_OEM_MINUS:  return Keyboard::Key_Minus;
		case VK_OEM_PERIOD: return Keyboard::Key_Period;
		case VK_OEM_2:      return Keyboard::Key_Slash;
		case VK_OEM_3:      return Keyboard::Key_Grave;
		case VK_OEM_4:      return Keyboard::Key_LeftBracket;
		case VK_OEM_5:      return Keyboard::Key_Backslash;
		case VK_OEM_6:      return Keyboard::Key_RightBracket;
		case VK_OEM_7:      return Keyboard::Key_Apostrophe;
		case VK_CAPITAL:    return Keyboard::Key_CapsLock;
		case VK_NUMLOCK:    return Keyboard::Key_NumLock;
		case VK_SCROLL:     return Keyboard::Key_ScrollLock;
		case VK_PAUSE:      return Keyboard::Key_Pause;
		case VK_SNAPSHOT:   return Keyboard::Key_PrintScreen;
		case VK_LWIN:       return Keyboard::Key_LeftWin;
		case VK_RWIN:       return Keyboard::Key_RightWin;
		case VK_APPS:       return Keyboard::Key_Apps;
		default:            return Keyboard::Key_Invalid;
	}
	
}

static void syncRemixOsKeyboard(bool * keyStates) {
	
	if(!remix::isRemixDllHooked() && !remix::isRemixPreviewActive()) {
		return;
	}
	
	std::fill_n(keyStates, Keyboard::KeyCount, false);
	
	auto apply = [&](int vk) {
		const Keyboard::Key key = vkToArxKey(vk);
		if(key == Keyboard::Key_Invalid) {
			return;
		}
		if(GetAsyncKeyState(vk) & 0x8000) {
			keyStates[key - Keyboard::KeyBase] = true;
		}
	};
	
	for(int vk = 'A'; vk <= 'Z'; ++vk) {
		apply(vk);
	}
	for(int vk = '0'; vk <= '9'; ++vk) {
		apply(vk);
	}
	for(int vk = VK_F1; vk <= VK_F24; ++vk) {
		apply(vk);
	}
	for(int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) {
		apply(vk);
	}
	
	static const int kNamed[] = {
		VK_ESCAPE, VK_SPACE, VK_TAB, VK_RETURN, VK_BACK,
		VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
		VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
		VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
		VK_ADD, VK_SUBTRACT, VK_MULTIPLY, VK_DIVIDE, VK_DECIMAL,
		VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
		VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7,
		VK_CAPITAL, VK_NUMLOCK, VK_SCROLL, VK_PAUSE, VK_SNAPSHOT,
		VK_LWIN, VK_RWIN, VK_APPS
	};
	for(int vk : kNamed) {
		apply(vk);
	}
	
	// Generic Shift/Ctrl/Alt in case the left/right variants are not reported.
	if(GetAsyncKeyState(VK_SHIFT) & 0x8000) {
		if(!keyStates[Keyboard::Key_LeftShift - Keyboard::KeyBase]
		   && !keyStates[Keyboard::Key_RightShift - Keyboard::KeyBase]) {
			keyStates[Keyboard::Key_LeftShift - Keyboard::KeyBase] = true;
		}
	}
	if(GetAsyncKeyState(VK_CONTROL) & 0x8000) {
		if(!keyStates[Keyboard::Key_LeftCtrl - Keyboard::KeyBase]
		   && !keyStates[Keyboard::Key_RightCtrl - Keyboard::KeyBase]) {
			keyStates[Keyboard::Key_LeftCtrl - Keyboard::KeyBase] = true;
		}
	}
	if(GetAsyncKeyState(VK_MENU) & 0x8000) {
		if(!keyStates[Keyboard::Key_LeftAlt - Keyboard::KeyBase]
		   && !keyStates[Keyboard::Key_RightAlt - Keyboard::KeyBase]) {
			keyStates[Keyboard::Key_LeftAlt - Keyboard::KeyBase] = true;
		}
	}
}
#endif

bool SDL2InputBackend::update() {
	
	currentWheel = wheel;
	std::copy(clickCount, clickCount + std::size(clickCount), currentClickCount);
	std::copy(unclickCount, unclickCount + std::size(unclickCount), currentUnclickCount);
	
	wheel = 0;
	
	cursorRel = cursorRelAccum;
	cursorRelAccum = Vec2i(0);
	
	std::fill_n(clickCount, std::size(clickCount), 0);
	std::fill_n(unclickCount, std::size(unclickCount), 0);
	
#if ARX_HAVE_RTX_REMIX && ARX_PLATFORM == ARX_PLATFORM_WIN32
	syncRemixOsKeyboard(keyStates);
#endif
	
	return true;
}

bool SDL2InputBackend::setMouseMode(Mouse::Mode mode) {
	
	const bool relative = (mode == Mouse::Relative);
	if(SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE) == 0) {
		return true;
	}
	
	LogWarning << "Could not enable relative mouse mode: " << SDL_GetError();
	return false;
}

bool SDL2InputBackend::getAbsoluteMouseCoords(int & absX, int & absY) const {
	absX = cursorAbs.x, absY = cursorAbs.y;
	return cursorInWindow;
}

void SDL2InputBackend::setAbsoluteMouseCoords(int absX, int absY) {
	cursorAbs = Vec2i(absX, absY);
	SDL_WarpMouseInWindow(m_window->m_window, absX, absY);
}

void SDL2InputBackend::getRelativeMouseCoords(int & relX, int & relY, int & wheelDir) const {
	relX = cursorRel.x, relY = cursorRel.y, wheelDir = currentWheel;
}

void SDL2InputBackend::getMouseButtonClickCount(int buttonId, int & numClick, int & numUnClick) const {
	arx_assert(buttonId >= Mouse::ButtonBase && buttonId < Mouse::ButtonMax);
	size_t i = buttonId - Mouse::ButtonBase;
	numClick = currentClickCount[i], numUnClick = currentUnclickCount[i];
}

bool SDL2InputBackend::isKeyboardKeyPressed(int keyId) const {
	arx_assert(keyId >= Keyboard::KeyBase && keyId < Keyboard::KeyMax);
	return keyStates[keyId - Keyboard::KeyBase];
}

void SDL2InputBackend::startTextInput(const Rect & box, TextInputHandler * handler) {
	SDL_Rect rect;
	rect.x = box.left;
	rect.y = box.right;
	rect.w = box.width();
	rect.h = box.height();
	SDL_SetTextInputRect(&rect);
	if(!m_textHandler) {
		SDL_StartTextInput();
	} else if(handler != m_textHandler && !m_editText.empty()) {
		m_textHandler->editingText({ }, 0, 0);
		handler->editingText(m_editText, m_editCursorPos, m_editCursorLength);
	}
	m_textHandler = handler;
}

void SDL2InputBackend::stopTextInput() {
	if(m_textHandler) {
		if(!m_editText.empty()) {
			m_textHandler->editingText({ }, 0, 0);
			m_editText.clear();
		}
		SDL_StopTextInput();
	}
	m_textHandler = nullptr;
}

std::string SDL2InputBackend::getKeyName(Keyboard::Key key) const {
	
	arx_assert(key >= Keyboard::KeyBase && key < Keyboard::KeyMax);
	
	for(size_t i = 0; i < std::size(sdlToArxKey); i++) {
		if(sdlToArxKey[i] == key) {
			const char * name = SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(i)));
			if(name[0] == '\0') {
				name = SDL_GetScancodeName(SDL_Scancode(i));
			}
			return name;
		}
	}
	
	return std::string();
}

void SDL2InputBackend::onEvent(const SDL_Event & event) {
	
	switch(event.type) {
		
		case SDL_WINDOWEVENT: {
			if(event.window.event == SDL_WINDOWEVENT_ENTER) {
				cursorInWindow = true;
			} else if(event.window.event == SDL_WINDOWEVENT_LEAVE) {
				cursorInWindow = false;
			}
			break;
		}
		
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			SDL_Scancode key = event.key.keysym.scancode;
			if(size_t(key) < std::size(sdlToArxKey) && sdlToArxKey[size_t(key)] != Keyboard::Key_Invalid) {
				Keyboard::Key arxkey = sdlToArxKey[size_t(key)];
				if(m_textHandler && event.key.state == SDL_PRESSED) {
					KeyModifiers mod;
					mod.shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
					mod.control = (event.key.keysym.mod & KMOD_CTRL) != 0;
					mod.alt = (event.key.keysym.mod & KMOD_ALT) != 0;
					mod.gui = (event.key.keysym.mod & KMOD_GUI) != 0;
					mod.num = (event.key.keysym.mod & KMOD_NUM) != 0;
					if(m_textHandler->keyPressed(arxkey, mod)) {
						break;
					}
				}
				keyStates[arxkey - Keyboard::KeyBase] = (event.key.state == SDL_PRESSED);
			} else {
				LogWarning << "Unmapped SDL key: " << int(key) << " = " << SDL_GetScancodeName(key);
			}
			break;
		}
		
		case SDL_TEXTINPUT: {
			if(m_textHandler) {
				m_editText.clear();
				m_textHandler->newText(event.text.text);
			}
			break;
		}
		
		case SDL_TEXTEDITING: {
			if(m_textHandler) {
				// TODO do we need to convert from characters to bytes here?
				m_editText = event.edit.text;
				m_editCursorPos = glm::clamp(size_t(event.edit.start), size_t(0), m_editText.size());
				m_editCursorLength = glm::clamp(size_t(event.edit.length), size_t(0), m_editText.size() - m_editCursorPos);
				m_textHandler->editingText(m_editText, m_editCursorPos, m_editCursorLength);
			}
			break;
		}
		
		#if SDL_VERSION_ATLEAST(2, 0, 5)
		case SDL_DROPTEXT: {
			if(m_textHandler) {
				m_textHandler->droppedText(event.drop.file);
			}
			SDL_free(event.drop.file);
			break;
		}
		#endif
		
		case SDL_MOUSEMOTION: {
			cursorAbs = Vec2i(event.motion.x, event.motion.y);
			cursorRelAccum += Vec2i(event.motion.xrel, event.motion.yrel);
			cursorInWindow = true;
			break;
		}
		
		case SDL_MOUSEWHEEL: {
			wheel += event.wheel.y;
			break;
		}
		
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			Uint8 button = event.button.button;
			if(button < std::size(sdlToArxButton) && sdlToArxButton[button] != Mouse::Button_Invalid) {
				size_t i = sdlToArxButton[button] - Mouse::ButtonBase;
				if(event.button.state == SDL_PRESSED) {
					clickCount[i]++;
				} else {
					unclickCount[i]++;
				}
			} else if(button != 0) {
				LogWarning << "Unmapped SDL mouse button: " << int(button);
			}
			break;
		}
		
	}
	
}
