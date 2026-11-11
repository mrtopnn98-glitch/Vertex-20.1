/* Vertex client — Vertex Aim System v2 (simple & working).
 *
 * Переписан с нуля по фидбеку: никакой "дальности", никаких режимов
 * Silent/Smooth — только честные простые функции:
 *
 *  - ЗОНА ЗАХВАТА = FOV (1..360°) вокруг текущего прицела.
 *    360° = захват во все стороны (хук/удар "спиной" работают).
 *    Радиус зоны растёт вместе с FOV и рисуется на экране (Draw FOV):
 *    если цель в нарисованной зоне — она будет взята.
 *    Отдельной настройки "дальность" больше НЕТ.
 *  - ХУК: берёт кого угодно (и Dummy тоже). Работает при нажатии крюка
 *    (тихо подставляет прицел) и в авто-режиме (сам крюкает цель в зоне).
 *    Путь крюка проверяется на стену — глухие цели не берутся.
 *  - МОЛОТ: реально бьёт — цель должна быть в физическом радиусе удара
 *    (молот достаёт на ~63px, дальше не попадёт — это физика, не настройка).
 *    360° = бьёт во все стороны. Есть авто-удар.
 *  - ПИСТОЛЕТ/ГРАНАТА: честная баллистика через CalcPos с кривизной и
 *    скоростью серверных тюнингов + упреждение по скорости цели.
 *  - ДРОБОВИК: на DDRace-картах это луч (как лазер) — прямой аим + ОТСКОК;
 *    на vanilla — обычные пули с баллистикой.
 *  - ЛАЗЕР: прямой аим + ОТСКОК ("аим на отскок"): перебирает направления
 *    и находит то, чей отражённый луч попадёт в цель (свой FOV у отскока).
 *    Отражения считаются ровно как CLaser::DoBounce.
 *  - ESP ТРАЕКТОРИЙ: от твоего прицела по физике рисуется полёт
 *    гранаты / дробаша / лазера со всеми отскоками и точка попадания.
 *  - ПОДСВЕТКА: если аим залочен на игрока (зависит от FOV хука) — он
 *    подсвечивается выбранным тобой цветом.
 *
 * Интеграция (всё уже вшито в полный controls.cpp из этого набора):
 *  - CControls::SnapInput, ПОСЛЕ Avoid Freeze + Auto Aled:
 *        GameClient()->m_AimBot.Update(GameClient(), Client(), this, &m_aInputData[g_Config.m_ClDummy]);
 *  - CControls::OnRender (конец):
 *        GameClient()->m_AimBot.RenderBinds(GameClient());
 *  - players.cpp (мировой оверлей — рядом с остальным Vertex ESP):
 *        GameClient()->m_AimBot.RenderWorld(GameClient());
 *
 * Приоритет ввода: Dummy Unfreeze → Avoid Freeze → Auto Aled → Aim.
 * Аим выключен — ввод вообще не трогается. Настройки: vx_aim_*.
 * Debug log tag: "vertex-aim" (vx_aim_debug 1).
 */
#ifndef GAME_CLIENT_AIMBOT_H
#define GAME_CLIENT_AIMBOT_H
#include <base/vmath.h>
class CGameClient;
class IClient;
class CControls;
class CNetObj_PlayerInput;

class CAimBot
{
public:
	enum
	{
		BIND_NONE = 0,
		BIND_HOLD = 1, // захват клавиши "aim только при удержании"
	};

	// раз в тик из CControls::SnapInput (после Avoid Freeze / Auto Aled)
	void Update(CGameClient *pGame, IClient *pClient, CControls *pControls, CNetObj_PlayerInput *pSlot);
	// раз в кадр из CControls::OnRender: захват клавиши бинда
	void RenderBinds(CGameClient *pGame);
	// мировой оверлей из players.cpp: зоны FOV, ESP траекторий, подсветка цели
	void RenderWorld(CGameClient *pGame);
	// полный сброс (реконнект / смена карты)
	void Reset();

	int m_BindMode = BIND_NONE;

	// виртуальный "слот" крюка для m_AimWeapon
	static constexpr int AB_HOOK = 100;

	// живое состояние (для меню / рендера)
	bool m_Aiming = false; // в этом тике аим навёлся и держит прицел
	int m_TargetId = -1; // цель в зоне (подсветка)
	int m_AimWeapon = -1; // чем наведены (WEAPON_* или AB_HOOK)
	vec2 m_AimPoint = vec2(0.0f, 0.0f); // куда наведён прицел
	vec2 m_PredPoint = vec2(0.0f, 0.0f); // предсказанная точка попадания

private:
	// параметры зоны для рендера (пишутся в Update каждый тик)
	int m_HookFovDeg = 0; // FOV зоны хука (0 = хук выключен)
	float m_HookZoneR = 0.0f;
	int m_WepFovDeg = 0; // FOV зоны текущего оружия (0 = не рисовать)
	float m_WepZoneR = 0.0f;
	vec2 m_ZonePos = vec2(0.0f, 0.0f); // центр зон (моя позиция)
	float m_ZoneAng = 0.0f; // направление прицела для клина FOV
	int m_VisUntil = -1000000; // пока рисовать линию аима
	vec2 m_From = vec2(0.0f, 0.0f);
	bool m_FireOwned = false; // авто-огонь: мы сами держим счётчик огня
	int m_LastWorkTick = -1000000;
	int64_t m_BindBlockUntil = 0;
	int64_t m_NextDrawTime = 0;
	int m_LogGateTick = -1000000;
};
#endif
