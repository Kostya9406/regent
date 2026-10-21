# Regent

[English](README.md)

Диспетчер задач и системный монитор для Linux, сделанный под Omarchy и Hyprland: управляется с клавиатуры,
знает окна и рабочие столы Hyprland, systemd-юниты и cgroup, сам замечает неполадки и записывает, что к ним
привело. Qt 6 Widgets / C++20, цвета и шрифт текущей темы Omarchy.

Главное:

- **Ctrl+K** — палитра команд: страницы, все команды, процессы («kil fir» → Kill firefox), службы («rst blu»).
- **Окна Hyprland**: колонки «Рабочий стол» и «Окно», фокус окна (`f`), перенос на рабочий стол, закрытие;
  «Pick a window» (Ctrl+Shift+W) — переключитесь на любое окно, и Regent покажет его процесс;
  `regent --active-window` на горячей клавише делает то же для окна в фокусе.
- **Vim-клавиши** во всех списках: `j k h l`, `gg G`, `Ctrl+D/U`, `/` — фильтр, `Esc` — обратно в список;
  в Processes `x` завершает, `X` убивает, `f` фокусирует окно, `Enter` — свойства.
- **Оповещения**: перегрев, троттлинг, нехватка памяти (PSI), почти полный диск, упавшие юниты. Значок в строке
  состояния и бар краснеют, уведомление приходит через `omarchy-notification-send` (клик открывает нужную страницу).
- **Units**: CPU, память, диск и число задач по slice/service/scope из учёта cgroup v2 — «весь Chrome» одной
  строкой; остановка, перезапуск, заморозка и разморозка.
- **Загрузка любой видеокарты**: у amdgpu — её собственный счётчик, у NVIDIA — через `nvidia-smi`, у Intel (i915, xe)
  и Arm — по времени движков DRM-клиентов ваших процессов; спящую дискретную карту программа не будит.
- **Процессы глубже**: GPU по процессам (DRM fdinfo: i915, amdgpu, xe), пробуждения в секунду, приоритет
  ввода-вывода (ionice), OOM-приоритет, заморозка приложения целиком (cgroup freezer).
- **Services** показывает журнал выбранной службы, ошибки красным (Ctrl+J).
- **Benchmarks** записывает к каждой оценке условия: профиль питания, сеть или батарея, температура до и после,
  средняя частота, фоновая загрузка, троттлинг. Тест на троттлинг грузит все ядра 30–300 с и рисует график
  производительности, частоты и температуры с выводом («Throttles: 24% slower by the end, from 18 s…»).
- **Power & Freq**: переключатель профиля power-profiles-daemon и процессы, которые чаще всех будят CPU.
- **Flight Recorder** отмечает на графике моменты с высокой нагрузкой, жарой и давлением памяти и называет
  вероятного виновника; сравнивает с сохранённой сессией; экспортирует HTML-отчёт (без командных строк);
  по желанию записывает сам, начиная за две минуты до неполадки.

Опасные действия ограничены: композитор, оболочку, polkit, порталы, сессию и сам Regent нельзя
приостановить или заморозить, а их остановка и завершение требуют отдельного подтверждения.

## Установка

