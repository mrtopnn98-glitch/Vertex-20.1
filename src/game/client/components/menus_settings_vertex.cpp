/* Vertex client — settings tab v3. Glass cards UI rendered with the native DDNet UI. */
#include "menus.h"
#include <base/color.h>
#include <base/vmath.h>
#include <game/collision.h>

#include <base/time.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>
#include <game/client/vertex_gate.h>
#include <game/client/ui.h>
#include <game/localization.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

static constexpr int VERTEX_TAB_COUNT = 8;
static constexpr int VX_MAX_CARDS = 24;

void CMenus::VertexToggleClickGui()
{
	if(time_get() < m_VertexBindBlockUntil)
		return;
	if(m_VertexClickGui && m_MenuActive)
	{
		m_VertexClickGui = false;
		SetActive(false);
		return;
	}
	if(m_MenuActive)
		return; // regular menu is open, leave it alone
	m_VertexClickGui = true;
	SetActive(true);
}

// reset the flag when the menu was closed some other way (Esc etc.)
void CMenus::VertexClickGuiTick()
{
	if(m_VertexClickGui && !m_MenuActive)
		m_VertexClickGui = false;
}

// Click GUI overlay: only the Vertex window floats over the game, without the normal menu bar.
void CMenus::RenderVertexClickGui(CUIRect Screen)
{
	if(!m_VertexBindListening && Input()->KeyPress(KEY_ESCAPE))
	{
		m_VertexClickGui = false;
		SetActive(false);
		return;
	}

	g_Config.m_VxClickGuiScale = std::clamp(g_Config.m_VxClickGuiScale, 40, 120);
	const float S = g_Config.m_VxClickGuiScale / 100.0f;
	const float W = std::min(900.0f * S, Screen.w - 10.0f);
	const float H = std::min(520.0f * S, Screen.h - 10.0f);

	static bool s_Placed = false;
	if(!s_Placed)
	{
		s_Placed = true;
		m_VertexGuiX = Screen.x + (Screen.w - W) / 2.0f;
		m_VertexGuiY = Screen.y + (Screen.h - H) / 2.0f;
	}
	m_VertexGuiX = std::max(Screen.x, std::min(m_VertexGuiX, Screen.x + Screen.w - W));
	m_VertexGuiY = std::max(Screen.y, std::min(m_VertexGuiY, Screen.y + Screen.h - H));

	CUIRect Area;
	Area.x = m_VertexGuiX;
	Area.y = m_VertexGuiY;
	Area.w = W;
	Area.h = H;
	m_VertexOverlay = true;
	RenderSettingsVertex(Area);
	m_VertexOverlay = false;

	// small plus-shaped cursor of the Click GUI (the native arrow must be skipped in CMenus::OnRender while m_VertexClickGui is true)
	{
		const float Mx = Ui()->MouseX(), My = Ui()->MouseY();
		const float L = 4.5f, Th = 1.1f, O = 0.6f; // arm length, thickness, outline
		CUIRect H1, V1;
		H1.x = Mx - L - O;
		H1.y = My - Th / 2.0f - O;
		H1.w = 2.0f * L + 2.0f * O;
		H1.h = Th + 2.0f * O;
		V1.x = Mx - Th / 2.0f - O;
		V1.y = My - L - O;
		V1.w = Th + 2.0f * O;
		V1.h = 2.0f * L + 2.0f * O;
		H1.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.55f), IGraphics::CORNER_NONE, 0.0f);
		V1.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.55f), IGraphics::CORNER_NONE, 0.0f);
		H1.x += O;
		H1.w -= 2.0f * O;
		H1.y += O;
		H1.h -= 2.0f * O;
		V1.y += O;
		V1.h -= 2.0f * O;
		V1.x += O;
		V1.w -= 2.0f * O;
		H1.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.95f), IGraphics::CORNER_NONE, 0.0f);
		V1.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.95f), IGraphics::CORNER_NONE, 0.0f);
	}
}

