VERTEX AIM SYSTEM V2 — УСТАНОВКА В C:\Vertex-20.1

ПАКЕТ ВКЛЮЧАЕТ ПРИСЛАННЫЕ ПОЛНЫЕ ФАЙЛЫ:
- aimbot.h / aimbot.cpp — новый Vertex Aim System v2;
- controls.cpp — полный controls.cpp из твоего набора;
- menus_settings_vertex.cpp — полный новый GUI (8 вкладок), плюс определения рендереров погоды/дождя/частиц/листьев;
- config_aim_additions.txt — 34 новые vx_aim_* настройки;
- Install-VertexAimV2.ps1 — установщик с предварительной проверкой, нормализацией переносов строк и резервными копиями.

УСТАНОВКА:
1. Распакуй папку Vertex_AimV2_Install в C:\Vertex-20.1.
2. Открой PowerShell в C:\Vertex-20.1.
3. Выполни:

powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Vertex_AimV2_Install\Install-VertexAimV2.ps1 -ProjectRoot C:\Vertex-20.1

Скрипт сначала проверяет зависимости и готовит все правки в памяти. Если проверка не проходит, он останавливается ДО записи исходников и показывает причину. Если проходит — создаёт резервные копии *.before_aimv2_<timestamp>.bak и устанавливает файлы.

АДАПТАЦИИ К ТЕКУЩЕМУ VERTEX:
- Auto Aled в этой ветке использует singleton VertexAutoAled(), а не CGameClient::m_AutoAled. Установщик подключает правильный заголовок и добавляет IsBusy() к существующему классу.
- Существующий AvoidFreeze сохраняется. Вызов из нового controls.cpp адаптируется к текущей сигнатуре Update(GameClient(), Client(), slot), а захват биндов — к RenderBinds(GameClient()). IsBusy() добавляется как тонкая обёртка над текущим AnyActive().
- Дублирующий VertexAvoidRender() из нового controls.cpp убирается: существующий AvoidFreeze::RenderWorld() уже вызывается из players.cpp.
- Старый src/game/client/components/aimbot.cpp/.h удаляется только из списка сборки CMake, если он там перечислен, чтобы не собирать две реализации CAimBot. Исходные файлы не удаляются.
- Новый меню-файл ставится целиком. В нём есть определения VertexRenderParticles() и VertexRenderWorld(), нужные вызовам из hud.cpp. Сам hud.cpp не отключается и не меняется.
- В gameclient.h добавляется CAimBot, в controls.h — m_VertexAimTargetId при отсутствии, в players.cpp — Aim RenderWorld.
- Новые vx_aim_* строки добавляются в config_variables.h и, если в этой ветке есть отдельный ручной список, в config.cpp. Старые ClHookAim, ClVertexAimbot* и VxEsp* не удаляются.
- Если в конфиге нет трёх необязательных параметров Auto Aled (SafeTicks/Cooldown/MaxPresses), соответствующие необязательные строки-ползунки меню пропускаются; остальные отсутствующие поля приводят к остановке до записи.

СБОРКА НЕ ЗАПУСКАЕТСЯ АВТОМАТИЧЕСКИ.
После сообщения об успехе собери привычную папку:

cmake --build .\build --config Release --parallel 4

Запуск:

Start-Process .\build\Release\DDNet.exe

Если установщик остановится на preflight, пришли полный текст ошибки — проект к этому моменту ещё не будет изменён.
