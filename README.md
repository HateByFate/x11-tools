# x11-tools

Небольшие утилиты для X11-сессии (Debian + GNOME/mutter). Каждая утилита —
маленький C-бинарник: midmove зависит только от libX11, headbat грузит
GTK/appindicator через `dlopen` (заголовки не нужны).

## Утилиты

| Утилита | Что делает |
| --- | --- |
| [`midmove`](midmove/) | Двигает окно kitty средней кнопкой мыши: держишь кнопку и ведёшь указатель — окно едет, отпустил — остановилось. Короткий клик остаётся обычной средней кнопкой (вставка). |
| [`headbat`](headbat/) | Заряд наушников Logitech G435 в трее GNOME + уведомления о низком заряде. Читает по HID-протоколу донгла; опрос пропускается, пока играет звук (чтение глушит звук на ~0.6 с). |

## Сборка

Нужны `gcc` и заголовки X11:

```sh
sudo apt install build-essential libx11-dev
```

```sh
make            # собрать всё в bin/
make tests      # дополнительно собрать автотесты
make test       # поднять midmove и прогнать автотесты
make install    # поставить в ~/.local/bin и добавить автозапуск GNOME
make uninstall  # удалить установленное
make clean      # убрать bin/
make release    # собрать stripped-бинарники в release/ и deb-пакет в dist/
make deb        # собрать только deb: dist/x11-tools_<верс>_amd64.deb
```

## Установка

Из релиза (рекомендуется):

```sh
sudo dpkg -i dist/x11-tools_0.1.0_amd64.deb
sudo udevadm control --reload && sudo udevadm trigger
```

Пакет содержит бинарники в `/usr/bin`, автозапуск в `/etc/xdg/autostart`, udev-rule для
доступа к G435 в `/usr/lib/udev/rules.d`. Для доступа к донглу без root по-прежнему нужен
`sudo install -Dm644 headbat/70-headbat.rules /etc/udev/rules.d/70-headbat.rules`.

Из исходников:

```sh
make install
```

Обе утилиты умеют версию: `midmove -V` / `headbat -V` (или `--version`).

## Требования и ограничения

- Только **X11**. В Wayland-сессии (в Debian 13 GNOME по умолчанию работает на Wayland)
  эти приёмы не работают: там нет глобального перехвата кнопок и доступа к чужим окнам.
  Проверить сессию: `echo $XDG_SESSION_TYPE`.
- `midmove` во время работы использует `libXtst.so.6` (пакет `libxtst6`, в Debian стоит
  по умолчанию). Заголовки не нужны — библиотека подключается через `dlopen()`.
- Всё проверено на Debian 13, GNOME 48.7 / mutter 48.7, kitty 0.41.1.

## Структура

```
x11-tools/
├── Makefile                  сборка и установка всех утилит
├── midmove/                  утилита midmove
│   ├── midmove.c             исходник
│   ├── midmove.desktop.in    шаблон файла автозапуска
│   ├── README.md             подробное описание и принцип работы
│   └── tests/                автотесты (настоящие события мыши через XTest)
├── headbat/                  утилита headbat (заряд G435)
│   ├── headbat.c             демон: HID, трей, уведомления
│   ├── protocol.c/.h         протокол донгла и кривая процентов
│   ├── 70-headbat.rules      udev-rule для доступа к донглу
│   ├── headbat.desktop.in    шаблон автозапуска
│   ├── icons/                иконка наушников
│   ├── README.md             описание и протокол
│   └── tests/                автотесты протокола
└── bin/                      результаты сборки (в git не попадает)
```

## Лицензия

MIT, см. [LICENSE](LICENSE).