void CMenus::RenderSettingsVertex(CUIRect MainView)
{
	const float S = m_VertexOverlay ? std::clamp(g_Config.m_VxClickGuiScale, 40, 120) / 100.0f : 1.0f;
	const float F = S * 0.9f * std::clamp(g_Config.m_VxUiFont, 80, 130) / 100.0f; // text scale

	// 0 Aim, 1 Avoid, 2 Misc, 3 Visual, 4 Fly, 5 Settings, 6 Configs, 7 Admin
	static const char *s_apTabNames[VERTEX_TAB_COUNT] = {
		"Наведение", "Avoid", "Разное", "Визуал", "Полёт", "Настройки", "Конфиги", "Админ"};
	static int s_Tab = 0;
static char s_aSearchBuf[32] = "";

	// ---------- colour helpers ----------
	const auto Mix = [](const ColorRGBA &A, const ColorRGBA &B, float T) {
		return ColorRGBA(A.r + (B.r - A.r) * T, A.g + (B.g - A.g) * T, A.b + (B.b - A.b) * T, A.a + (B.a - A.a) * T);
	};
	const auto WithA = [](ColorRGBA C, float A) {
		C.a = A;
		return C;
	};

	// ---------- appearance (all from config) ----------
	const float Glass = std::clamp(g_Config.m_VxUiGlass, 10, 100) / 100.0f;
	const float Frost = std::clamp(g_Config.m_VxUiFrost, 0, 100) / 100.0f;
	const float Glow = std::clamp(g_Config.m_VxUiGlow, 0, 100) / 100.0f;
	const float EdgeK = std::clamp(g_Config.m_VxUiEdge, 0, 100) / 100.0f;
	const float CardA = std::clamp(g_Config.m_VxUiCardAlpha, 10, 100) / 100.0f;
	const float Rnd = std::clamp(g_Config.m_VxUiRadius, 4, 32) * S;
	const float CardR = std::max(5.0f * S, Rnd * 0.6f);

	ColorRGBA Accent = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_VxUiAccent));
	if(g_Config.m_VxUiRainbow)
	{
		const float Hue = std::fmod((float)((double)time_get() / (double)time_freq()) * 0.08f, 1.0f);
		Accent = color_cast<ColorRGBA>(ColorHSLA(Hue, 0.9f, 0.68f));
	}
	Accent.a = 1.0f;
	const ColorRGBA Tint = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_VxUiTint));
	const ColorRGBA White(1.0f, 1.0f, 1.0f, 1.0f);
	const ColorRGBA Black(0.0f, 0.0f, 0.0f, 1.0f);

	// bright glass (white preset): dark text and lighter controls
	const bool Light = ColorHSLA(g_Config.m_VxUiTint).l > 0.6f;

	const ColorRGBA ColWindow = WithA(Tint, 0.22f + 0.70f * Glass);
	const ColorRGBA ColRail = WithA(Mix(Tint, Black, 0.08f), 0.22f + 0.70f * Glass);
	const ColorRGBA ColEdge = WithA(Mix(Tint, White, 0.65f), EdgeK);
	const ColorRGBA ColBtn = WithA(Mix(Tint, White, 0.25f), 0.70f);
	const ColorRGBA ColBtnHot = WithA(Mix(Tint, White, 0.42f), 0.85f);
	const ColorRGBA ColPill = Light ? WithA(Mix(Tint, Black, 0.14f), 0.80f) : WithA(Mix(Tint, Black, 0.65f), 0.70f);
	const ColorRGBA ColPillHot = Light ? WithA(Mix(Tint, Black, 0.24f), 0.90f) : WithA(Mix(Tint, Black, 0.45f), 0.85f);
	const ColorRGBA ColField = Light ? WithA(Mix(Tint, Black, 0.10f), 1.0f) : WithA(Mix(Tint, Black, 0.60f), 1.0f);
	const ColorRGBA ColCard = Light ? WithA(Mix(Tint, Black, 0.07f), CardA * 0.9f) : WithA(Mix(Tint, Black, 0.35f), CardA * 0.9f);
	const ColorRGBA ColText = Light ? ColorRGBA(0.08f, 0.08f, 0.12f, 0.96f) : ColorRGBA(0.97f, 0.96f, 1.0f, 0.96f);
	const ColorRGBA ColDim = Light ? ColorRGBA(0.12f, 0.12f, 0.20f, 0.60f) : ColorRGBA(0.88f, 0.86f, 1.0f, 0.55f);
	const ColorRGBA ColKnob(0.97f, 0.96f, 1.0f, 0.97f);
	const ColorRGBA ColOnPill = ColorRGBA(0.97f, 0.96f, 1.0f, 0.97f); // text on accent / dark pills
	(void)ColBtn;
	(void)ColBtnHot;

	// ---------- animation ----------
	static std::unordered_map<uint64_t, float> s_AnimMap;
	const auto Anim = [&](const void *pId, int Slot, float Target) {
		float &V = s_AnimMap.try_emplace((uint64_t)(uintptr_t)pId * 4u + (uint64_t)Slot, Target).first->second;
		const int Speed = std::clamp(g_Config.m_VxUiAnim, 0, 100);
		if(Speed == 0)
			V = Target;
		else
			V += (Target - V) * std::min(1.0f, Client()->RenderFrameTime() * (6.0f + Speed * 0.14f));
		return V;
	};

	// ---------- drawing primitives ----------
	const auto Label = [&](const CUIRect *pRect, const char *pText, float Size, int Align, const ColorRGBA &Col) {
		TextRender()->TextColor(Col);
		Ui()->DoLabel(pRect, pText, Size, Align);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	};

	// frosted glass panel: gradient border, gradient body, top sheen
	const auto DrawGlass = [&](const CUIRect &R, float Rounding, const ColorRGBA &Fill, const ColorRGBA &Edge) {
		const ColorRGBA EdgeTop = Edge;
		const ColorRGBA EdgeBot = WithA(Edge, Edge.a * 0.40f);
		R.Draw4(EdgeTop, EdgeTop, EdgeBot, EdgeBot, IGraphics::CORNER_ALL, Rounding);
		CUIRect In = R;
		In.Margin(1.0f * S, &In);
		const ColorRGBA BodyTop = WithA(Mix(Fill, White, Frost * 0.14f), Fill.a);
		In.Draw4(BodyTop, BodyTop, Fill, Fill, IGraphics::CORNER_ALL, std::max(0.0f, Rounding - 1.0f * S));
		if(Frost > 0.01f)
		{
			CUIRect Sheen = In;
			Sheen.h = In.h * 0.45f;
			const ColorRGBA S0(1.0f, 1.0f, 1.0f, Frost * 0.10f);
			const ColorRGBA S1(1.0f, 1.0f, 1.0f, 0.0f);
			Sheen.Draw4(S0, S0, S1, S1, IGraphics::CORNER_T, std::max(0.0f, Rounding - 1.0f * S));
		}
	};

	const auto DoToggle = [&](int *pValue, const CUIRect &Rect) {
		const int Clicked = Ui()->DoButtonLogic(pValue, 0, &Rect, BUTTONFLAG_LEFT);
		const bool Hot = Ui()->HotItem() == pValue;
		const float T = Anim(pValue, 0, *pValue ? 1.0f : 0.0f);
		const float Hv = Anim(pValue, 1, Hot ? 1.0f : 0.0f);

		CUIRect Track = Rect;
		Track.HMargin(std::max(0.0f, (Track.h - 16.0f * S) / 2.0f), &Track);
		if(T > 0.01f && Glow > 0.01f)
		{
			CUIRect G = Track;
			G.Margin(-2.5f * S, &G);
			G.Draw(WithA(Accent, 0.30f * T * Glow), IGraphics::CORNER_ALL, G.h / 2.0f);
		}
		Track.Draw(Mix(Mix(ColPill, ColPillHot, Hv), Accent, T), IGraphics::CORNER_ALL, Track.h / 2.0f);

		CUIRect Knob;
		Knob.h = Track.h - 4.0f * S;
		Knob.w = Knob.h;
		Knob.y = Track.y + 2.0f * S;
		const float X0 = Track.x + 2.0f * S;
		const float X1 = Track.x + Track.w - Knob.w - 2.0f * S;
		Knob.x = X0 + (X1 - X0) * T;
		Knob.Draw(ColKnob, IGraphics::CORNER_ALL, Knob.h / 2.0f);

		if(Clicked)
			*pValue = *pValue ? 0 : 1;
	};

	static unsigned int *s_pOpenColor = nullptr; // colour whose inline picker is open
	static unsigned int *s_pHsvFor = nullptr; // colour the HSV state below belongs to
	static float s_aHsv[3] = {0.0f, 0.0f, 1.0f};

	// round colour dot; click opens the inline picker (no popup needed)
	const auto DoSwatch = [&](unsigned int *pColor, const CUIRect &Rect) {
		const int Clicked = Ui()->DoButtonLogic(pColor, 0, &Rect, BUTTONFLAG_LEFT);
		const float Hv = Anim(pColor, 1, Ui()->HotItem() == pColor ? 1.0f : 0.0f);
		const bool Open = s_pOpenColor == pColor;
		const float D = std::min(Rect.h, 16.0f * S);
		CUIRect Ring;
		Ring.w = D + 4.0f * S;
		Ring.h = Ring.w;
		Ring.x = Rect.x + (Rect.w - Ring.w) / 2.0f;
		Ring.y = Rect.y + (Rect.h - Ring.h) / 2.0f;
		Ring.Draw(Open ? Accent : WithA(Light ? Black : White, 0.30f + 0.35f * Hv), IGraphics::CORNER_ALL, Ring.w / 2.0f);
		CUIRect Dot = Ring;
		Dot.Margin(1.5f * S, &Dot);
		ColorRGBA C = color_cast<ColorRGBA>(ColorHSLA(*pColor));
		C.a = 1.0f;
		Dot.Draw(C, IGraphics::CORNER_ALL, Dot.w / 2.0f);
		if(Clicked)
		{
			s_pOpenColor = Open ? nullptr : pColor;
			s_pHsvFor = nullptr;
		}
	};

	// pill button, returns true when clicked
	const auto DoPill = [&](const void *pId, const char *pText, bool Active, const CUIRect &Rect) {
		const int Clicked = Ui()->DoButtonLogic(pId, 0, &Rect, BUTTONFLAG_LEFT);
		const bool Hot = Ui()->HotItem() == pId;
		const float A = Anim(pId, 0, Active ? 1.0f : 0.0f);
		const float Hv = Anim(pId, 1, Hot ? 1.0f : 0.0f);
		CUIRect R = Rect;
		R.Draw(Mix(Mix(ColPill, ColPillHot, Hv), Accent, A), IGraphics::CORNER_ALL, R.h / 2.0f);
		Label(&R, pText, 12.0f * F, TEXTALIGN_MC, (A > 0.5f || !Light) ? (A > 0.5f && Light ? ColorRGBA(0.08f, 0.08f, 0.12f, 0.96f) : ColOnPill) : ColText);
		return Clicked != 0;
	};

	// text field: native edit box handles input, the look is drawn by us
	const auto DoField = [&](CLineInput *pInput, const CUIRect &Rect, const char *pHint, bool Hidden, float Rounding) {
		Rect.Draw(WithA(ColEdge, ColEdge.a * 0.7f), IGraphics::CORNER_ALL, Rounding);
		CUIRect In = Rect;
		In.Margin(1.0f * S, &In);
		CUIRect Edit = In;
		Ui()->DoEditBox(pInput, &Edit, 11.0f * F);
		In.Draw(ColField, IGraphics::CORNER_ALL, std::max(0.0f, Rounding - 1.0f * S));

		CUIRect TextRect = In;
		TextRect.VMargin(8.0f * S, &TextRect);
		const bool Active = pInput->IsActive();
		char aShown[160];
		if(pInput->IsEmpty() && !Active)
		{
			Label(&TextRect, pHint, 11.5f * F, TEXTALIGN_ML, ColDim);
			return;
		}
		const bool Caret = Active && ((time_get() * 2 / time_freq()) % 2 == 0);
		if(Hidden)
		{
			int n = std::min((int)str_length(pInput->GetString()), 60);
			int i = 0;
			for(; i < n; i++)
				aShown[i] = '*';
			aShown[i] = '\0';
			if(Caret)
				str_append(aShown, "|", sizeof(aShown));
		}
		else
			str_format(aShown, sizeof(aShown), "%s%s", pInput->GetString(), Caret ? "|" : "");
		Label(&TextRect, aShown, 11.5f * F, TEXTALIGN_ML, ColText);
	};

	// thin slider with value readout (flat track + white knob, like the reference client)
	const auto DoVertexSlider = [&](int *pValue, int Min, int Max, const CUIRect &Rect, const char *pLabel, const char *pSuffix) {
		CUIRect Top, Bot;
		Rect.HSplitTop(18.0f * S, &Top, &Bot);

		char aBuf[32];
		str_format(aBuf, sizeof(aBuf), "%d%s", *pValue, pSuffix);
		Label(&Top, pLabel, 12.0f * F, TEXTALIGN_ML, ColText);
		Label(&Top, aBuf, 12.0f * F, TEXTALIGN_MR, ColDim);

		CUIRect Track = Bot;
		Track.VMargin(5.0f * S, &Track);
		Track.HMargin(std::max(0.0f, (Bot.h - 4.0f * S) / 2.0f), &Track);

		Ui()->DoButtonLogic(pValue, 0, &Rect, BUTTONFLAG_LEFT);
		if(Ui()->CheckActiveItem(pValue) && Track.w > 1.0f)
		{
			const float Ratio = std::clamp((Ui()->MouseX() - Track.x) / Track.w, 0.0f, 1.0f);
			*pValue = Min + (int)std::round(Ratio * (float)(Max - Min));
		}
		*pValue = std::clamp(*pValue, Min, Max);
		const bool Hot = Ui()->HotItem() == pValue || Ui()->CheckActiveItem(pValue);
		const float Hv = Anim(pValue, 0, Hot ? 1.0f : 0.0f);

		const float Ratio = Max > Min ? (float)(*pValue - Min) / (float)(Max - Min) : 0.0f;
		Track.Draw(Light ? ColorRGBA(0.0f, 0.0f, 0.0f, 0.18f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.14f), IGraphics::CORNER_ALL, 2.0f * S);
		CUIRect Fill = Track;
		Fill.w = Track.w * Ratio;
		Fill.Draw4(Mix(Accent, Black, 0.35f), Accent, Mix(Accent, Black, 0.35f), Accent, IGraphics::CORNER_ALL, 2.0f * S);

		CUIRect Knob;
		Knob.w = (9.0f + 2.0f * Hv) * S;
		Knob.h = Knob.w;
		Knob.x = Track.x + Track.w * Ratio - Knob.w / 2.0f;
		Knob.y = Track.y + Track.h / 2.0f - Knob.h / 2.0f;
		CUIRect Halo = Knob;
		Halo.Margin(-3.0f * S, &Halo);
		Halo.Draw(WithA(Accent, (0.08f + 0.20f * Hv) * (0.4f + Glow)), IGraphics::CORNER_ALL, Halo.w / 2.0f);
		Knob.Draw(ColKnob, IGraphics::CORNER_ALL, Knob.w / 2.0f);
	};

	// thin line icons (outline style, no background cut-outs)
	const auto Poly = [&](const vec2 *pPts, int Num, float Th, const ColorRGBA &Col) {
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(Col);
		for(int i = 0; i + 1 < Num; i++)
		{
			const float dx = pPts[i + 1].x - pPts[i].x, dy = pPts[i + 1].y - pPts[i].y;
			const float Len = std::sqrt(dx * dx + dy * dy) + Th * 0.6f;
			const float mx = (pPts[i].x + pPts[i + 1].x) / 2.0f, my = (pPts[i].y + pPts[i + 1].y) / 2.0f;
			Graphics()->QuadsSetRotation(std::atan2(dy, dx));
			IGraphics::CQuadItem Q(mx - Len / 2.0f, my - Th / 2.0f, Len, Th);
			Graphics()->QuadsDrawTL(&Q, 1);
		}
		Graphics()->QuadsSetRotation(0.0f);
		Graphics()->QuadsEnd();
	};
	const auto DrawIcon = [&](int Tab, const CUIRect &C, const ColorRGBA &Col) {
		const float cx = C.x + C.w / 2.0f;
		const float cy = C.y + C.h / 2.0f;
		const float T = std::max(1.3f, 1.6f * S);
		const auto P = [&](float x, float y) { return vec2(cx + x * S, cy + y * S); };
		const auto Ring = [&](float x, float y, float r) {
			vec2 a[25];
			for(int i = 0; i <= 24; i++)
				a[i] = P(x + r * std::cos(i * 6.2831853f / 24.0f), y + r * std::sin(i * 6.2831853f / 24.0f));
			Poly(a, 25, T, Col);
		};
		const auto Line = [&](float x0, float y0, float x1, float y1) {
			const vec2 a[2] = {P(x0, y0), P(x1, y1)};
			Poly(a, 2, T, Col);
		};
		const auto Rect = [&](float x, float y, float w, float h) {
			const vec2 a[5] = {P(x, y), P(x + w, y), P(x + w, y + h), P(x, y + h), P(x, y)};
			Poly(a, 5, T, Col);
		};
		switch(Tab)
		{
		case 0: // crosshair
			Ring(0, 0, 7.5f);
			Line(0, -12, 0, -4);
			Line(0, 4, 0, 12);
			Line(-12, 0, -4, 0);
			Line(4, 0, 12, 0);
			break;
		case 1: // person
		{
			Ring(0, -5, 4.0f);
			vec2 b[13];
			for(int i = 0; i <= 12; i++)
			{
				const float a = 3.14159265f * i / 12.0f;
				b[i] = P(-9.0f * std::cos(a), 10.0f - 8.0f * std::sin(a));
			}
			Poly(b, 13, T, Col);
			break;
		}
		case 2: // grid
			Rect(-9, -9, 7, 7);
			Rect(2, -9, 7, 7);
			Rect(-9, 2, 7, 7);
			Rect(2, 2, 7, 7);
			break;
		case 3: // eye
		{
			vec2 up[17], lo[17];
			for(int i = 0; i <= 16; i++)
			{
				const float t = i / 16.0f;
				const float w = std::sin(t * 3.14159265f);
				up[i] = P(-11.0f + 22.0f * t, -6.5f * w);
				lo[i] = P(-11.0f + 22.0f * t, 6.5f * w);
			}
			Poly(up, 17, T, Col);
			Poly(lo, 17, T, Col);
			Ring(0, 0, 3.2f);
			break;
		}
		case 4: // fly >>
		{
			const vec2 a[3] = {P(-8, -6), P(-2, 0), P(-8, 6)};
			const vec2 b[3] = {P(1, -6), P(7, 0), P(1, 6)};
			Poly(a, 3, T, Col);
			Poly(b, 3, T, Col);
			break;
		}
		case 5: // sliders
			Line(-10, -7, 10, -7);
			Line(-10, 0, 10, 0);
			Line(-10, 7, 10, 7);
			Ring(-3, -7, 2.6f);
			Ring(4, 0, 2.6f);
			Ring(-1, 7, 2.6f);
			break;
		case 6: // file
			Rect(-7, -10, 14, 20);
			Line(-4, -3, 4, -3);
			Line(-4, 2, 4, 2);
			break;
		case 7: // lock
		{
			Rect(-8, -2, 16, 12);
			vec2 b[13];
			for(int i = 0; i <= 12; i++)
			{
				const float a = 3.14159265f * i / 12.0f;
				b[i] = P(6.0f * std::cos(a), -2.0f - 7.0f * std::sin(a));
			}
			Poly(b, 13, T, Col);
			break;
		}
		case 8: // link (two chain loops), small icon next to the card switch
			Rect(-6.5f, -2.5f, 7.0f, 5.0f);
			Rect(-0.5f, -2.5f, 7.0f, 5.0f);
			break;
		}
	};

	// ---------- layout ----------
	CUIRect Rail, Window, SearchArea;
	MainView.VSplitLeft(54.0f * S, &Rail, &Window);
	Window.VSplitLeft(12.0f * S, nullptr, &Window);
	Window.HSplitBottom(36.0f * S, &Window, &SearchArea);
	SearchArea.HSplitTop(8.0f * S, nullptr, &SearchArea);

	// rail with round tab buttons
	{
		const float Btn = 38.0f * S;
		const float RailGap = 8.0f * S;
		const float RailH = std::min(Rail.h, VERTEX_TAB_COUNT * Btn + (VERTEX_TAB_COUNT - 1) * RailGap + 24.0f * S);
		CUIRect RailBox = Rail;
		RailBox.h = RailH;
		DrawGlass(RailBox, std::min(24.0f * S, Rnd + 4.0f * S), ColRail, ColEdge);

		static CButtonContainer s_aTabButtons[VERTEX_TAB_COUNT];
		static const int s_aRailOrder[VERTEX_TAB_COUNT] = {0, 4, 3, 1, 2, 5, 6, 7}; // aim, fly, visual, avoid, misc, settings, configs, admin
		float y = RailBox.y + 12.0f * S;
		for(int Pos = 0; Pos < VERTEX_TAB_COUNT; Pos++)
		{
			const int i = s_aRailOrder[Pos];
			CUIRect Circle;
			Circle.x = RailBox.x + (RailBox.w - Btn) / 2.0f;
			Circle.y = y;
			Circle.w = Btn;
			Circle.h = Btn;
			y += Btn + RailGap;

			const int Clicked = Ui()->DoButtonLogic(&s_aTabButtons[i], 0, &Circle, BUTTONFLAG_LEFT);
			const bool Hot = Ui()->HotItem() == &s_aTabButtons[i];
			const float A = Anim(&s_aTabButtons[i], 0, s_Tab == i ? 1.0f : 0.0f);
			const float Hv = Anim(&s_aTabButtons[i], 1, Hot ? 1.0f : 0.0f);
			if(A > 0.01f && Glow > 0.01f)
			{
				CUIRect Ring = Circle;
				Ring.Margin(-3.0f * S, &Ring);
				Ring.Draw(WithA(Accent, 0.22f * A * Glow), IGraphics::CORNER_ALL, Ring.w / 2.0f);
			}
			const ColorRGBA Bg = Mix(WithA(Light ? Black : White, 0.10f * Hv), Mix(Accent, White, 0.55f), A);
			if(Bg.a > 0.01f)
				Circle.Draw(Bg, IGraphics::CORNER_ALL, Btn / 2.0f);
			DrawIcon(i, Circle, Mix(WithA(ColText, 0.85f), ColorRGBA(0.10f, 0.10f, 0.14f, 1.0f), A));
			if(Clicked)
			{
				s_Tab = i;
						 s_aSearchBuf[0] = '\0';
				s_pOpenColor = nullptr;
			}
		}
	}

	// window: soft accent glow, glass body
	if(Glow > 0.01f)
	{
		for(int g = 4; g >= 1; g--)
		{
			CUIRect G = Window;
			G.x -= g * 3.0f * S;
			G.y -= g * 3.0f * S;
			G.w += g * 6.0f * S;
			G.h += g * 6.0f * S;
			G.Draw(WithA(Accent, 0.07f * Glow), IGraphics::CORNER_ALL, Rnd + g * 3.0f * S);
		}
	}
	DrawGlass(Window, Rnd, ColWindow, ColEdge);

	// frosted backdrop: slow drifting soft blobs inside the window (cheap fake of a blur / cloud shader)
	if(Frost > 0.01f)
	{
		Ui()->ClipEnable(&Window);
		const float Tm = (float)((double)time_get() / (double)time_freq());
		for(int b = 0; b < 7; b++)
		{
			const float Bx = Window.x + Window.w * (0.5f + 0.45f * std::sin(Tm * 0.11f * (1.0f + b * 0.37f) + b * 1.7f));
			const float By = Window.y + Window.h * (0.5f + 0.45f * std::cos(Tm * 0.09f * (1.0f + b * 0.29f) + b * 2.3f));
			const float Rd0 = Window.h * (0.18f + 0.04f * (b % 3));
			for(int k = 0; k < 3; k++)
			{
				const float Rd = Rd0 * (1.0f + 0.35f * k);
				CUIRect B;
				B.x = Bx - Rd;
				B.y = By - Rd;
				B.w = Rd * 2.0f;
				B.h = Rd * 2.0f;
				const ColorRGBA BC = (b % 2) ? Accent : (Light ? Black : White);
				B.Draw(WithA(BC, 0.030f * Frost * (Light ? 0.6f : 1.0f)), IGraphics::CORNER_ALL, Rd);
			}
		}
		Ui()->ClipDisable();
	}

	CUIRect Inner = Window;
	Inner.Margin(14.0f * S, &Inner);

	// title row
	CUIRect Title;
	Inner.HSplitTop(22.0f * S, &Title, &Inner);
	{
		CUIRect Bar = Title;
		if(m_VertexOverlay)
		{
			CUIRect BtnPlus, BtnMinus;
			Bar.VSplitRight(22.0f * S, &Bar, &BtnPlus);
			Bar.VSplitRight(4.0f * S, &Bar, nullptr);
			Bar.VSplitRight(22.0f * S, &Bar, &BtnMinus);
			static CButtonContainer s_ScaleMinus, s_ScalePlus;
			if(DoPill(&s_ScaleMinus, "-", false, BtnMinus))
				g_Config.m_VxClickGuiScale = std::max(40, g_Config.m_VxClickGuiScale - 10);
			if(DoPill(&s_ScalePlus, "+", false, BtnPlus))
				g_Config.m_VxClickGuiScale = std::min(120, g_Config.m_VxClickGuiScale + 10);

			static CButtonContainer s_DragBar;
			static bool s_Dragging = false;
			static vec2 s_Grab = vec2(0.0f, 0.0f);
			Ui()->DoButtonLogic(&s_DragBar, 0, &Bar, BUTTONFLAG_LEFT);
			if(Ui()->CheckActiveItem(&s_DragBar))
			{
				if(!s_Dragging)
				{
					s_Dragging = true;
					s_Grab = vec2(Ui()->MouseX() - MainView.x, Ui()->MouseY() - MainView.y);
				}
				m_VertexGuiX = Ui()->MouseX() - s_Grab.x;
				m_VertexGuiY = Ui()->MouseY() - s_Grab.y;
			}
			else
				s_Dragging = false;
		}
		CUIRect Dot = Bar;
		Dot.w = 8.0f * S;
		Dot.h = 8.0f * S;
		Dot.y = Bar.y + (Bar.h - Dot.h) / 2.0f;
		Dot.Draw(Accent, IGraphics::CORNER_ALL, Dot.w / 2.0f);
		CUIRect Txt = Bar;
		Txt.VSplitLeft(16.0f * S, nullptr, &Txt);
		char aTitle[64];
		str_format(aTitle, sizeof(aTitle), "Vertex  ·  %s", s_apTabNames[s_Tab]);
		Label(&Txt, aTitle, 13.0f * F, TEXTALIGN_ML, ColText);
	}
	Inner.HSplitTop(6.0f * S, nullptr, &Inner);

	// search pill (filters the cards of the current tab)
	
	static CLineInput s_SearchInput(s_aSearchBuf, sizeof(s_aSearchBuf));
	{
		CUIRect Pill = SearchArea;
		const float PW = std::min(320.0f * S, SearchArea.w);
		Pill.x += (SearchArea.w - PW) / 2.0f;
		Pill.w = PW;
		DoField(&s_SearchInput, Pill, "Поиск модулей...", false, Pill.h / 2.0f);
	}
	const char *pSearch = s_SearchInput.GetString();
	const auto Show = [&](const char *pName) {
		return pSearch[0] == '\0' || str_find_nocase(pName, pSearch) != nullptr;
	};
	const auto ShowK = [&](const char *pRu, const char *pEn) { return Show(pRu) || Show(pEn); };

	// ---------- scrolling content ----------
	static float s_aScroll[VERTEX_TAB_COUNT] = {};
	static float s_aContentHeight[VERTEX_TAB_COUNT] = {};
	static float s_aCardH[VERTEX_TAB_COUNT][VX_MAX_CARDS] = {};
	float &Scroll = s_aScroll[s_Tab];
	float &ContentHeight = s_aContentHeight[s_Tab];

	CUIRect ScrollBar, Clip;
	Inner.VSplitRight(15.0f * S, &Clip, &ScrollBar);
	Clip.VSplitRight(8.0f * S, &Clip, nullptr);

	const float MaxScroll = std::max(0.0f, ContentHeight - Clip.h);
	if(Ui()->MouseInside(&Clip))
	{
		if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
			Scroll -= 40.0f * S;
		if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
			Scroll += 40.0f * S;
	}
	Scroll = std::clamp(Scroll, 0.0f, MaxScroll);
	float ScrollRatio = MaxScroll > 0.0f ? Scroll / MaxScroll : 0.0f;
	ScrollRatio = Ui()->DoScrollbarV(&Scroll, &ScrollBar, ScrollRatio);
	Scroll = std::clamp(ScrollRatio, 0.0f, 1.0f) * MaxScroll;

	CUIRect Content = Clip;
	Content.y -= Scroll;
	const float ContentTop = Content.y;
	const float ColGap = 10.0f * S;
	CUIRect Col[2];
	Col[0] = Content;
	Col[0].w = (Content.w - ColGap) / 2.0f;
	Col[1] = Col[0];
	Col[1].x += Col[0].w + ColGap;
	int CurCol = 0;
	Ui()->ClipEnable(&Clip);

	const float RowH = 22.0f * S;
	const float HeadH = 26.0f * S; // card header (pill) height
	const float Gap = 5.0f * S;
	const float CardPad = 12.0f * S;
	const float CardGap = 9.0f * S;
	bool InCard = false;
	int CardIdx = 0;
	float CardStartY = 0.0f;
	float *pCardH = nullptr;

	const auto Take = [&](float H, float After) {
		CUIRect R;
		Content.HSplitTop(H, &R, &Content);
		Content.HSplitTop(After, nullptr, &Content);
		if(InCard)
			R.VMargin(CardPad, &R);
		return R;
	};

	// ----- rows (inside a card) -----
	const auto Caption = [&](const char *pText) {
		CUIRect Row = Take(16.0f * S, 3.0f * S);
		Label(&Row, pText, 10.5f * F, TEXTALIGN_ML, ColDim);
	};
	const auto Note = [&](const char *pText) {
		CUIRect Row = Take(RowH, Gap);
		Label(&Row, pText, 11.5f * F, TEXTALIGN_ML, ColDim);
	};
	const auto Rule = [&]() {
		CUIRect D = Take(1.0f * S, 7.0f * S);
		D.Draw(Light ? ColorRGBA(0.0f, 0.0f, 0.0f, 0.12f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.09f), IGraphics::CORNER_NONE, 0.0f);
	};
	const auto HsvToRgb = [](float h, float sat, float v) {
		h -= std::floor(h);
		const float f6 = h * 6.0f;
		const int i = (int)f6 % 6;
		const float f = f6 - std::floor(f6);
		const float p = v * (1.0f - sat), q = v * (1.0f - f * sat), t = v * (1.0f - (1.0f - f) * sat);
		switch(i)
		{
		case 0: return ColorRGBA(v, t, p, 1.0f);
		case 1: return ColorRGBA(q, v, p, 1.0f);
		case 2: return ColorRGBA(p, v, t, 1.0f);
		case 3: return ColorRGBA(p, q, v, 1.0f);
		case 4: return ColorRGBA(t, p, v, 1.0f);
		default: return ColorRGBA(v, p, q, 1.0f);
		}
	};
	const auto RgbToHsv = [](const ColorRGBA &C, float *pOut) {
		const float Mx = std::max(C.r, std::max(C.g, C.b)), Mn = std::min(C.r, std::min(C.g, C.b)), D = Mx - Mn;
		float h = 0.0f;
		if(D > 1e-6f)
		{
			if(Mx == C.r)
				h = std::fmod((C.g - C.b) / D + 6.0f, 6.0f);
			else if(Mx == C.g)
				h = (C.b - C.r) / D + 2.0f;
			else
				h = (C.r - C.g) / D + 4.0f;
			h /= 6.0f;
		}
		pOut[0] = h;
		pOut[1] = Mx > 0.0f ? D / Mx : 0.0f;
		pOut[2] = Mx;
	};
	// inline colour picker: saturation/value square + hue bar + hex
	const auto ColorPickerInline = [&](unsigned int *pColor) {
		if(s_pOpenColor != pColor)
			return;
		if(s_pHsvFor != pColor)
		{
			RgbToHsv(color_cast<ColorRGBA>(ColorHSLA(*pColor)), s_aHsv);
			s_pHsvFor = pColor;
		}
		CUIRect Sq = Take(100.0f * S, 8.0f * S);
		const void *pSqId = (const char *)pColor + 1;
		Ui()->DoButtonLogic(pSqId, 0, &Sq, BUTTONFLAG_LEFT);
		bool Changed = false;
		if(Ui()->CheckActiveItem(pSqId) && Sq.w > 1.0f && Sq.h > 1.0f)
		{
			s_aHsv[1] = std::clamp((Ui()->MouseX() - Sq.x) / Sq.w, 0.0f, 1.0f);
			s_aHsv[2] = 1.0f - std::clamp((Ui()->MouseY() - Sq.y) / Sq.h, 0.0f, 1.0f);
			Changed = true;
		}
		CUIRect Bar = Take(11.0f * S, 8.0f * S);
		const void *pHueId = (const char *)pColor + 2;
		Ui()->DoButtonLogic(pHueId, 0, &Bar, BUTTONFLAG_LEFT);
		if(Ui()->CheckActiveItem(pHueId) && Bar.w > 1.0f)
		{
			s_aHsv[0] = std::clamp((Ui()->MouseX() - Bar.x) / Bar.w, 0.0f, 0.999f);
			Changed = true;
		}
		if(Changed)
		{
			const ColorHSLA Hsl = color_cast<ColorHSLA>(HsvToRgb(s_aHsv[0], s_aHsv[1], s_aHsv[2]));
			*pColor = ((unsigned int)std::round(std::clamp(Hsl.h, 0.0f, 1.0f) * 255.0f) << 16) | ((unsigned int)std::round(std::clamp(Hsl.s, 0.0f, 1.0f) * 255.0f) << 8) | (unsigned int)std::round(std::clamp(Hsl.l, 0.0f, 1.0f) * 255.0f);
		}

		// square
		const ColorRGBA HueC = HsvToRgb(s_aHsv[0], 1.0f, 1.0f);
		Sq.Draw4(White, HueC, White, HueC, IGraphics::CORNER_ALL, 6.0f * S);
		Sq.Draw4(WithA(Black, 0.0f), WithA(Black, 0.0f), Black, Black, IGraphics::CORNER_ALL, 6.0f * S);
		{
			CUIRect M;
			M.w = 11.0f * S;
			M.h = M.w;
			M.x = Sq.x + Sq.w * s_aHsv[1] - M.w / 2.0f;
			M.y = Sq.y + Sq.h * (1.0f - s_aHsv[2]) - M.h / 2.0f;
			M.Draw(WithA(Black, 0.45f), IGraphics::CORNER_ALL, M.w / 2.0f);
			M.Margin(1.3f * S, &M);
			M.Draw(White, IGraphics::CORNER_ALL, M.w / 2.0f);
			M.Margin(1.8f * S, &M);
			M.Draw(HsvToRgb(s_aHsv[0], s_aHsv[1], s_aHsv[2]), IGraphics::CORNER_ALL, M.w / 2.0f);
		}
		// hue bar
		for(int i = 0; i < 12; i++)
		{
			const float T0 = i / 12.0f, T1 = (i + 1) / 12.0f;
			CUIRect Seg = Bar;
			Seg.x = Bar.x + Bar.w * T0;
			Seg.w = Bar.w * (T1 - T0) + 0.6f;
			const int Cor = i == 0 ? IGraphics::CORNER_L : (i == 11 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
			const ColorRGBA C0 = HsvToRgb(T0, 1.0f, 1.0f), C1 = HsvToRgb(T1, 1.0f, 1.0f);
			Seg.Draw4(C0, C1, C0, C1, Cor, Bar.h / 2.0f);
		}
		{
			CUIRect K;
			K.w = 13.0f * S;
			K.h = K.w;
			K.x = Bar.x + Bar.w * s_aHsv[0] - K.w / 2.0f;
			K.y = Bar.y + Bar.h / 2.0f - K.h / 2.0f;
			K.Draw(WithA(Black, 0.40f), IGraphics::CORNER_ALL, K.w / 2.0f);
			K.Margin(1.4f * S, &K);
			K.Draw(White, IGraphics::CORNER_ALL, K.w / 2.0f);
			K.Margin(2.2f * S, &K);
			K.Draw(HsvToRgb(s_aHsv[0], 1.0f, 1.0f), IGraphics::CORNER_ALL, K.w / 2.0f);
		}
		// hex
		const ColorRGBA Cur = color_cast<ColorRGBA>(ColorHSLA(*pColor));
		char aHex[16];
		str_format(aHex, sizeof(aHex), "#%02X%02X%02X", (int)std::round(Cur.r * 255.0f), (int)std::round(Cur.g * 255.0f), (int)std::round(Cur.b * 255.0f));
		CUIRect HexRow = Take(RowH, Gap);
		CUIRect HL, HR;
		HexRow.VSplitLeft(60.0f * S, &HL, &HR);
		Label(&HL, "Цвет", 11.5f * F, TEXTALIGN_ML, ColDim);
		Label(&HR, aHex, 11.5f * F, TEXTALIGN_MR, ColText);
	};
	const auto ToggleRow = [&](const char *pName, int *pValue) {
		CUIRect Row = Take(RowH, Gap);
		CUIRect Lbl, Switch;
		Row.VSplitRight(40.0f * S, &Lbl, &Switch);
		Label(&Lbl, pName, 12.5f * F, TEXTALIGN_ML, ColText);
		DoToggle(pValue, Switch);
	};
	const auto ToggleColorRow = [&](const char *pName, int *pValue, unsigned int *pColor) {
		CUIRect Row = Take(RowH, Gap);
		CUIRect Lbl, ColorBtn, Switch;
		Row.VSplitRight(40.0f * S, &Row, &Switch);
		Row.VSplitRight(8.0f * S, &Row, nullptr);
		Row.VSplitRight(30.0f * S, &Lbl, &ColorBtn);
		Label(&Lbl, pName, 12.5f * F, TEXTALIGN_ML, ColText);
		DoSwatch(pColor, ColorBtn);
		DoToggle(pValue, Switch);
		ColorPickerInline(pColor);
	};
	const auto SliderRow = [&](const char *pName, int *pValue, int Min, int Max, const char *pSuffix) {
		CUIRect Row = Take(36.0f * S, Gap);
		DoVertexSlider(pValue, Min, Max, Row, pName, pSuffix);
	};
	const auto ColorRow = [&](const char *pName, unsigned int *pColor) {
		CUIRect Row = Take(RowH, Gap);
		CUIRect Lbl, Btn;
		Row.VSplitRight(60.0f * S, &Lbl, &Btn);
		Label(&Lbl, pName, 12.5f * F, TEXTALIGN_ML, ColText);
		DoSwatch(pColor, Btn);
		ColorPickerInline(pColor);
	};
	const auto EditRow = [&](const char *pLabel, CLineInput *pInput, bool Hidden) {
		CUIRect Row = Take(30.0f * S, Gap);
		CUIRect Lbl, Field;
		Row.VSplitLeft(118.0f * S, &Lbl, &Field);
		Label(&Lbl, pLabel, 11.5f * F, TEXTALIGN_ML, ColText);
		DoField(pInput, Field, "", Hidden, 8.0f * S);
	};

	// ----- cards -----
	// A card is one module: header pill (name + link icon + optional colour + switch) and its settings below.
	// Returns true while the module is on (or has no switch), so the caller can show the options.
	const auto BeginCard = [&](const char *pTitle, int *pValue, unsigned int *pColor) -> bool {
		CurCol = Col[0].y <= Col[1].y ? 0 : 1;
		Content = Col[CurCol];
		float &H = s_aCardH[s_Tab][std::min(CardIdx++, VX_MAX_CARDS - 1)];
		if(H < 1.0f)
			H = HeadH + 4.0f * S + CardPad;
		pCardH = &H;
		CardStartY = Content.y;

		CUIRect Card = Content;
		Card.h = H;
		const bool Hover = Ui()->MouseInside(&Card) && Ui()->MouseInside(&Clip);
		const float Hv = Anim(&H, 0, Hover ? 1.0f : 0.0f);
		const float On = pValue ? Anim(pValue, 2, *pValue ? 1.0f : 0.0f) : 0.0f;

		ColorRGBA Fill = ColCard;
		Fill.a = std::min(1.0f, Fill.a * (1.0f + 0.18f * Hv));
		ColorRGBA Edge = Mix(WithA(Mix(Tint, White, 0.55f), 0.16f + 0.45f * EdgeK), WithA(Accent, 0.30f + 0.55f * EdgeK), On * 0.85f + Hv * 0.15f);
		DrawGlass(Card, H < HeadH * 2.0f ? H / 2.0f : CardR, Fill, Edge);
		if(On > 0.01f && Glow > 0.01f)
		{
			CUIRect G = Card;
			G.Margin(-2.0f * S, &G);
			G.Draw(WithA(Accent, 0.05f * On * Glow), IGraphics::CORNER_ALL, (H < HeadH * 2.0f ? H / 2.0f : CardR) + 2.0f * S);
		}

		Content.HSplitTop(CardPad * 0.55f, nullptr, &Content);
		InCard = true;
		if(pTitle)
		{
			CUIRect Row = Take(HeadH, 4.0f * S);
			CUIRect Lbl = Row;
			if(pValue)
			{
				CUIRect Switch, LinkR;
				Row.VSplitRight(40.0f * S, &Row, &Switch);
				Row.VSplitRight(22.0f * S, &Row, &LinkR);
				DrawIcon(8, LinkR, WithA(ColText, 0.40f));
				if(pColor)
				{
					CUIRect ColorBtn;
					Row.VSplitRight(30.0f * S, &Row, &ColorBtn);
					DoSwatch(pColor, ColorBtn);
				}
				Lbl = Row;
				DoToggle(pValue, Switch);
			}
			Label(&Lbl, pTitle, 13.5f * F, TEXTALIGN_ML, ColText);
			if(pColor)
				ColorPickerInline(pColor);
		}
		return pValue == nullptr || *pValue != 0;
	};
	const auto EndCard = [&]() {
		Content.HSplitTop(CardPad * 0.45f, nullptr, &Content);
		if(pCardH)
			*pCardH = Content.y - CardStartY;
		InCard = false;
		Content.HSplitTop(CardGap, nullptr, &Content);
		Col[CurCol] = Content;
	};

	// ===================== Aim =====================
	if(s_Tab == 0)
	{
		const auto AimCard = [&](const char *pRu, const char *pEn, int *pOn, int *pFov, int *pPredict, bool WithEsp, int *pTrajectory, unsigned int *pTrajectoryColor, int *pBlastCircle) {
			if(!ShowK(pRu, pEn) && !(WithEsp && (Show("ESP") || Show("FOV") || Show("Рамка") || Show("Свечение"))))
				return;
			if(BeginCard(pRu, pOn, nullptr))
			{
				SliderRow("Угол FOV", pFov, 1, 360, "°");
				
				ToggleRow("Предикт движения", pPredict);
				
				if(WithEsp)
			{
				Rule();
				Caption("Отображение");
				ToggleRow("ESP игроков", &g_Config.m_VxAimbotEspPlayers);
				ToggleColorRow("Показывать FOV", &g_Config.m_VxAimbotDrawFov, &g_Config.m_VxAimbotFovColor);
				if(g_Config.m_VxAimbotDrawFov)
					ToggleColorRow("Подсветка FOV", &g_Config.m_VxAimbotFovHighlight, &g_Config.m_VxAimbotFovHighlightColor);
				ToggleColorRow("Рамка игрока", &g_Config.m_VxAimbotShowBox, &g_Config.m_VxAimbotEspColor);
				ToggleColorRow("Подсветка скина", &g_Config.m_VxAimbotSkinHighlight, &g_Config.m_VxAimbotSkinColor);
			}
			if(pTrajectory && pTrajectoryColor)
			{
				Rule();
				Caption("Траектория");
				ToggleColorRow("Показывать траекторию", pTrajectory, pTrajectoryColor);
			}
			if(pBlastCircle)
				ToggleRow("Круг взрыва", pBlastCircle);
			}
			EndCard();
		};
		
		AimCard("Наведение хука", "Hook Aim", &g_Config.m_VxAimbotHook, &g_Config.m_VxAimbotHookFov, &g_Config.m_VxAimHookPredict, true, nullptr, nullptr, nullptr);
		AimCard("Наведение молота", "Hammer Aim", &g_Config.m_VxAimbotHammer, &g_Config.m_VxAimbotHammerFov, &g_Config.m_VxAimHammerPredict, false, nullptr, nullptr, nullptr);
		AimCard("Наведение дробовика", "Shotgun Aim", &g_Config.m_VxAimbotShotgun, &g_Config.m_VxAimbotShotgunFov, &g_Config.m_VxAimbotShotgunPredict, false, &g_Config.m_VxAimbotShotgunTrajectory, &g_Config.m_VxAimbotShotgunTrajectoryColor, nullptr);
		AimCard("Наведение гранаты", "Grenade Aim", &g_Config.m_VxAimbotGrenade, &g_Config.m_VxAimbotGrenadeFov, &g_Config.m_VxAimGrenadePredict, false, &g_Config.m_VxAimbotGrenadeTrajectory, &g_Config.m_VxAimbotGrenadeTrajectoryColor, &g_Config.m_VxAimbotGrenadeBlastCircle);
		AimCard("Наведение лазера", "Laser Aim", &g_Config.m_VxAimbotLaser, &g_Config.m_VxAimbotLaserFov, &g_Config.m_VxAimbotLaserPredict, false, &g_Config.m_VxAimbotLaserTrajectory, &g_Config.m_VxAimbotLaserTrajectoryColor, nullptr);
	}

	// ===================== Avoid =====================
	else if(s_Tab == 1)
	{
		if(Show("Avoid") || Show("Basic") || Show("Blatant") || Show("Режим"))
		{
			// одна карточка на всю ширину окна (обе колонки макета)
			Col[0] = Content;
			Col[1] = Content;
			BeginCard("Режим Avoid", nullptr, nullptr);
			CUIRect ButtonRow = Take(28.0f * S, Gap);
			const float BtnGap = 6.0f * S;
			float BtnWidth = 150.0f * S;
			const float MaxBtnW = (ButtonRow.w - BtnGap) / 2.0f;
			if(BtnWidth > MaxBtnW)
				BtnWidth = MaxBtnW;
			const float CenterOff = (ButtonRow.w - (BtnWidth * 2.0f + BtnGap)) / 2.0f;
			if(CenterOff > 0.0f)
				ButtonRow.VSplitLeft(CenterOff, nullptr, &ButtonRow);
			CUIRect BtnBasic, BtnBlatant;
			ButtonRow.VSplitLeft(BtnWidth, &BtnBasic, &ButtonRow);
			ButtonRow.VSplitLeft(BtnGap, nullptr, &ButtonRow);
			ButtonRow.VSplitLeft(BtnWidth, &BtnBlatant, &ButtonRow);

			static CButtonContainer s_BtnBasic, s_BtnBlatant;
			// клик по пилюле только выбирает агента; вкл/выкл авойда -- биндом
			if(DoPill(&s_BtnBasic, "Basic", g_Config.m_AvoidAgentType == 0, BtnBasic))
			{
				if(g_Config.m_AvoidAgentType != 0)
				{
					g_Config.m_AvoidAgentType = 0;
					g_Config.m_VxAfEnable = 0;
					g_Config.m_VxAfBlatant = 0;
				}
			}
			if(DoPill(&s_BtnBlatant, "Blatant", g_Config.m_AvoidAgentType == 2, BtnBlatant))
			{
				if(g_Config.m_AvoidAgentType != 2)
				{
					g_Config.m_AvoidAgentType = 2;
					g_Config.m_VxAfEnable = 0;
					g_Config.m_VxAfBlatant = 0;
				}
			}

			if(g_Config.m_AvoidAgentType == 2)
			{
				Rule();
				Caption("Предикт и безопасность");
				ToggleRow("Предикт игроков", &g_Config.m_AvoidPlayerPrediction);
				ToggleRow("Защита от AFK", &g_Config.m_AvoidAfkProtection);
				if(g_Config.m_AvoidAfkProtection)
					SliderRow("AFK секунд", &g_Config.m_AvoidAfkTime, 5, 60, " с");
				ToggleRow("NSIF (не спасать в заморозке)", &g_Config.m_AvoidNsif);
				SliderRow("Тики прогноза", &g_Config.m_AvoidBlatantCheckTicks, 5, 50, "");
				SliderRow("Включить через тиков", &g_Config.m_AvoidKickInTicks, 2, 50, "");

				Rule();
				Caption("Движение");
				ToggleRow("Автохук", &g_Config.m_AvoidBlatantHook);
				ToggleRow("Авто направление / прыжок", &g_Config.m_AvoidBlatantDirection);
				ToggleRow("Обходить телепорты", &g_Config.m_AvoidBlatantTeles);
				ToggleRow("Реагировать на смерть-тайлы", &g_Config.m_AvoidBlatantDeath);
				ToggleRow("Выход из заморозки", &g_Config.m_AvoidBlatantUnfreezeTile);
				SliderRow("Тики безопасного тайла", &g_Config.m_AvoidBlatantUnfreezeTicks, 5, 30, "");

				Rule();
				Caption("Наведение хука");
				ToggleRow("Поиск наведения хука", &g_Config.m_AvoidBlatantAimbot);
				if(g_Config.m_AvoidBlatantAimbot)
				{
					SliderRow("Сегменты поиска", &g_Config.m_AvoidAimbotSegments, 6, 64, "");
					SliderRow("Угол FOV", &g_Config.m_AvoidBlatantFov, 10, 360, "°");
					ToggleRow("Автонаведение", &g_Config.m_AvoidAutoAimAimbot);
					ToggleRow("Помощь в наведении", &g_Config.m_AvoidAimAssistAimbot);
				}

				Rule();
				Caption("Трекинг и визуал");
				ToggleRow("Точка трекинга", &g_Config.m_AvoidTrackPoints);
				if(g_Config.m_AvoidTrackPoints)
					ToggleRow("Безопасный трекинг", &g_Config.m_AvoidSafeAimTracking);
				ToggleRow("Визуал трекинга", &g_Config.m_AvoidDrawTrackPoint);
				ToggleRow("Визуал наведения", &g_Config.m_AvoidDrawAimbot);
				ToggleRow("Визуал пути", &g_Config.m_AvoidDrawPath);
				ToggleRow("Лог отладки", &g_Config.m_AvoidDebug);
			}
			EndCard();
			Col[1] = Col[0]; // синхронизируем колонки после полноширинной карточки
		}
	}

	// ===================== Misc =====================
	else if(s_Tab == 2)
	{
		if(ShowK("Разморозка дамми", "Dummy Unfreeze"))
		{
			if(BeginCard("Разморозка дамми", &g_Config.m_ClVertexDummyUnfreeze, nullptr))
			{
				// extra targets: the dummy also unfreezes these players ("nick1, nick2" or add one by one)
				static char s_aDuAddBuf[96] = "";
				static CLineInput s_DuAddInput(s_aDuAddBuf, sizeof(s_aDuAddBuf));
				Caption("Дополнительные цели");
				CUIRect AddRow = Take(28.0f * S, Gap);
				CUIRect AddBox, AddBtnRect;
				AddRow.VSplitRight(56.0f * S, &AddBox, &AddBtnRect);
				AddBox.VSplitRight(6.0f * S, &AddBox, nullptr);
				DoField(&s_DuAddInput, AddBox, "Ник(и): a, b, c", false, 8.0f * S);
				static CButtonContainer s_DuAddBtn;
				if(DoPill(&s_DuAddBtn, "Add", false, AddBtnRect) && !s_DuAddInput.IsEmpty())
				{
					char aNew[512];
					str_format(aNew, sizeof(aNew), "%s%s%s", g_Config.m_VxDuNames, g_Config.m_VxDuNames[0] ? ", " : "", s_DuAddInput.GetString());
					str_copy(g_Config.m_VxDuNames, aNew, sizeof(g_Config.m_VxDuNames));
					s_DuAddInput.Clear();
				}

				char aNames[16][32];
				int NumNames = 0;
				for(const char *pL = g_Config.m_VxDuNames; *pL && NumNames < 16;)
				{
					int n = 0;
					char aTmp[32];
					while(*pL && *pL != ',' && *pL != ';')
					{
						if(n < 31)
							aTmp[n++] = *pL;
						pL++;
					}
					if(*pL)
						pL++;
					aTmp[n] = 0;
					int St = 0;
					while(aTmp[St] == ' ')
						St++;
					while(n > St && aTmp[n - 1] == ' ')
						aTmp[--n] = 0;
					if(n > St)
						str_copy(aNames[NumNames++], aTmp + St, sizeof(aNames[0]));
				}
				static CButtonContainer s_aDuDelBtns[16];
				int DelIdx = -1;
				for(int i = 0; i < NumNames; i++)
				{
					CUIRect ListRow = Take(RowH, Gap);
					CUIRect NameLabel, DelBtn;
					ListRow.VSplitRight(40.0f * S, &NameLabel, &DelBtn);
					Label(&NameLabel, aNames[i], 12.5f * F, TEXTALIGN_ML, ColText);
					if(DoPill(&s_aDuDelBtns[i], "X", false, DelBtn))
						DelIdx = i;
				}
				if(DelIdx >= 0)
				{
					g_Config.m_VxDuNames[0] = '\0';
					for(int i = 0; i < NumNames; i++)
					{
						if(i == DelIdx)
							continue;
						char aNew[512];
						str_format(aNew, sizeof(aNew), "%s%s%s", g_Config.m_VxDuNames, g_Config.m_VxDuNames[0] ? ", " : "", aNames[i]);
						str_copy(g_Config.m_VxDuNames, aNew, sizeof(g_Config.m_VxDuNames));
					}
				}
			}
			EndCard();
		}

		if(ShowK("Авто-разморозка", "Auto Unfreeze"))
		{
			if(BeginCard("Авто-разморозка", &g_Config.m_ClVertexUnfreezeBot, nullptr))
			{
				ToggleRow("Тихий аим (silent)", &g_Config.m_ClVertexUnfreezeSilent);
				ToggleRow("Автосмена оружия", &g_Config.m_ClVertexUnfreezeAuto);
				SliderRow("Тики триггера", &g_Config.m_ClVertexUnfreezeTriggerTicks, 1, 40, "");
				SliderRow("Углы", &g_Config.m_ClVertexUnfreezeAngles, 48, 280, "");
				SliderRow("Тики симуляции", &g_Config.m_ClVertexUnfreezeTicks, 12, 55, "");
				SliderRow("Угол FOV", &g_Config.m_ClVertexUnfreezeFov, 1, 360, "°");
				ToggleRow("Лог отладки", &g_Config.m_ClVertexUnfreezeDebug);
			}
			EndCard();
		}

		// Auto Aled: one switch, everything else is built into the module
		if(ShowK("Авто алед", "Auto Aled"))
		{
			BeginCard("Авто алед", &g_Config.m_VxAledEnable, nullptr);
			EndCard();
		}

		if(ShowK("Войны и команда", "War and Team"))
		{
			static int s_WarAndTeamOpen = 1;
			if(BeginCard("Войны и команда", &s_WarAndTeamOpen, nullptr))
			{
				ToggleRow("Цветной чат (ник + текст)", &g_Config.m_VxChatNameColors);
				ColorRow("Цвет врага (!war)", &g_Config.m_VxColorWar);
				ColorRow("Цвет тиммейта (!team)", &g_Config.m_VxColorTeam);
				ColorRow("Цвет помощника (!helper)", &g_Config.m_VxColorHelper);
				Rule();

				static char s_NameInputBuf[32] = "";
				static CLineInput s_NameInput(s_NameInputBuf, sizeof(s_NameInputBuf));
				static unsigned int s_Color = 65408;

				CUIRect Row = Take(28.0f * S, Gap);
				CUIRect InputBox, ColorBtn, AddBtn, Rest;
				Row.VSplitRight(56.0f * S, &Rest, &AddBtn);
				Rest.VSplitRight(6.0f * S, &Rest, nullptr);
				Rest.VSplitRight(30.0f * S, &Rest, &ColorBtn);
				Rest.VSplitRight(6.0f * S, &InputBox, nullptr);

				DoField(&s_NameInput, InputBox, "Ник игрока", false, 8.0f * S);
				DoSwatch(&s_Color, ColorBtn);

				static CButtonContainer s_AddBtn;
				if(DoPill(&s_AddBtn, "Add", false, AddBtn) && !s_NameInput.IsEmpty())
				{
					GameClient()->AddNameColor(s_NameInput.GetString(), s_Color);
					s_NameInput.Clear();
				}
				ColorPickerInline(&s_Color);

				static CButtonContainer s_aDelBtns[512];
				for(int i = 0; i < (int)GameClient()->m_vNameColors.size() && i < 512; i++)
				{
					auto &e = GameClient()->m_vNameColors[i];
					CUIRect ListRow = Take(RowH, Gap);
					CUIRect NameLabel, ColorBtn2, DelBtn;
					ListRow.VSplitRight(40.0f * S, &NameLabel, &DelBtn);
					NameLabel.VSplitRight(40.0f * S, &NameLabel, &ColorBtn2);
					ColorBtn2.VSplitRight(6.0f * S, &ColorBtn2, nullptr);

					Label(&NameLabel, e.m_Name, 12.5f * F, TEXTALIGN_ML, ColText);
					DoSwatch(&e.m_Color, ColorBtn2);

					if(DoPill(&s_aDelBtns[i], "X", false, DelBtn))
					{
						GameClient()->m_vNameColors.erase(GameClient()->m_vNameColors.begin() + i);
						GameClient()->SaveNameColors();
						i--;
					}
				}
			}
			EndCard();
		}
	}

	// ===================== Visual =====================
	else if(s_Tab == 3)
	{
                // ---- appearance ----
		if(ShowK("Оформление меню", "Appearance") || Show("цвет") || Show("стекл") || Show("свечение") || Show("фон"))
		{
			BeginCard("Оформление меню", nullptr, nullptr);

			Caption("Фон и пресеты");
			{
				static const char *s_apPresetNames[7] = {"Сирень", "Лёд", "Роза", "Ночь", "Чёрный", "Оранжевый", "Белый"};
				// accent H, L | tint H, S, L | glass %
				static const float s_aPreset[7][6] = {
					{0.70f, 0.75f, 0.68f, 0.18f, 0.16f, 38.0f},
					{0.52f, 0.68f, 0.55f, 0.20f, 0.15f, 36.0f},
					{0.93f, 0.74f, 0.92f, 0.15f, 0.16f, 38.0f},
					{0.60f, 0.70f, 0.65f, 0.10f, 0.07f, 55.0f},
					{0.00f, 0.85f, 0.00f, 0.00f, 0.04f, 60.0f}, // black
					{0.08f, 0.62f, 0.07f, 0.85f, 0.40f, 45.0f}, // orange
					{0.60f, 0.45f, 0.00f, 0.00f, 0.93f, 75.0f}}; // white (switches the menu to dark text)
				const auto PackHsl = [](float H, float Sa, float L) -> unsigned int {
					return ((unsigned int)(H * 255.0f) << 16) | ((unsigned int)(Sa * 255.0f) << 8) | (unsigned int)(L * 255.0f);
				};
				static CButtonContainer s_aPresetBtns[7];
				const float PG = 6.0f * S;
				for(int RowI = 0; RowI < 2; RowI++)
				{
					CUIRect Row = Take(28.0f * S, Gap);
					const float PW = (Row.w - 3.0f * PG) / 4.0f;
					for(int c = 0; c < 4; c++)
					{
						const int i = RowI * 4 + c;
						if(i >= 7)
							break;
						CUIRect B;
						Row.VSplitLeft(PW, &B, &Row);
						Row.VSplitLeft(PG, nullptr, &Row);
						if(DoPill(&s_aPresetBtns[i], s_apPresetNames[i], false, B))
						{
							g_Config.m_VxUiAccent = PackHsl(s_aPreset[i][0], 1.0f, s_aPreset[i][1]);
							g_Config.m_VxUiTint = PackHsl(s_aPreset[i][2], s_aPreset[i][3], s_aPreset[i][4]);
							g_Config.m_VxUiGlass = (int)s_aPreset[i][5];
							s_pHsvFor = nullptr;
							g_Config.m_VxUiRainbow = 0;
						}
					}
				}
			}

			Rule();
			Caption("Цвета");
			ColorRow("Акцентный цвет", &g_Config.m_VxUiAccent);
			ToggleRow("Плавная RGB-подсветка", &g_Config.m_VxUiRainbow);
			ColorRow("Цвет стекла", &g_Config.m_VxUiTint);

			Rule();
			Caption("Стекло");
			SliderRow("Застекление (плотность)", &g_Config.m_VxUiGlass, 10, 100, "%");
			SliderRow("Иней / блик", &g_Config.m_VxUiFrost, 0, 100, "%");
			SliderRow("Свечение", &g_Config.m_VxUiGlow, 0, 100, "%");
			SliderRow("Яркость рамки", &g_Config.m_VxUiEdge, 0, 100, "%");
			SliderRow("Прозрачность блоков", &g_Config.m_VxUiCardAlpha, 10, 100, "%");

			Rule();
			Caption("Форма и движение");
			SliderRow("Скругление", &g_Config.m_VxUiRadius, 4, 32, "");
			SliderRow("Скорость анимаций (0 = выкл)", &g_Config.m_VxUiAnim, 0, 100, "%");
			SliderRow("Размер шрифта", &g_Config.m_VxUiFont, 80, 130, "%");
			SliderRow("Масштаб меню", &g_Config.m_VxClickGuiScale, 40, 120, "%");

			CUIRect ResetRow = Take(28.0f * S, Gap);
			CUIRect ResetBtn;
			ResetRow.VSplitRight(180.0f * S, nullptr, &ResetBtn);
			static CButtonContainer s_ResetStyle;
			if(DoPill(&s_ResetStyle, "Сбросить оформление", false, ResetBtn))
			{
				g_Config.m_VxUiAccent = 11049407;
				g_Config.m_VxUiTint = 11146280;
				g_Config.m_VxUiRainbow = 0;
				g_Config.m_VxUiGlass = 35;
				g_Config.m_VxUiFrost = 35;
				g_Config.m_VxUiGlow = 15;
				g_Config.m_VxUiEdge = 40;
				s_pHsvFor = nullptr;
				g_Config.m_VxUiRadius = 14;
				g_Config.m_VxUiCardAlpha = 35;
				g_Config.m_VxUiAnim = 60;
				g_Config.m_VxUiFont = 100;
			}
			EndCard();
		}

		// ---- binds: click the pill, then press a key or a mouse button (not the left one); Esc clears it ----
		if(ShowK("Мировые частицы", "World Particles"))
		{
			if(BeginCard("Мировые частицы", &g_Config.m_VxParticles, &g_Config.m_VxParticlesColor))
			{
				static const char *s_apShapes[9] = {"Сердца", "Звёзды", "Круги", "Снежинки", "Пузыри", "Светлячки", "Лепестки", "Конфетти", "Микс"};
				static CButtonContainer s_aShapeBtns[9];
				const float SG = 5.0f * S;
				for(int RowI = 0; RowI < 3; RowI++)
				{
					CUIRect Row = Take(26.0f * S, Gap);
					const float SW = (Row.w - 2.0f * SG) / 3.0f;
					for(int c = 0; c < 3; c++)
					{
						const int i = RowI * 3 + c;
						CUIRect B;
						Row.VSplitLeft(SW, &B, &Row);
						Row.VSplitLeft(SG, nullptr, &Row);
						if(DoPill(&s_aShapeBtns[i], s_apShapes[i], g_Config.m_VxParticlesShape == i, B))
							g_Config.m_VxParticlesShape = i;
					}
				}
				SliderRow("Количество", &g_Config.m_VxParticlesCount, 1, 300, "");
				SliderRow("Размер", &g_Config.m_VxParticlesSize, 10, 300, "%");
				SliderRow("Скорость", &g_Config.m_VxParticlesSpeed, 10, 200, "%");
			}
			EndCard();
		}
		if(ShowK("Листья", "Leaves") || Show("осень") || Show("Autumn"))
		{
			if(BeginCard("Листья", &g_Config.m_VxLeaves, nullptr))
			{
				SliderRow("Количество", &g_Config.m_VxLeavesCount, 1, 400, "");
				SliderRow("Размер", &g_Config.m_VxLeavesSize, 20, 300, "%");
				SliderRow("Ветер", &g_Config.m_VxLeavesWind, 0, 200, "%");
                                SliderRow("Дальность прорисовки", &g_Config.m_VxLeavesRange, 100, 400, "%");
                                ToggleRow("Лежат и не исчезают", &g_Config.m_VxLeavesPersist);
			}
			EndCard();
		}
		if(ShowK("Дождь", "Rain") || Show("ливень"))
		{
			if(BeginCard("Дождь", &g_Config.m_VxRain, &g_Config.m_VxRainColor))
			{
				static const char *s_apRain[2] = {"Лёгкий", "Сильный"};
				static CButtonContainer s_aRainBtns[2];
				CUIRect Row = Take(26.0f * S, Gap);
				const float RG = 6.0f * S;
				const float RW = (Row.w - RG) / 2.0f;
				for(int i = 0; i < 2; i++)
				{
					CUIRect B;
					Row.VSplitLeft(RW, &B, &Row);
					Row.VSplitLeft(RG, nullptr, &Row);
					if(DoPill(&s_aRainBtns[i], s_apRain[i], g_Config.m_VxRainHeavy == i, B))
						g_Config.m_VxRainHeavy = i;
				}
				SliderRow("Наклон (ветер)", &g_Config.m_VxRainSlant, 0, 100, "%");
			}
			EndCard();
		}
		if(ShowK("Список модулей", "Module List"))
		{
			BeginCard("Список модулей", &g_Config.m_VxModuleList, &g_Config.m_VxModuleListColor);
			EndCard();
		}
		if(ShowK("ESP разморозки дамми", "Dummy Unfreeze ESP"))
		{
			BeginCard("ESP разморозки дамми", &g_Config.m_VxDuEsp, nullptr);
			EndCard();
		}
	}

	// ===================== Settings =====================
	else if(s_Tab == 5)
	{
		static int s_BindTarget = 0;
		const auto BindRow = [&](const char *pName, int *pKey, int Target) {
			CUIRect Row = Take(RowH + 4.0f * S, Gap);
			CUIRect Lbl, Btn;
			Row.VSplitRight(140.0f * S, &Lbl, &Btn);
			Label(&Lbl, pName, 12.5f * F, TEXTALIGN_ML, ColText);

			static CButtonContainer s_aBindBtns[8];
			const bool Listening = m_VertexBindListening && s_BindTarget == Target;
			char aBuf[64];
			if(Listening)
				str_copy(aBuf, "Нажмите клавишу...", sizeof(aBuf));
			else if(*pKey <= 0)
				str_copy(aBuf, "Нет", sizeof(aBuf));
			else if(*pKey >= KEY_MOUSE_1 && *pKey <= KEY_MOUSE_9)
				str_format(aBuf, sizeof(aBuf), "Mouse %d", *pKey - KEY_MOUSE_1 + 1);
			else
				str_format(aBuf, sizeof(aBuf), "Key %d", *pKey);
			if(DoPill(&s_aBindBtns[Target], aBuf, Listening, Btn))
			{
				m_VertexBindListening = true;
				s_BindTarget = Target;
				m_VertexBindBlockUntil = time_get() + time_freq() / 5;
			}

			if(Listening && time_get() >= m_VertexBindBlockUntil)
			{
				for(int Key = 1; Key <= KEY_MOUSE_9; Key++)
				{
					if(Key == KEY_MOUSE_1 || !Input()->KeyPress(Key))
						continue;
					*pKey = Key == KEY_ESCAPE ? 0 : Key;
					if(*pKey > 0)
					{
						// the same key can only belong to one bind: take it away from the others
												int *apAll[5] = {&g_Config.m_VxClickGuiKey, &g_Config.m_VxBindAvoid, &g_Config.m_VxBindAim, &g_Config.m_VxBindUnfreeze, &g_Config.m_VxBindAled};
						for(int *pOther : apAll)
						{
							if(pOther != pKey && *pOther == *pKey)
								*pOther = 0;
						}
					}
					m_VertexBindListening = false;
					m_VertexBindBlockUntil = time_get() + time_freq() / 3;
					break;
				}
			}
		};

		if(ShowK("Бинды", "Binds") || Show("Click GUI") || Show("Avoid") || Show("Aim") || Show("Auto Unfreeze"))
		{
			BeginCard("Бинды", nullptr, nullptr);
			BindRow("Click GUI", &g_Config.m_VxClickGuiKey, 1);
			BindRow("Avoid", &g_Config.m_VxBindAvoid, 2);
			BindRow("Aim", &g_Config.m_VxBindAim, 3);
			BindRow("Auto Unfreeze", &g_Config.m_VxBindUnfreeze, 4);
                        BindRow("Auto Aled", &g_Config.m_VxBindAled, 5);
			EndCard();
		}

		// ---- SOCKS5 proxy ----
		static bool s_ProxyInputsInitialized = false;
		static char s_aProxyHost[128];
		static char s_aProxyPort[8];
		static char s_aProxyUsername[64];
		static char s_aProxyPassword[64];
		static CLineInput s_ProxyHostInput(s_aProxyHost, sizeof(s_aProxyHost));
		static CLineInput s_ProxyPortInput(s_aProxyPort, sizeof(s_aProxyPort));
		static CLineInput s_ProxyUsernameInput(s_aProxyUsername, sizeof(s_aProxyUsername));
		static CLineInput s_ProxyPasswordInput(s_aProxyPassword, sizeof(s_aProxyPassword));

		if(!s_ProxyInputsInitialized)
		{
			str_copy(s_aProxyHost, g_Config.m_ClSocks5Host, sizeof(s_aProxyHost));
			str_format(s_aProxyPort, sizeof(s_aProxyPort), "%d", g_Config.m_ClSocks5Port);
			str_copy(s_aProxyUsername, g_Config.m_ClSocks5Username, sizeof(s_aProxyUsername));
			str_copy(s_aProxyPassword, g_Config.m_ClSocks5Password, sizeof(s_aProxyPassword));
			s_ProxyInputsInitialized = true;
		}
		if(ShowK("SOCKS5 прокси", "SOCKS5 proxy"))
		{
			if(BeginCard("SOCKS5 прокси", &g_Config.m_ClSocks5Enable, nullptr))
			{
				s_ProxyPasswordInput.SetHidden(true);
				EditRow("Адрес прокси", &s_ProxyHostInput, false);
				EditRow("Порт", &s_ProxyPortInput, false);
				EditRow("Логин", &s_ProxyUsernameInput, false);
				EditRow("Пароль", &s_ProxyPasswordInput, true);

				str_copy(g_Config.m_ClSocks5Host, s_ProxyHostInput.GetString(), sizeof(g_Config.m_ClSocks5Host));
				str_copy(g_Config.m_ClSocks5Username, s_ProxyUsernameInput.GetString(), sizeof(g_Config.m_ClSocks5Username));
				str_copy(g_Config.m_ClSocks5Password, s_ProxyPasswordInput.GetString(), sizeof(g_Config.m_ClSocks5Password));
				const int Port = std::atoi(s_ProxyPortInput.GetString());
				if(Port >= 1 && Port <= 65535)
					g_Config.m_ClSocks5Port = Port;

				Note("Нужен SOCKS5-сервер с поддержкой UDP ASSOCIATE.");
				Note("Через прокси пойдут все игровые подключения; список серверов останется напрямую.");
				Note("Логин и пароль сохраняются в config.cfg обычным текстом.");
				Note("Не передавайте config.cfg другим людям.");
				Note("После изменения параметров перезапустите Vertex.");
			}
			EndCard();
		}
	}

	// ===================== Admin =====================
	else if(s_Tab == 7)
	{
		static char s_aAdminUser[64] = "";
		static char s_aAdminPass[64] = "";
		static char s_aNewLogin[64] = "";
		static char s_aNewPass[64] = "";
		static char s_aTargetLogin[64] = "";
		static char s_aBanReason[96] = "Policy violation";
		static CLineInput s_AdminUserInput(s_aAdminUser, sizeof(s_aAdminUser));
		static CLineInput s_AdminPassInput(s_aAdminPass, sizeof(s_aAdminPass));
		static CLineInput s_NewLoginInput(s_aNewLogin, sizeof(s_aNewLogin));
		static CLineInput s_NewPassInput(s_aNewPass, sizeof(s_aNewPass));
		static CLineInput s_TargetLoginInput(s_aTargetLogin, sizeof(s_aTargetLogin));
		static CLineInput s_BanReasonInput(s_aBanReason, sizeof(s_aBanReason));
		s_AdminPassInput.SetHidden(true);
		s_NewPassInput.SetHidden(true);
		static int s_LicenseHours = 4;
		static CButtonContainer s_AdminLoginButton, s_AdminCreateButton, s_AdminRefreshButton;
		static CButtonContainer s_AdminBanButton, s_AdminUnbanButton;
		static CButtonContainer s_aHoursBtns[4];

		if(!VxGate::AdminIsLoggedIn())
		{
			BeginCard("Вход администратора", nullptr, nullptr);
			Note("Данные администратора настраиваются на сервере лицензий");
			EditRow("Логин админа", &s_AdminUserInput, false);
			EditRow("Пароль админа", &s_AdminPassInput, true);
			CUIRect Row = Take(RowH + 4.0f * S, Gap);
			CUIRect Button;
			Row.VSplitRight(150.0f * S, nullptr, &Button);
			if(DoPill(&s_AdminLoginButton, "Войти", false, Button))
				VxGate::AdminLogin(GameClient(), s_AdminUserInput.GetString(), s_AdminPassInput.GetString());
			Note(VxGate::g_aAdminStatus);
			Note("Не храните пароль администратора в исходном коде.");
			EndCard();
		}
		else
		{
			BeginCard("Выдача лицензии", nullptr, nullptr);
			Note("Сессия администратора активна");
			EditRow("Логин игрока", &s_NewLoginInput, false);
			EditRow("Пароль игрока", &s_NewPassInput, true);
			Caption("Срок лицензии");
			{
				static const char *s_apHourNames[4] = {"4 часа", "24 часа", "7 дней", "30 дней"};
				static const int s_aHourValues[4] = {4, 24, 168, 720};
				CUIRect Row = Take(RowH + 2.0f * S, Gap);
				const float HG = 6.0f * S;
				const float HW = (Row.w - 3.0f * HG) / 4.0f;
				for(int i = 0; i < 4; i++)
				{
					CUIRect B;
					Row.VSplitLeft(HW, &B, &Row);
					Row.VSplitLeft(HG, nullptr, &Row);
					if(DoPill(&s_aHoursBtns[i], s_apHourNames[i], s_LicenseHours == s_aHourValues[i], B))
						s_LicenseHours = s_aHourValues[i];
				}
			}
			CUIRect RowCreate = Take(RowH + 2.0f * S, Gap);
			CUIRect CreateButton;
			RowCreate.VSplitRight(190.0f * S, nullptr, &CreateButton);
			if(DoPill(&s_AdminCreateButton, "Создать лицензию", false, CreateButton))
				VxGate::AdminCreateLicense(GameClient(), s_NewLoginInput.GetString(), s_NewPassInput.GetString(), s_LicenseHours);
			CUIRect RowRefresh = Take(RowH + 2.0f * S, Gap);
			CUIRect RefreshButton;
			RowRefresh.VSplitRight(190.0f * S, nullptr, &RefreshButton);
			if(DoPill(&s_AdminRefreshButton, "Обновить список", false, RefreshButton))
				VxGate::AdminRefreshLicenses(GameClient());
			EndCard();

			BeginCard("Бан игрока", nullptr, nullptr);
			Note("Бан / разбан по точному логину");
			EditRow("Логин цели", &s_TargetLoginInput, false);
			EditRow("Причина бана", &s_BanReasonInput, false);
			CUIRect RowBan = Take(RowH + 2.0f * S, Gap);
			CUIRect BanButton, UnbanButton, Rest;
			RowBan.VSplitLeft(RowBan.w / 2.0f - 3.0f * S, &BanButton, &Rest);
			Rest.VSplitLeft(6.0f * S, nullptr, &UnbanButton);
			if(DoPill(&s_AdminBanButton, "Бан", false, BanButton))
				VxGate::AdminBan(GameClient(), s_TargetLoginInput.GetString(), s_BanReasonInput.GetString());
			if(DoPill(&s_AdminUnbanButton, "Разбан", false, UnbanButton))
				VxGate::AdminUnban(GameClient(), s_TargetLoginInput.GetString());
			Note(VxGate::g_aAdminStatus);
			EndCard();

			BeginCard("Лицензии", nullptr, nullptr);
			Caption("логин | срок (UTC) | статус | устройство");
			std::string List = VxGate::g_aLicenseList;
			size_t Pos = 0;
			int Shown = 0;
			while(Pos < List.size() && Shown < 10)
			{
				const size_t End = List.find('\n', Pos);
				const std::string Line = List.substr(Pos, End == std::string::npos ? std::string::npos : End - Pos);
				if(!Line.empty())
				{
					CUIRect ListRow = Take(RowH, 2.0f * S);
					Label(&ListRow, Line.c_str(), 9.5f * F, TEXTALIGN_ML, ColText);
					++Shown;
				}
				if(End == std::string::npos)
					break;
				Pos = End + 1;
			}
			EndCard();
		}
	}

	// ===================== Fly / Configs =====================
	else
	{
		BeginCard("Скоро", nullptr, nullptr);
		Note("Этот раздел ещё в разработке.");
		EndCard();
	}

	// total height of everything drawn (without scroll offset) -> scroll limit
	ContentHeight = (std::max(Col[0].y, Col[1].y) - ContentTop) + 10.0f * S;
	Ui()->ClipDisable();
}

