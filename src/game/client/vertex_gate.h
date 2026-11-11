/* Vertex login gate + watermark HUD. Header-only: put this file next to gameclient.cpp (src/game/client/). */
#ifndef GAME_CLIENT_VERTEX_GATE_H
#define GAME_CLIENT_VERTEX_GATE_H

#include <base/hash.h>
#include <base/hash_ctxt.h>
#include <base/io.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/input.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>
#include <game/client/ui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>
#include <random>
#include <vector>

// Defined once in gameclient.cpp; also used by client.cpp (Connect is blocked while locked).
extern bool g_VertexLocked;

namespace VxGate
{
// Set this to the HTTPS URL produced when deploying the Vertex license Worker.
inline constexpr const char *API_BASE_URL = "https://vertex-license-api.vertex-denis-2910.workers.dev";
inline constexpr const char *CREDENTIALS_FILE = "vertex_login.dat";
inline constexpr const char *DEVICE_FILE = "vertex_device.dat";

inline char g_aText[2][64] = {"", ""}; // 0 = login, 1 = password
inline int g_Field = 0;
inline char g_aStatus[96] = "";
inline int64_t g_Expiry = 0;
inline int64_t g_NextCheck = 0; // Local expiry timer.
inline int64_t g_NextServerCheck = 0;
inline char g_aDeviceId[65] = "";
inline char g_aClientToken[160] = "";
inline char g_aAdminToken[160] = "";
inline char g_aAdminStatus[192] = "Not signed in";
inline char g_aLicenseList[2048] = "";

enum class ERequestKind
{
	NONE, CLIENT_LOGIN, CLIENT_CHECK, ADMIN_LOGIN, ADMIN_CREATE, ADMIN_LIST, ADMIN_BAN, ADMIN_UNBAN
};
inline ERequestKind g_RequestKind = ERequestKind::NONE;
inline std::shared_ptr<IHttpRequest> g_pHttpRequest;

// This encoder intentionally writes compact hex without spaces. The engine's str_hex()
// output can contain separators, which cannot safely be parsed as two whitespace-delimited fields.
inline void EncodeHexCompact(const char *pInput, char *pOutput, int OutputSize)
{
	static const char s_aHex[] = "0123456789abcdef";
	if(OutputSize <= 0)
		return;
	const int InputLen = str_length(pInput);
	const int Count = std::min(InputLen, (OutputSize - 1) / 2);
	for(int i = 0; i < Count; ++i)
	{
		const unsigned char C = (unsigned char)pInput[i];
		pOutput[i * 2] = s_aHex[C >> 4];
		pOutput[i * 2 + 1] = s_aHex[C & 0x0f];
	}
	pOutput[Count * 2] = '\0';
}

inline int HexValue(char C)
{
	if(C >= '0' && C <= '9')
		return C - '0';
	if(C >= 'a' && C <= 'f')
		return C - 'a' + 10;
	if(C >= 'A' && C <= 'F')
		return C - 'A' + 10;
	return -1;
}

inline bool DecodeHexCompact(char *pOutput, int OutputSize, const char *pHex)
{
	if(OutputSize <= 0)
		return false;
	const int Len = str_length(pHex);
	if((Len % 2) != 0 || Len / 2 >= OutputSize)
	{
		pOutput[0] = '\0';
		return false;
	}
	for(int i = 0; i < Len; i += 2)
	{
		const int High = HexValue(pHex[i]);
		const int Low = HexValue(pHex[i + 1]);
		if(High < 0 || Low < 0)
		{
			pOutput[0] = '\0';
			return false;
		}
		const unsigned char C = (unsigned char)((High << 4) | Low);
		if(C == 0)
		{
			pOutput[0] = '\0';
			return false;
		}
		pOutput[i / 2] = (char)C;
	}
	pOutput[Len / 2] = '\0';
	return true;
}

inline void SaveCredentials(CGameClient *pGame)
{
	char aLoginHex[128];
	char aPassHex[128];
	char aBuf[300];
	EncodeHexCompact(g_aText[0], aLoginHex, sizeof(aLoginHex));
	EncodeHexCompact(g_aText[1], aPassHex, sizeof(aPassHex));

	str_format(aBuf, sizeof(aBuf), "VX1 %s %s\n", aLoginHex, aPassHex);
	IOHANDLE File = pGame->Storage()->OpenFile(CREDENTIALS_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;
	io_write(File, aBuf, str_length(aBuf));
	io_close(File);
}

inline void LoadCredentials(CGameClient *pGame)
{
	IOHANDLE File = pGame->Storage()->OpenFile(CREDENTIALS_FILE, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(!File)
		return;
	char *pContent = io_read_all_str(File);
	io_close(File);
	if(!pContent)
		return;

	char aVersion[8] = "";
	char aLoginHex[128] = "";
	char aPassHex[128] = "";
	const int Num = sscanf(pContent, "%7s %127s %127s", aVersion, aLoginHex, aPassHex);
	free(pContent);
	if(Num != 3 || str_comp(aVersion, "VX1") != 0)
		return;

	char aLogin[64] = "";
	char aPass[64] = "";
	if(!DecodeHexCompact(aLogin, sizeof(aLogin), aLoginHex) ||
		!DecodeHexCompact(aPass, sizeof(aPass), aPassHex))
		return;

	str_copy(g_aText[0], aLogin, sizeof(g_aText[0]));
	str_copy(g_aText[1], aPass, sizeof(g_aText[1]));
}

inline std::string JsonQuote(const char *pText)
{
	std::string Out = "\"";
	for(unsigned char C : std::string(pText ? pText : ""))
	{
		if(C == '"') Out += "\\\"";
		else if(C == '\\') Out += "\\\\";
		else if(C == '\n') Out += "\\n";
		else if(C == '\r') Out += "\\r";
		else if(C == '\t') Out += "\\t";
		else if(C >= 32) Out += (char)C;
	}
	Out += '"';
	return Out;
}

inline void LoadDeviceId(CGameClient *pGame)
{
	if(g_aDeviceId[0]) return;
	IOHANDLE File = pGame->Storage()->OpenFile(DEVICE_FILE, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(File)
	{
		char *pContent = io_read_all_str(File);
		io_close(File);
		if(pContent)
		{
			str_copy(g_aDeviceId, pContent, sizeof(g_aDeviceId));
			free(pContent);
		}
	}
	if(str_length(g_aDeviceId) != 32)
	{
		try
		{
			std::random_device Rd;
			str_format(g_aDeviceId, sizeof(g_aDeviceId), "%08x%08x%08x%08x",
				(unsigned)Rd(), (unsigned)Rd(), (unsigned)Rd(), (unsigned)Rd());
		}
		catch(...)
		{
			str_format(g_aDeviceId, sizeof(g_aDeviceId), "%08x%08x%08x%08x",
				(unsigned)time_get(), (unsigned)time_timestamp(), (unsigned)rand(), (unsigned)rand());
		}
		File = pGame->Storage()->OpenFile(DEVICE_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(File)
		{
			io_write(File, g_aDeviceId, str_length(g_aDeviceId));
			io_close(File);
		}
	}
}

inline bool StartApiRequest(CGameClient *pGame, const char *pPath, const std::string &Body, ERequestKind Kind)
{
	if(g_pHttpRequest)
	{
		str_copy(g_aAdminStatus, "Request already running", sizeof(g_aAdminStatus));
		return false;
	}
	if(std::string(API_BASE_URL).find("YOUR-WORKER") != std::string::npos)
	{
		str_copy(g_aStatus, "Configure API_BASE_URL in vertex_gate.h", sizeof(g_aStatus));
		str_copy(g_aAdminStatus, "Set API_BASE_URL in vertex_gate.h", sizeof(g_aAdminStatus));
		return false;
	}
	char aUrl[512];
	str_format(aUrl, sizeof(aUrl), "%s%s", API_BASE_URL, pPath);
	auto pRequest = HttpPostJson(aUrl, Body.c_str());
	pRequest->MaxResponseSize(8192);
	pRequest->LogProgress(HTTPLOG::NONE);
	g_pHttpRequest = std::shared_ptr<IHttpRequest>(std::move(pRequest));
	g_RequestKind = Kind;
	pGame->Http()->Run(g_pHttpRequest);
	return true;
}

inline std::vector<std::string> SplitTabs(const std::string &Text)
{
	std::vector<std::string> Fields;
	size_t Start = 0;
	for(size_t i = 0; i <= Text.size(); ++i)
	{
		if(i == Text.size() || Text[i] == '\t')
		{
			Fields.emplace_back(Text.substr(Start, i - Start));
			Start = i + 1;
		}
	}
	return Fields;
}

inline void AdminLogin(CGameClient *pGame, const char *pUsername, const char *pPassword)
{
	const std::string Body = "{\"username\":" + JsonQuote(pUsername) + ",\"password\":" + JsonQuote(pPassword) + "}";
	str_copy(g_aAdminStatus, "Signing in...", sizeof(g_aAdminStatus));
	StartApiRequest(pGame, "/api/admin/login", Body, ERequestKind::ADMIN_LOGIN);
}

inline void AdminCreateLicense(CGameClient *pGame, const char *pLogin, const char *pPassword, int Hours)
{
	const std::string Body = "{\"adminToken\":" + JsonQuote(g_aAdminToken) + ",\"login\":" + JsonQuote(pLogin) + ",\"password\":" + JsonQuote(pPassword) + ",\"hours\":" + std::to_string(Hours) + "}";
	str_copy(g_aAdminStatus, "Creating license...", sizeof(g_aAdminStatus));
	StartApiRequest(pGame, "/api/admin/licenses/create", Body, ERequestKind::ADMIN_CREATE);
}

inline void AdminRefreshLicenses(CGameClient *pGame)
{
	const std::string Body = "{\"adminToken\":" + JsonQuote(g_aAdminToken) + "}";
	str_copy(g_aAdminStatus, "Loading licenses...", sizeof(g_aAdminStatus));
	StartApiRequest(pGame, "/api/admin/licenses", Body, ERequestKind::ADMIN_LIST);
}

inline void AdminBan(CGameClient *pGame, const char *pLogin, const char *pReason)
{
	const std::string Body = "{\"adminToken\":" + JsonQuote(g_aAdminToken) + ",\"login\":" + JsonQuote(pLogin) + ",\"reason\":" + JsonQuote(pReason) + "}";
	str_copy(g_aAdminStatus, "Banning license...", sizeof(g_aAdminStatus));
	StartApiRequest(pGame, "/api/admin/ban", Body, ERequestKind::ADMIN_BAN);
}

inline void AdminUnban(CGameClient *pGame, const char *pLogin)
{
	const std::string Body = "{\"adminToken\":" + JsonQuote(g_aAdminToken) + ",\"login\":" + JsonQuote(pLogin) + "}";
	str_copy(g_aAdminStatus, "Unbanning license...", sizeof(g_aAdminStatus));
	StartApiRequest(pGame, "/api/admin/unban", Body, ERequestKind::ADMIN_UNBAN);
}

inline bool AdminIsLoggedIn() { return g_aAdminToken[0] != '\0'; }

inline void ProcessHttp(CGameClient *pGame)
{
	if(!g_pHttpRequest || !g_pHttpRequest->Done()) return;
	const ERequestKind Kind = g_RequestKind;
	std::string Body;
	if(g_pHttpRequest->State() == EHttpState::DONE)
	{
		unsigned char *pResult = nullptr;
		size_t ResultLength = 0;
		g_pHttpRequest->Result(&pResult, &ResultLength);
		if(pResult && ResultLength) Body.assign((const char *)pResult, ResultLength);
	}
	const bool HttpOk = g_pHttpRequest->State() == EHttpState::DONE && g_pHttpRequest->StatusCode() >= 200 && g_pHttpRequest->StatusCode() < 300;
	g_pHttpRequest.reset();
	g_RequestKind = ERequestKind::NONE;

	if(!HttpOk || Body.empty())
	{
		if(Kind == ERequestKind::CLIENT_CHECK) g_NextServerCheck = time_timestamp() + 20;
		else if(Kind == ERequestKind::CLIENT_LOGIN) str_copy(g_aStatus, "License server unavailable", sizeof(g_aStatus));
		else str_copy(g_aAdminStatus, "Server unavailable", sizeof(g_aAdminStatus));
		return;
	}

	if(Kind == ERequestKind::CLIENT_LOGIN)
	{
		const auto F = SplitTabs(Body);
		if(F.size() >= 3 && F[0] == "OK")
		{
			str_copy(g_aClientToken, F[1].c_str(), sizeof(g_aClientToken));
			g_Expiry = std::strtoll(F[2].c_str(), nullptr, 10);
			SaveCredentials(pGame);
			g_VertexLocked = false;
			g_aStatus[0] = '\0';
			g_NextServerCheck = time_timestamp() + 20;
			pGame->Input()->StopTextInput();
		}
		else
		{
			g_aStatus[0] = '\0';
			if(Body.find("BANNED") == 0) str_copy(g_aStatus, "Доступ заблокирован", sizeof(g_aStatus));
			else if(Body.find("EXPIRED") == 0) str_copy(g_aStatus, "Лицензия истекла", sizeof(g_aStatus));
			else if(Body.find("DEVICE_MISMATCH") != std::string::npos) str_copy(g_aStatus, "Лицензия привязана к другому устройству", sizeof(g_aStatus));
			else str_copy(g_aStatus, "Неверный логин или пароль", sizeof(g_aStatus));
			g_aText[1][0] = '\0';
		}
		return;
	}

	if(Kind == ERequestKind::CLIENT_CHECK)
	{
		if(Body.find("OK\t") == 0)
		{
			const auto F = SplitTabs(Body);
			if(F.size() >= 2) g_Expiry = std::strtoll(F[1].c_str(), nullptr, 10);
			g_NextServerCheck = time_timestamp() + 20;
		}
		else if(Body.find("BANNED") == 0)
		{
			g_VertexLocked = true;
			g_aClientToken[0] = '\0';
			str_copy(g_aStatus, "Доступ заблокирован администратором", sizeof(g_aStatus));
			pGame->Client()->Disconnect();
			pGame->Input()->StartTextInput();
		}
		else if(Body.find("EXPIRED") == 0 || Body.find("ERR\tSESSION_INVALID") == 0 || Body.find("ERR\tLICENSE_NOT_FOUND") == 0)
		{
			g_VertexLocked = true;
			g_aClientToken[0] = '\0';
			str_copy(g_aStatus, "Лицензия недействительна или истекла", sizeof(g_aStatus));
			pGame->Client()->Disconnect();
			pGame->Input()->StartTextInput();
		}
		else g_NextServerCheck = time_timestamp() + 20;
		return;
	}

	if(Kind == ERequestKind::ADMIN_LOGIN)
	{
		const auto F = SplitTabs(Body);
		if(F.size() >= 2 && F[0] == "OK")
		{
			str_copy(g_aAdminToken, F[1].c_str(), sizeof(g_aAdminToken));
			str_copy(g_aAdminStatus, "Admin session active", sizeof(g_aAdminStatus));
		}
		else str_copy(g_aAdminStatus, Body.c_str(), sizeof(g_aAdminStatus));
		return;
	}
	if(Kind == ERequestKind::ADMIN_LIST)
	{
		if(Body.rfind("OK\n", 0) == 0)
		{
			str_copy(g_aLicenseList, Body.c_str() + 3, sizeof(g_aLicenseList));
			str_copy(g_aAdminStatus, "License list updated", sizeof(g_aAdminStatus));
		}
		else str_copy(g_aAdminStatus, Body.c_str(), sizeof(g_aAdminStatus));
		return;
	}
	if(Body.rfind("ERR\tADMIN_AUTH_REQUIRED", 0) == 0)
	{
		g_aAdminToken[0] = '\0';
		str_copy(g_aAdminStatus, "Admin session expired; sign in again", sizeof(g_aAdminStatus));
	}
	else str_copy(g_aAdminStatus, Body.c_str(), sizeof(g_aAdminStatus));
}

inline void Load(CGameClient *pGame)
{
	g_VertexLocked = true;
	g_Expiry = 0;
	LoadCredentials(pGame);
	LoadDeviceId(pGame);
}

inline bool TryLogin(CGameClient *pGame)
{
	if(g_aText[0][0] == '\0')
	{
		str_copy(g_aStatus, "Введите логин", sizeof(g_aStatus));
		return false;
	}
	if(g_aText[1][0] == '\0')
	{
		str_copy(g_aStatus, "Введите пароль", sizeof(g_aStatus));
		return false;
	}
	LoadDeviceId(pGame);
	const std::string Body = "{\"login\":" + JsonQuote(g_aText[0]) + ",\"password\":" + JsonQuote(g_aText[1]) + ",\"deviceId\":" + JsonQuote(g_aDeviceId) + "}";
	g_aClientToken[0] = '\0';
	str_copy(g_aStatus, "Проверяем лицензию...", sizeof(g_aStatus));
	StartApiRequest(pGame, "/api/client/login", Body, ERequestKind::CLIENT_LOGIN);
	return false; // Login is asynchronous; ProcessHttp unlocks on server approval.
}

inline void FormatLeft(int64_t Seconds, char *pBuf, int Size)
{
	if(Seconds < 0)
		Seconds = 0;
	str_format(pBuf, Size, "%02d:%02d:%02d", (int)(Seconds / 3600), (int)((Seconds / 60) % 60), (int)(Seconds % 60));
}

// Call at the top of CGameClient::OnInput while g_VertexLocked.
inline void HandleInput(CGameClient *pGame, const IInput::CEvent &Event)
{
	char *pBuf = g_aText[g_Field];
	const int Cap = (int)sizeof(g_aText[0]);

	if(Event.m_Flags & IInput::FLAG_TEXT)
	{
		const int Len = str_length(pBuf);
		const int Add = str_length(Event.m_aText);
		if(Add > 0 && (unsigned char)Event.m_aText[0] >= 32 && Len + Add < Cap - 1)
			str_append(pBuf, Event.m_aText, Cap);
		return;
	}

	if(Event.m_Flags & (IInput::FLAG_PRESS | IInput::FLAG_REPEAT))
	{
		if(Event.m_Key == KEY_BACKSPACE)
		{
			const int Len = str_length(pBuf);
			if(Len > 0)
			{
				int i = Len - 1;
				while(i > 0 && (pBuf[i] & 0xC0) == 0x80)
					--i;
				pBuf[i] = '\0';
			}
		}
		else if(Event.m_Key == KEY_V && pGame->Input()->ModifierIsPressed())
		{
			const std::string Clip = pGame->Input()->GetClipboardText();
			for(const char C : Clip)
			{
				const int Len = str_length(pBuf);
				if((unsigned char)C < 32 || Len >= Cap - 2)
					continue;
				pBuf[Len] = C;
				pBuf[Len + 1] = '\0';
			}
		}
	}
	if(Event.m_Flags & IInput::FLAG_PRESS)
	{
		if(Event.m_Key == KEY_TAB || Event.m_Key == KEY_UP || Event.m_Key == KEY_DOWN)
		{
			g_Field ^= 1;
			g_aStatus[0] = '\0';
		}
		else if(Event.m_Key == KEY_RETURN || Event.m_Key == KEY_KP_ENTER)
		{
			TryLogin(pGame);
		}
	}
}

// Called in CGameClient::OnRender after all components were rendered.
inline void Render(CGameClient *pGame)
{
	ProcessHttp(pGame);
	const int64_t ServerNow = time_timestamp();
	if(!g_VertexLocked && g_aClientToken[0] && ServerNow >= g_NextServerCheck && !g_pHttpRequest)
	{
		const std::string Body = "{\"token\":" + JsonQuote(g_aClientToken) + "}";
		g_NextServerCheck = ServerNow + 20;
		StartApiRequest(pGame, "/api/client/check", Body, ERequestKind::CLIENT_CHECK);
	}
	CUi *pUi = pGame->Ui();
	IGraphics *pGfx = pGame->Graphics();
	ITextRender *pText = pGame->TextRender();
	const int64_t NowTs = time_timestamp();

	// License expired while playing: lock again and leave the server.
	if(!g_VertexLocked && NowTs >= g_NextCheck)
	{
		g_NextCheck = NowTs + 1;
		if(NowTs >= g_Expiry)
		{
			g_VertexLocked = true;
			str_copy(g_aStatus, "Лицензия истекла", sizeof(g_aStatus));
			g_aClientToken[0] = '\0';
			pGame->Client()->Disconnect();
			pGame->Input()->StartTextInput();
		}
	}

	const ColorRGBA ColWindow(0.33f, 0.32f, 0.54f, 0.78f);
	const ColorRGBA ColEdge(0.75f, 0.70f, 1.00f, 0.45f);
	const ColorRGBA ColPill(0.13f, 0.12f, 0.24f, 0.75f);
	const ColorRGBA ColAccent(0.48f, 0.42f, 1.00f, 1.00f);
	const ColorRGBA ColSoft(0.85f, 0.82f, 1.00f, 0.75f);
	const ColorRGBA ColGood(0.60f, 1.00f, 0.70f, 1.00f);
	const ColorRGBA ColBad(1.00f, 0.45f, 0.50f, 1.00f);

	if(g_VertexLocked)
	{
		pGame->Input()->StartTextInput();
		pUi->MapScreen();
		const CUIRect *pScreen = pUi->Screen();
		const float T = (float)(time_get() % (time_freq() * 600)) / (float)time_freq();

		CUIRect Full = *pScreen;
		Full.Draw(ColorRGBA(0.05f, 0.04f, 0.12f, 1.0f), IGraphics::CORNER_NONE, 0.0f);

		pGfx->TextureClear();
		pGfx->BlendNormal();
		pGfx->QuadsBegin();
		const auto Glow = [&](float Cx, float Cy, float R, float r, float g, float b) {
			for(int k = 6; k >= 1; --k)
			{
				pGfx->SetColor(r, g, b, 0.035f);
				pGfx->DrawCircle(Cx, Cy, R * (float)k / 6.0f, 48);
			}
		};
		Glow(pScreen->w * 0.25f + std::sin(T * 0.30f) * 60.0f, pScreen->h * 0.30f + std::cos(T * 0.25f) * 40.0f, 260.0f, 0.55f, 0.42f, 1.0f);
		Glow(pScreen->w * 0.78f + std::cos(T * 0.22f) * 70.0f, pScreen->h * 0.70f + std::sin(T * 0.28f) * 50.0f, 300.0f, 0.35f, 0.30f, 0.95f);
		Glow(pScreen->w * 0.55f + std::sin(T * 0.18f) * 50.0f, pScreen->h * 0.15f, 200.0f, 0.75f, 0.45f, 1.0f);
		pGfx->QuadsEnd();

		CUIRect Box;
		Box.w = 400.0f;
		Box.h = 320.0f;
		Box.x = pScreen->w / 2.0f - Box.w / 2.0f;
		Box.y = pScreen->h / 2.0f - Box.h / 2.0f;
		for(int g = 3; g >= 1; --g)
		{
			CUIRect G = Box;
			G.x -= g * 4.0f;
			G.y -= g * 4.0f;
			G.w += g * 8.0f;
			G.h += g * 8.0f;
			G.Draw(ColorRGBA(0.50f, 0.44f, 1.00f, 0.05f), IGraphics::CORNER_ALL, 22.0f + g * 4.0f);
		}
		CUIRect Inner = Box;
		Box.Draw(ColEdge, IGraphics::CORNER_ALL, 22.0f);
		Inner.Margin(1.5f, &Inner);
		Inner.Draw(ColWindow, IGraphics::CORNER_ALL, 21.0f);

		pText->TextColor(pText->DefaultTextColor());
		CUIRect Title = {Box.x, Box.y + 16.0f, Box.w, 34.0f};
		pUi->DoLabel(&Title, "Vertex", 30.0f, TEXTALIGN_MC);
		CUIRect Sub = {Box.x, Box.y + 50.0f, Box.w, 16.0f};
		pText->TextColor(ColSoft);
		pUi->DoLabel(&Sub, "доступ к клиенту", 12.0f, TEXTALIGN_MC);
		pText->TextColor(pText->DefaultTextColor());

		const bool Caret = ((time_get() * 2 / time_freq()) % 2) == 0;
		const char *apNames[2] = {"Логин", "Пароль"};
		for(int i = 0; i < 2; ++i)
		{
			const float Y = Box.y + 84.0f + i * 66.0f;
			CUIRect Label = {Box.x + 30.0f, Y, Box.w - 60.0f, 14.0f};
			pText->TextColor(ColSoft);
			pUi->DoLabel(&Label, apNames[i], 11.0f, TEXTALIGN_ML);
			pText->TextColor(pText->DefaultTextColor());

			CUIRect Row = {Box.x + 30.0f, Y + 16.0f, Box.w - 60.0f, 34.0f};
			const bool Focus = i == g_Field;
			Row.Draw(Focus ? ColAccent : ColEdge, IGraphics::CORNER_ALL, 17.0f);
			CUIRect RowIn = Row;
			RowIn.Margin(1.5f, &RowIn);
			RowIn.Draw(Focus ? ColorRGBA(0.16f, 0.14f, 0.30f, 0.95f) : ColPill, IGraphics::CORNER_ALL, 16.0f);

			char aShown[96];
			if(i == 1)
			{
				const int Len = std::min(str_length(g_aText[1]), 40);
				for(int k = 0; k < Len; ++k)
					aShown[k] = '*';
				aShown[Len] = '\0';
			}
			else
				str_copy(aShown, g_aText[0], sizeof(aShown));
			if(Focus && Caret)
				str_append(aShown, "|", sizeof(aShown));

			CUIRect Text = RowIn;
			Text.VMargin(14.0f, &Text);
			pUi->DoLabel(&Text, aShown, 14.0f, TEXTALIGN_ML);
		}

		const float Pulse = 0.5f + 0.5f * std::sin(T * 3.0f);
		CUIRect Btn = {Box.x + 30.0f, Box.y + 224.0f, Box.w - 60.0f, 40.0f};
		Btn.Draw(ColorRGBA(0.60f, 0.55f, 1.0f, 0.25f + 0.2f * Pulse), IGraphics::CORNER_ALL, 20.0f);
		CUIRect BtnIn = Btn;
		BtnIn.Margin(1.5f, &BtnIn);
		BtnIn.Draw(ColAccent, IGraphics::CORNER_ALL, 19.0f);
		pUi->DoLabel(&BtnIn, "Далее   (Enter)", 15.0f, TEXTALIGN_MC);

		CUIRect Status = {Box.x + 20.0f, Box.y + 274.0f, Box.w - 40.0f, 16.0f};
		if(g_aStatus[0])
		{
			pText->TextColor(ColBad);
			pUi->DoLabel(&Status, g_aStatus, 12.0f, TEXTALIGN_MC);
		}
		CUIRect Lic = {Box.x + 20.0f, Box.y + 294.0f, Box.w - 40.0f, 16.0f};
		char aLic[96];
		if(g_Expiry > NowTs)
		{
			char aLeft[32];
			FormatLeft(g_Expiry - NowTs, aLeft, sizeof(aLeft));
			str_format(aLic, sizeof(aLic), "Лицензия: осталось %s", aLeft);
			pText->TextColor(ColGood);
		}
		else if(g_Expiry != 0)
		{
			str_copy(aLic, "Лицензия истекла", sizeof(aLic));
			pText->TextColor(ColBad);
		}
		else
		{
			str_copy(aLic, "Срок лицензии проверяется сервером", sizeof(aLic));
			pText->TextColor(ColSoft);
		}
		pUi->DoLabel(&Lic, aLic, 12.0f, TEXTALIGN_MC);
		pText->TextColor(pText->DefaultTextColor());
		return;
	}

	// Watermark HUD: Vertex | player name | fps | server address | license time.
	pUi->MapScreen();
	const float H = 24.0f, FS = 11.0f, Gap = 12.0f;
	char aFps[32];
	str_format(aFps, sizeof(aFps), "%d fps", round_to_int(1.0f / std::max(pGame->Client()->FrameTimeAverage(), 0.0001f)));
	char aAddr[NETADDR_MAXSTRSIZE] = "offline";
	if(pGame->Client()->State() == IClient::STATE_ONLINE)
	{
		const NETADDR &Addr = pGame->Client()->ServerAddress();
		net_addr_str(&Addr, aAddr, sizeof(aAddr), true);
	}
	const int64_t Left = g_Expiry - NowTs;
	char aLeft[32];
	FormatLeft(Left, aLeft, sizeof(aLeft));
	const bool LowTime = Left < 600;
	const char *apParts[5] = {"Vertex", pGame->Client()->PlayerName(), aFps, aAddr, aLeft};
	float aWidth[5];
	float Total = H + Gap;
	for(int i = 0; i < 5; ++i)
	{
		aWidth[i] = pText->TextWidth(FS, apParts[i]);
		Total += aWidth[i] + Gap;
	}
	CUIRect Bar = {8.0f, 8.0f, Total, H};
	CUIRect BarIn = Bar;
	Bar.Draw(ColEdge, IGraphics::CORNER_ALL, H / 2.0f);
	BarIn.Margin(1.0f, &BarIn);
	BarIn.Draw(ColorRGBA(0.12f, 0.11f, 0.24f, 0.78f), IGraphics::CORNER_ALL, H / 2.0f - 1.0f);
	CUIRect Icon = {Bar.x + 3.0f, Bar.y + 3.0f, H - 6.0f, H - 6.0f};
	Icon.Draw(ColAccent, IGraphics::CORNER_ALL, Icon.h / 2.0f);
	pText->TextColor(pText->DefaultTextColor());
	pUi->DoLabel(&Icon, "V", 11.0f, TEXTALIGN_MC);
	float X = Bar.x + H + Gap;
	for(int i = 0; i < 5; ++i)
	{
		CUIRect Text = {X, Bar.y, aWidth[i] + 2.0f, H};
		if(i == 4)
			pText->TextColor(LowTime ? ColBad : ColGood);
		else if(i == 0)
			pText->TextColor(ColorRGBA(0.80f, 0.75f, 1.0f, 1.0f));
		else
			pText->TextColor(pText->DefaultTextColor());
		pUi->DoLabel(&Text, apParts[i], FS, TEXTALIGN_ML);
		X += aWidth[i] + Gap;
		if(i < 4)
		{
			CUIRect Sep = {X - Gap / 2.0f, Bar.y + 6.0f, 1.0f, H - 12.0f};
			Sep.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.18f), IGraphics::CORNER_NONE, 0.0f);
		}
	}
	pText->TextColor(pText->DefaultTextColor());
}
} // namespace VxGate

#endif // GAME_CLIENT_VERTEX_GATE_H