На Arch и Omarchy — пакетом со [страницы релиза](https://github.com/Kostya9406/regent/releases/latest):

```bash
sudo pacman -U https://github.com/Kostya9406/regent/releases/download/v0.6.0/regent-0.6.0-1-x86_64.pkg.tar.zst
```

Или собрать тот же пакет из PKGBUILD:

```bash
git clone https://github.com/Kostya9406/regent && cd regent/packaging && makepkg -si
```

Или собрать вручную. Нужны `qt6-base` и компилятор C++20 (cmake не требуется):

```bash
mkdir -p build && cd build
qmake6 .. && make -j$(nproc)
./regent
make install        # ~/.local/bin/regent, значок и ярлык в ~/.local/share
```

Для всей системы: `qmake6 PREFIX=/usr .. && make && sudo make install`. Без `PREFIX=/usr` команда `sudo make install` положит файлы root в ваш `~/.local`.

По желанию: `ffmpeg` (видеозапись страницы), `power-profiles-daemon` (переключатель профиля), `hwdata`
(названия PCI/USB на странице Drivers). Hyprland и Omarchy используются, если есть: без Hyprland нет функций окон,
без Omarchy уведомления идут любому freedesktop-демону уведомлений.

Отладочная сборка с AddressSanitizer:

```bash
mkdir -p build-debug && cd build-debug
qmake6 .. CONFIG+=debug CONFIG-=release QMAKE_CXXFLAGS+=-fsanitize=address QMAKE_LFLAGS+=-fsanitize=address
make -j$(nproc)
```

## Страницы

| Страница | Что показывает |
|---|---|
| Summary | пиксельная панель OMARCHY, шкалы CPU / Memory / GPU / Thermal, карта ядер, PSI, сеть, диски, топ процессов |
| Performance | **Live**: CPU (общий график и сетка ядер), память, энергия, температуры, диски, сеть, GPU. **Sensors & power**: частоты, hwmon, батарея, RAPL, политика cpufreq, профиль питания, самые частые пробуждения |
| Processes | `/proc/<pid>/*` с группировкой по приложениям (окна Hyprland, потом фоновые), по пользователям или деревом по ppid; GPU, пробуждения, рабочий стол и окно |
| System | **Overview**: DMI, os-release, процессор, память, графика, накопители, питание, сеть, источники данных. **Devices**: устройства PCI/USB и их драйверы, модули ядра |
| Startup apps | что запускает Hyprland (ваш `~/.config/hypr/autostart.lua`, строки которого Regent умеет выключать и включать, и автозапуск самого Omarchy) и XDG autostart |
| Services | systemd по D-Bus, системная и пользовательская шина, Start / Stop / Restart через polkit, журнал |
| Units | дерево cgroup v2: ресурсы по slice / service / scope, Stop / Restart / Freeze / Thaw |
| Connections | TCP/UDP/Unix из `/proc/net/*` и их владельцы, класс доступности, приборная панель трафика |
| Installed Apps | локальная база pacman и `.desktop`-файлы пакетов |
| Disk Space | пиксельная карта занятого места, сканер в отдельном потоке, treemap, типы и крупнейшие файлы |
| Benchmarks | CPU (одно ядро и все ядра), тест на троттлинг, память, диск (кэш или direct I/O мимо кэша), OpenGL, загрузка по URL; условия каждого замера, история, CSV |
| Flight Recorder (нижняя панель) | история снимков, отмеченные моменты, сравнение, HTML-отчёт, автозапись |

У каждого списка два вида, переключаются клавишей `v` и запоминаются для каждой страницы. **Строки** — вид по
умолчанию для длинных списков (процессы, службы, соединения, датчики, пакеты, устройства): одна строка
на запись и столько столбцов, сколько помещается; щелчок по заголовку столбца сортирует по нему. **Карточки**
показывают все столбцы целиком (длинные значения переносятся до шести строк, полностью — во всплывающей подсказке),
поля стоят на одних и тех же местах во всех карточках. Части страниц переключаются Alt и подчёркнутой буквой
(Performance: Alt+L, Alt+S; System: Alt+O, Alt+D). Иерархия — отступами и разделами (`▸/▾`, Right/Left или `l/h`), сортировка — `s`,
Ctrl+G или щелчок по «SORT …», Ctrl+щелчок и Shift+щелчок выделяют несколько. Таблица остаётся скрытой
моделью: клавиши страницы (`x`, Delete, Enter, Menu) и контекстное меню работают с выделением в обоих видах.

## Клавиатура

| Клавиши | Действие |
|---|---|
| Ctrl+K (Ctrl+P) | палитра команд |
| Ctrl+1 … Ctrl+9, Ctrl+Tab / Ctrl+Shift+Tab | страницы |
| j k h l, gg G, Ctrl+D / Ctrl+U | движение по любому списку |
| / или Ctrl+F, Esc | фильтр страницы и обратно в список |
| x / X, Delete / Shift+Delete | завершить (SIGTERM) / убить (SIGKILL, с подтверждением) |
| f, Ctrl+Enter | фокус окна процесса |
| Ctrl+Shift+W | найти процесс окна: переключитесь на нужное окно |
| Enter, Menu | свойства процесса, контекстное меню |
| Ctrl+J | журнал службы (Services) |
| Ctrl+Shift+E | HTML-отчёт Flight Recorder |
| Ctrl+Shift+F, Ctrl+Shift+H / S / O | Flight Recorder: запись, история, сохранить как, открыть |
| Ctrl+G, s | сортировка списка |
| v | строки или карточки |
| Ctrl+B | боковая панель |
| F5, Ctrl+N, Ctrl+Shift+R, Ctrl+Shift+P | обновить, новая задача, видеозапись страницы, страница в отдельном окне |
| Ctrl+R | Disk Space: сканировать |
| Ctrl+, , F10, F1 | настройки, все команды, о программе |

Привязки для Hyprland (Omarchy, `~/.config/hypr/bindings.lua`):

```lua
o.bind("SUPER + CTRL + T", "Regent", "regent")
o.bind("SUPER + CTRL + I", "Process of this window", "regent --active-window")
o.bind("SUPER + CTRL + K", "Pick a process", "regent menu")
```

## Командная строка

```
regent                          открыть окно или поднять уже запущенное
regent --page processes         то же, сразу на нужной странице
regent --select-pid 1234        Processes с выбранным процессом
regent --active-window          Processes на процессе окна в фокусе
regent menu                     выбрать процесс в меню Omarchy (или walker/fuzzel/wofi/rofi) и действие с ним
regent top [-n 15] [--sort cpu|mem|gpu|wakeups]
regent kill NAME|PID… [--force] [--all]
regent --json [--fields cpu,mem,temp]          один снимок в JSON
regent --stream 2000 [--notify] [--fields …]   JSON-строка каждые 2000 мс (не чаще 250), с оповещениями
QT_QPA_PLATFORM=offscreen regent --capture-pages DIR   PNG всех страниц
```

Второй запуск передаёт команду первому через сокет `$XDG_RUNTIME_DIR/regent-instance-v1` и выходит.
Схема JSON — `src/core/Json.h` (`"schema": 1`), ключи только добавляются. `--stream` добавляет `alerts`
и `alertLevel`. Псевдонимы `--fields`: `mem`, `temp`, `net`, `gpu`, `top`, `procs`, а также пути через точку.

## Оповещения

Пороги задаются в настройках (Ctrl+,) и общие для окна и бара: температура CPU (по умолчанию 90 °C,
10 секунд), давление памяти PSI (20 %), свободное место (5 %), троттлинг, упавшие юниты, по желанию —
долгая загрузка CPU. Юниты, упавшие ещё до запуска, считаются фоном; оповещение — когда юнит падает позже
(для падающего по кругу — не чаще раза в 10 минут). Уведомления отправляет один процесс Regent на сессию —
тот, что держит `$XDG_RUNTIME_DIR/regent-alerts-UID.lock` (обычно `--stream --notify` бара).

## Виджет бара Omarchy

Виджет живёт в отдельном репозитории [omarchy-regent](https://github.com/Kostya9406/omarchy-regent):
CPU и память, температура, сеть или самый нагруженный процесс (колесо мыши переключает), красный при оповещениях.
Клик открывает Regent (при оповещении — нужную страницу), правый клик — Processes, средний — `regent menu`.

```bash
omarchy plugin add https://github.com/Kostya9406/omarchy-regent.git --enable
```

## Устройство

```
src/core/      сбор данных и действия, без GUI
  Sampler        снимок procfs/sysfs, окна Hyprland, GPU и пробуждения по процессам
  SamplingEngine Sampler в отдельном QThread, публикует SnapshotPtr в GUI
  Snapshot.h     неизменяемый снимок, который читают все страницы
  Hyprland       сокет Hyprland: окна, фокус, перенос, закрытие (Lua-диспетчеры)
  Cgroups        учёт cgroup v2 по юнитам
  Alerts, Notify пороги с удержанием и гистерезисом, уведомления
  Actions        сигналы через pidfd, nice, affinity, ionice, OOM, защита критичных процессов и юнитов
  Services       systemd по D-Bus, в том числе Freeze/Thaw и упавшие юниты
  FlightRecorder, FlightReport   история снимков, отмеченные моменты, HTML-отчёт
  Cli            top / kill / menu / --fields
  Startup, Inventory, HwIds, Connections, DiskScan, Benchmarks, Json, Settings
src/ui/        оболочка (MainWindow), палитра команд, Vim-клавиши, тема, рисованные виджеты, страницы
packaging/     regent.desktop, regent.svg, PKGBUILD
tests/         проверки, см. tests/README.md (все разом — tests/run-all.sh)
```

## Лицензия

MIT, см. [LICENSE](LICENSE).