// Restored original Vertex world effects from pre-AimV2 backup.
struct SVxParticle
{
        float m_X, m_Y, m_Vx, m_Vy, m_Rot, m_Vr, m_Size, m_Phase, m_Hue;
        int m_Shape;
};

struct SVxWLeaf
{
        vec2 m_Pos, m_Vel;
        float m_Rot, m_Size, m_Phase, m_Flip, m_FlipSpeed, m_Life, m_Age;
        int m_Color;
        bool m_Landed;
};

struct SVxDrop
{
        float m_X, m_Y, m_Speed, m_Len, m_Alpha;
};
static vec2 g_VxCamDelta = vec2(0.0f, 0.0f); // how far the camera moved this frame (fraction of the view)
static float VxRandF()
{
        return (float)rand() / (float)RAND_MAX;
}

static ColorRGBA VxHue(float h)
{
        h -= std::floor(h);
        const float f6 = h * 6.0f;
        const int i = (int)f6 % 6;
        const float f = f6 - std::floor(f6);
        const float q = 1.0f - f, t = f;
        switch(i)
        {
        case 0: return ColorRGBA(1.0f, t, 0.0f, 1.0f);
        case 1: return ColorRGBA(q, 1.0f, 0.0f, 1.0f);
        case 2: return ColorRGBA(0.0f, 1.0f, t, 1.0f);
        case 3: return ColorRGBA(0.0f, q, 1.0f, 1.0f);
        case 4: return ColorRGBA(t, 0.0f, 1.0f, 1.0f);
        default: return ColorRGBA(1.0f, 0.0f, q, 1.0f);
        }
}

// pointed leaf: two freeform quads + optional darker midrib/stem. Must be called between QuadsBegin/QuadsEnd.
static void VxDrawLeaf(IGraphics *pGraphics, float cx, float cy, float Rot, float Ln, float Wd, const ColorRGBA &Col, float Alpha, bool Rib)
{
        const float ca = std::cos(Rot), sa = std::sin(Rot);
        const auto Pt = [&](float lx, float ly) { return vec2(cx + lx * ca - ly * sa, cy + lx * sa + ly * ca); };
        const vec2 A = Pt(-Ln, 0.0f), B = Pt(-Ln * 0.35f, -Wd), C = Pt(Ln * 0.45f, -Wd * 0.8f), D = Pt(Ln, 0.0f);
        const vec2 E = Pt(-Ln * 0.35f, Wd), F = Pt(Ln * 0.45f, Wd * 0.8f);
        pGraphics->SetColor(ColorRGBA(Col.r, Col.g, Col.b, Alpha));
        const IGraphics::CFreeformItem Up(A.x, A.y, B.x, B.y, D.x, D.y, C.x, C.y);
        const IGraphics::CFreeformItem Lo(A.x, A.y, E.x, E.y, D.x, D.y, F.x, F.y);
        pGraphics->QuadsDrawFreeform(&Up, 1);
        pGraphics->QuadsDrawFreeform(&Lo, 1);
        if(Rib)
        {
                pGraphics->SetColor(ColorRGBA(Col.r * 0.6f, Col.g * 0.6f, Col.b * 0.6f, Alpha));
                const float Th = std::max(0.08f, Ln * 0.05f);
                const vec2 R0 = Pt(-Ln * 1.3f, -Th), R1 = Pt(Ln * 0.8f, -Th), R2 = Pt(-Ln * 1.3f, Th), R3 = Pt(Ln * 0.8f, Th);
                const IGraphics::CFreeformItem RibQ(R0.x, R0.y, R1.x, R1.y, R2.x, R2.y, R3.x, R3.y);
                pGraphics->QuadsDrawFreeform(&RibQ, 1);
        }
}

// ---------------- particles (screen space) ----------------
// Shapes: 0 heart, 1 star, 2 circle, 3 snowflake, 4 bubble, 5 firefly, 6 petal, 7 confetti, 8 mix of all
static void VxRenderHearts(IGraphics *pGraphics, float Dt, float W, float H)
{
        static std::vector<SVxParticle> s_vParticles;
        const int Want = std::clamp(g_Config.m_VxParticlesCount, 1, 300);
        while((int)s_vParticles.size() < Want)
        {
                SVxParticle p;
                p.m_X = VxRandF();
                p.m_Y = VxRandF();
                p.m_Vx = (VxRandF() - 0.5f) * 0.01f;
                p.m_Vy = 0.012f + VxRandF() * 0.035f;
                p.m_Rot = VxRandF() * 6.2831853f;
                p.m_Vr = (VxRandF() - 0.5f) * 0.9f;
                p.m_Size = 0.6f + VxRandF() * 0.8f;
                p.m_Phase = VxRandF() * 6.2831853f;
                p.m_Hue = VxRandF();
                p.m_Shape = rand();
                s_vParticles.push_back(p);
        }
        if((int)s_vParticles.size() > Want)
                s_vParticles.resize(Want);

        const float Spd = std::clamp(g_Config.m_VxParticlesSpeed, 10, 200) / 100.0f;
        const float Sz = std::clamp(g_Config.m_VxParticlesSize, 10, 300) / 100.0f * H * 0.010f;
        const int Mode = std::clamp(g_Config.m_VxParticlesShape, 0, 8);
        const ColorRGBA Base = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_VxParticlesColor));

        pGraphics->TextureClear();
        pGraphics->QuadsBegin();
        for(SVxParticle &p : s_vParticles)
        {
                const int Shape = Mode == 8 ? p.m_Shape % 8 : Mode;
                const bool Falls = Shape == 3 || Shape == 6 || Shape == 7;
                p.m_Phase += Dt * 0.9f * Spd;
                p.m_X -= g_VxCamDelta.x * (0.6f + 0.3f * p.m_Size);
                p.m_Y -= g_VxCamDelta.y * (0.6f + 0.3f * p.m_Size);

                float Vx = p.m_Vx + std::sin(p.m_Phase) * 0.006f;
                float Vy = Falls ? p.m_Vy * 0.9f : -p.m_Vy;
                if(Shape == 5) // firefly: lazy wandering
                {
                        Vx = std::cos(p.m_Phase * 0.7f + p.m_Rot) * 0.02f;
                        Vy = std::sin(p.m_Phase * 0.8f + p.m_Rot) * 0.02f;
                }
                p.m_X += Vx * Spd * Dt;
                p.m_Y += Vy * Spd * Dt;
                p.m_Rot += p.m_Vr * Spd * Dt;
                if(p.m_Y < -0.06f)
                {
                        p.m_Y = 1.06f;
                        p.m_X = VxRandF();
                }
                if(p.m_Y > 1.06f)
                {
                        p.m_Y = -0.06f;
                        p.m_X = VxRandF();
                }
                if(p.m_X < -0.06f)
                        p.m_X = 1.06f;
                if(p.m_X > 1.06f)
                        p.m_X = -0.06f;

                const float Alpha = 0.30f + 0.40f * (0.5f + 0.5f * std::sin(p.m_Phase * 1.3f));
                const float cx = p.m_X * W, cy = p.m_Y * H;
                const float s = Sz * p.m_Size;

                if(Shape == 0) // heart
                {
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, Alpha));
                        const float a = s * 1.6f;
                        const float Th = std::sin(p.m_Phase) * 0.35f;
                        const float k = a * 0.35355339f;
                        pGraphics->QuadsSetRotation(Th + 0.78539816f);
                        IGraphics::CQuadItem Q(cx - a / 2.0f, cy - a / 2.0f, a, a);
                        pGraphics->QuadsDrawTL(&Q, 1);
                        pGraphics->QuadsSetRotation(0.0f);
                        for(int Side = -1; Side <= 1; Side += 2)
                        {
                                const float lx = Side * k, ly = -k;
                                pGraphics->DrawCircle(cx + lx * std::cos(Th) - ly * std::sin(Th), cy + lx * std::sin(Th) + ly * std::cos(Th), a * 0.5f, 14);
                        }
                }
                else if(Shape == 1) // sparkle star
                {
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, Alpha));
                        for(int k = 0; k < 4; k++)
                        {
                                const float Len = (k % 2 == 0 ? 2.0f : 1.2f) * s;
                                pGraphics->QuadsSetRotation(p.m_Rot + k * 0.78539816f);
                                IGraphics::CQuadItem Q(cx - Len / 2.0f, cy - s * 0.12f, Len, s * 0.24f);
                                pGraphics->QuadsDrawTL(&Q, 1);
                        }
                        pGraphics->QuadsSetRotation(0.0f);
                }
                else if(Shape == 2) // circle
                {
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, Alpha));
                        pGraphics->DrawCircle(cx, cy, s * 0.5f, 12);
                }
                else if(Shape == 3) // snowflake: 3 crossing bars
                {
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, Alpha + 0.15f));
                        for(int k = 0; k < 3; k++)
                        {
                                pGraphics->QuadsSetRotation(p.m_Rot + k * 1.04719755f);
                                IGraphics::CQuadItem Q(cx - s, cy - s * 0.09f, s * 2.0f, s * 0.18f);
                                pGraphics->QuadsDrawTL(&Q, 1);
                        }
                        pGraphics->QuadsSetRotation(0.0f);
                }
                else if(Shape == 4) // bubble: soft body + highlight
                {
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, Alpha * 0.55f));
                        pGraphics->DrawCircle(cx, cy, s * 0.75f, 16);
                        pGraphics->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.75f));
                        pGraphics->DrawCircle(cx - s * 0.28f, cy - s * 0.28f, s * 0.15f, 8);
                }
                else if(Shape == 5) // firefly: pulsing glow
                {
                        const float Pulse = 0.5f + 0.5f * std::sin(p.m_Phase * 1.6f);
                        const float a = 0.10f + 0.90f * Pulse * Pulse;
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, a * 0.16f));
                        pGraphics->DrawCircle(cx, cy, s * 1.3f, 16);
                        pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, a * 0.35f));
                        pGraphics->DrawCircle(cx, cy, s * 0.6f, 12);
                        pGraphics->SetColor(ColorRGBA(1.0f, 1.0f, 0.9f, a));
                        pGraphics->DrawCircle(cx, cy, s * 0.22f, 8);
                }
                else if(Shape == 6) // petal
                {
                        const float Fl = std::max(0.2f, std::abs(std::cos(p.m_Phase * 1.7f + p.m_Hue * 6.0f)));
                        VxDrawLeaf(pGraphics, cx, cy, p.m_Rot, s * 0.9f, s * 0.55f * Fl, Base, std::min(1.0f, Alpha + 0.3f), false);
                }
                else // confetti: spinning coloured rectangle
                {
                        const ColorRGBA C = VxHue(p.m_Hue);
                        pGraphics->SetColor(ColorRGBA(C.r, C.g, C.b, std::min(1.0f, Alpha + 0.3f)));
                        const float Fl = std::max(0.15f, std::abs(std::cos(p.m_Phase * 2.3f + p.m_Rot)));
                        pGraphics->QuadsSetRotation(p.m_Rot);
                        IGraphics::CQuadItem Q(cx - s * 0.55f, cy - s * 0.3f * Fl, s * 1.1f, s * 0.6f * Fl);
                        pGraphics->QuadsDrawTL(&Q, 1);
                        pGraphics->QuadsSetRotation(0.0f);
                }
        }
        pGraphics->QuadsSetRotation(0.0f);
        pGraphics->QuadsEnd();
}

// Rain: light / heavy, thin slanted streaks (screen space)
static void VxRenderRain(IGraphics *pGraphics, float Dt, float W, float H)
{
        static std::vector<SVxDrop> s_vDrops;
        const bool Heavy = g_Config.m_VxRainHeavy != 0;
        const int Want = Heavy ? 520 : 140;
        while((int)s_vDrops.size() < Want)
        {
                SVxDrop D;
                D.m_X = VxRandF() * 1.4f - 0.2f;
                D.m_Y = VxRandF();
                D.m_Speed = Heavy ? 1.5f + VxRandF() * 0.8f : 0.8f + VxRandF() * 0.5f;
                D.m_Len = Heavy ? 0.035f + VxRandF() * 0.03f : 0.022f + VxRandF() * 0.02f;
                D.m_Alpha = 0.35f + VxRandF() * 0.4f;
                s_vDrops.push_back(D);
        }
        if((int)s_vDrops.size() > Want)
                s_vDrops.resize(Want);

        const float Slant = std::clamp(g_Config.m_VxRainSlant, 0, 100) / 100.0f * 0.45f; // dx/dy
        const ColorRGBA Base = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_VxRainColor));
        const float Thick = std::max(0.25f, H * (Heavy ? 0.0016f : 0.0012f));

        pGraphics->TextureClear();
        pGraphics->QuadsBegin();
        for(SVxDrop &D : s_vDrops)
        {
                                D.m_X -= g_VxCamDelta.x;
                D.m_Y -= g_VxCamDelta.y;
                if(D.m_X < -0.25f)
                        D.m_X += 1.5f;
                if(D.m_X > 1.25f)
                        D.m_X -= 1.5f;
                if(D.m_Y < -0.35f)
                        D.m_Y += 1.4f;
                D.m_Y += D.m_Speed * Dt;
                D.m_X += D.m_Speed * Slant * Dt * (H / W);
                if(D.m_Y > 1.05f)
                {
                        D.m_Y = -D.m_Len;
                        D.m_X = VxRandF() * 1.4f - 0.2f;
                }
                const float hx = D.m_X * W, hy = D.m_Y * H;
                const float tx = hx - D.m_Len * H * Slant, ty = hy - D.m_Len * H;
                const float dx = hx - tx, dy = hy - ty;
                const float Len = std::sqrt(dx * dx + dy * dy);
                if(Len < 0.001f)
                        continue;
                const float nx = -dy / Len * Thick * 0.5f, ny = dx / Len * Thick * 0.5f;
                pGraphics->SetColor(ColorRGBA(Base.r, Base.g, Base.b, D.m_Alpha));
                const IGraphics::CFreeformItem Q(tx - nx, ty - ny, tx + nx, ty + ny, hx - nx, hy - ny, hx + nx, hy + ny);
                pGraphics->QuadsDrawFreeform(&Q, 1);
        }
        pGraphics->QuadsEnd();
}

void VertexRenderParticles(IGraphics *pGraphics, float Dt, float ScreenW, float ScreenH)
{
        if(!g_Config.m_VxParticles && !g_Config.m_VxRain)
                return;
        if(ScreenW <= 0.0f || ScreenH <= 0.0f)
                return;
        Dt = std::clamp(Dt, 0.0f, 0.1f);

        if(g_Config.m_VxRain)
                VxRenderRain(pGraphics, Dt, ScreenW, ScreenH);
        if(g_Config.m_VxParticles)
                VxRenderHearts(pGraphics, Dt, ScreenW, ScreenH);
}

// ---------------- leaves (world space) ----------------
// The screen must already be mapped to the game group (so units are map units, a tee is ~28 units wide).
// Leaves fall with wind + sway + flip, stop on solid tiles (Collision()->CheckPoint), lie there for a while,
// and are kicked up when the local player runs through them.
void VertexRenderWorld(IGraphics *pGraphics, CCollision *pCollision, float Dt, vec2 PlayerPos, bool HasPlayer, float ViewX0, float ViewY0, float ViewX1, float ViewY1)
{
                {
                static vec2 s_PrevCenter = vec2(0.0f, 0.0f);
                static bool s_HavePrev = false;
                const vec2 Center((ViewX0 + ViewX1) * 0.5f, (ViewY0 + ViewY1) * 0.5f);
                g_VxCamDelta = vec2(0.0f, 0.0f);
                if(s_HavePrev && ViewX1 > ViewX0 && ViewY1 > ViewY0)
                {
                        g_VxCamDelta = vec2((Center.x - s_PrevCenter.x) / (ViewX1 - ViewX0), (Center.y - s_PrevCenter.y) / (ViewY1 - ViewY0));
                        if(std::abs(g_VxCamDelta.x) > 0.5f || std::abs(g_VxCamDelta.y) > 0.5f)
                                g_VxCamDelta = vec2(0.0f, 0.0f); // teleport / respawn
                }
                s_PrevCenter = Center;
                s_HavePrev = true;
        }
        if(!g_Config.m_VxLeaves || !pCollision)
                return;
        Dt = std::clamp(Dt, 0.0f, 0.1f);

        static std::vector<SVxWLeaf> s_vLeaves;
        static float s_Time = 0.0f;
        static vec2 s_PrevPlayer = vec2(0.0f, 0.0f);
        static bool s_HadPlayer = false;
        s_Time += Dt;

        static const ColorRGBA s_aPalette[5] = {
                ColorRGBA(0.93f, 0.46f, 0.10f, 1.0f), // orange
                ColorRGBA(0.80f, 0.20f, 0.10f, 1.0f), // red
                ColorRGBA(0.95f, 0.76f, 0.15f, 1.0f), // yellow
                ColorRGBA(0.55f, 0.32f, 0.12f, 1.0f), // brown
                ColorRGBA(0.85f, 0.55f, 0.10f, 1.0f)}; // amber

        // simulation area: the view enlarged by the "range" setting (leaves outside the view keep living, so nothing pops in)
        const float RangeK = std::clamp(g_Config.m_VxLeavesRange, 100, 400) / 100.0f;
        const float CtrX = (ViewX0 + ViewX1) * 0.5f, CtrY = (ViewY0 + ViewY1) * 0.5f;
        const float X0 = CtrX - (ViewX1 - ViewX0) * RangeK * 0.5f, X1 = CtrX + (ViewX1 - ViewX0) * RangeK * 0.5f;
        const float Y0 = CtrY - (ViewY1 - ViewY0) * RangeK * 0.5f, Y1 = CtrY + (ViewY1 - ViewY0) * RangeK * 0.5f;
        const float VW = X1 - X0, VH = Y1 - Y0;
        if(VW <= 1.0f || VH <= 1.0f)
                return;
        const float Mx = VW * 0.15f;

        vec2 PlayerVel = vec2(0.0f, 0.0f);
        if(HasPlayer && s_HadPlayer && Dt > 0.0001f)
        {
                PlayerVel = (PlayerPos - s_PrevPlayer) / Dt;
                PlayerVel.x = std::clamp(PlayerVel.x, -900.0f, 900.0f);
                PlayerVel.y = std::clamp(PlayerVel.y, -900.0f, 900.0f);
        }
        s_PrevPlayer = PlayerPos;
        s_HadPlayer = HasPlayer;

        const auto Respawn = [&](SVxWLeaf &L, bool AnyY) {
                for(int Try = 0; Try < 10; Try++)
                {
                        L.m_Pos.x = X0 - Mx + VxRandF() * (VW + 2.0f * Mx);
                        L.m_Pos.y = AnyY ? Y0 + VxRandF() * VH : Y0 - 10.0f - VxRandF() * 60.0f;
                        if(!pCollision->CheckPoint(L.m_Pos.x, L.m_Pos.y))
                                break;
                }
                L.m_Vel = vec2(0.0f, 25.0f);
                L.m_Rot = VxRandF() * 6.2831853f;
                L.m_Size = 0.7f + VxRandF() * 0.7f;
                L.m_Phase = VxRandF() * 6.2831853f;
                L.m_Flip = VxRandF() * 6.2831853f;
                L.m_FlipSpeed = 1.5f + VxRandF() * 3.0f;
                L.m_Life = 5.0f + VxRandF() * 10.0f;
                L.m_Age = 0.0f;
                L.m_Color = rand() % 5;
                L.m_Landed = false;
        };

        const int Want = std::clamp((int)((float)g_Config.m_VxLeavesCount * RangeK * RangeK), 1, 2000);
        while((int)s_vLeaves.size() < Want)
        {
                SVxWLeaf L;
                Respawn(L, true);
                s_vLeaves.push_back(L);
        }
        if((int)s_vLeaves.size() > Want)
                s_vLeaves.resize(Want);

        const float WindK = std::clamp(g_Config.m_VxLeavesWind, 0, 200) / 100.0f;
        const float SizeK = std::clamp(g_Config.m_VxLeavesSize, 20, 300) / 100.0f;
        const float Gust = std::max(0.0f, std::sin(s_Time * 0.23f + 1.3f)) * std::max(0.0f, std::sin(s_Time * 0.61f));
        const float Wind = (15.0f + 25.0f * std::sin(s_Time * 0.31f) + 15.0f * std::sin(s_Time * 0.97f + 2.0f) + 110.0f * Gust) * WindK;

        pGraphics->TextureClear();
        pGraphics->QuadsBegin();
        for(SVxWLeaf &L : s_vLeaves)
        {
                L.m_Age += Dt;

                // kicked up by the player
                if(HasPlayer)
                {
                        const vec2 d = L.m_Pos - PlayerPos;
                        if(d.x * d.x + d.y * d.y < 34.0f * 34.0f && (L.m_Landed || std::abs(PlayerVel.x) + std::abs(PlayerVel.y) > 120.0f))
                        {
                                L.m_Landed = false;
                                L.m_Life = 5.0f + VxRandF() * 8.0f;
                                L.m_Vel.x = PlayerVel.x * 0.35f + (VxRandF() - 0.5f) * 90.0f;
                                L.m_Vel.y = -(50.0f + VxRandF() * 90.0f) + PlayerVel.y * 0.2f;
                                L.m_FlipSpeed = 3.0f + VxRandF() * 4.0f;
                                L.m_Pos.y -= 2.0f;
                        }
                }

                float Fade = 1.0f;
                if(L.m_Landed)
                {
                                               if(!g_Config.m_VxLeavesPersist)
                        {
                                L.m_Life -= Dt;
                                Fade = std::clamp(L.m_Life, 0.0f, 1.0f);
                                if(L.m_Life <= 0.0f)
                                        Respawn(L, false);
                        }
                }
                else
                {
                        L.m_Phase += Dt * (2.0f + L.m_Size);
                        L.m_Flip += Dt * L.m_FlipSpeed;
                        const float Sway = std::sin(L.m_Phase);
                        const float Flat = std::abs(std::cos(L.m_Flip));
                        const float TargetVx = Wind * (0.5f + 0.4f * L.m_Size) + Sway * 28.0f;
                        const float TargetVy = 28.0f + 30.0f * (1.0f - Flat) + 8.0f * L.m_Size;
                        const float Relax = std::min(1.0f, Dt * 1.8f);
                        L.m_Vel.x += (TargetVx - L.m_Vel.x) * Relax;
                        L.m_Vel.y += (TargetVy - L.m_Vel.y) * Relax;
                        L.m_Rot += Dt * (Sway * 1.4f + L.m_Vel.x * 0.04f);

                        vec2 New = L.m_Pos + L.m_Vel * Dt;
                        if(pCollision->CheckPoint(New.x, L.m_Pos.y)) // wall: bounce a little
                        {
                                L.m_Vel.x = -L.m_Vel.x * 0.3f;
                                New.x = L.m_Pos.x;
                        }
                        if(pCollision->CheckPoint(New.x, New.y + 1.0f))
                        {
                                if(L.m_Vel.y > 0.0f)
                                {
                                        // land on the floor: lie flat, stay a while
                                        L.m_Landed = true;
                                        L.m_Vel = vec2(0.0f, 0.0f);
                                        L.m_Rot = (VxRandF() - 0.5f) * 0.9f + (VxRandF() < 0.5f ? 0.0f : 3.14159265f);
                                        L.m_Flip = 0.9f;
                                        L.m_Life = 4.0f + VxRandF() * 10.0f;
                                        New.y = L.m_Pos.y;
                                }
                                else
                                        New.y = L.m_Pos.y; // hit a ceiling
                        }
                        L.m_Pos = New;
                        // stuck inside a tile (e.g. camera jump): push out
                        for(int i = 0; i < 6 && pCollision->CheckPoint(L.m_Pos.x, L.m_Pos.y); i++)
                                L.m_Pos.y -= 1.5f;
                }

                // left the view: bring it back
                if(L.m_Pos.y > Y1 + 40.0f)
                        Respawn(L, false);
                else if(L.m_Pos.x < X0 - 2.0f * Mx || L.m_Pos.x > X1 + 2.0f * Mx || L.m_Pos.y < Y0 - 300.0f)
                        Respawn(L, false);
                                if(L.m_Pos.x < ViewX0 - 40.0f || L.m_Pos.x > ViewX1 + 40.0f || L.m_Pos.y < ViewY0 - 40.0f || L.m_Pos.y > ViewY1 + 40.0f)
                        continue; // simulated, but not drawn off-screen
                const float Fl = L.m_Landed ? 0.6f : std::max(0.12f, std::abs(std::cos(L.m_Flip)));
                const float Ln = 2.4f * SizeK * L.m_Size;
                const float Wd = 0.85f * SizeK * L.m_Size * Fl;
                const float Shade = 0.62f + 0.38f * Fl;
                const ColorRGBA &Pal = s_aPalette[L.m_Color];
                const ColorRGBA Col(Pal.r * Shade, Pal.g * Shade, Pal.b * Shade, 1.0f);
                const float Alpha = 0.95f * Fade * std::min(1.0f, L.m_Age * 2.0f);
                VxDrawLeaf(pGraphics, L.m_Pos.x, L.m_Pos.y, L.m_Rot, Ln, Wd, Col, Alpha, true);
        }
        pGraphics->QuadsEnd();
}