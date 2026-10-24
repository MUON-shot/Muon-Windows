<p align="center"><img src="res/muon.png" width="96" alt="Muon"></p>

<h1 align="center">Muon</h1>

<p align="center">Быстрый фоторедактор RAW и JPEG для Windows<br>
Один exe-файл на 2,4 МБ, без установки. Вся обработка идёт на видеокарте.</p>

<p align="center"><a href="#english">English below</a></p>

![Классическая правка](docs/screenshots/classic.jpg)

## Скачать

Откройте [Releases](../../releases), скачайте `Muon-1.0.0-win64.zip`, распакуйте в любую папку и запустите `Muon.exe`.

**Требования:** Windows 10 или 11, 64 бит. Подойдёт любая видеокарта с DirectX 11, то есть почти всё, что выпущено после 2010 года. Если видеокарты нет, Muon работает на процессоре через программный рендер Windows, медленнее, но работает. Ничего доустанавливать не нужно.

**Предупреждение Windows при первом запуске.** У программы нет платной цифровой подписи, поэтому SmartScreen может показать «Windows защитила ваш компьютер». Нажмите «Подробнее», затем «Выполнить в любом случае». Это нужно сделать один раз.

## Возможности

### Классика
- **RAW** почти всех камер (CR2, CR3, NEF, ARW, RAF, ORF, RW2, DNG, PEF и ещё около 20 форматов), а также JPEG, PNG, TIFF, BMP, WebP и HEIC.
- **Основные:** баланс белого (пипетка и авто), экспозиция, контраст, света, тени, белые, чёрные, текстура, чёткость, устранение дымки, красочность, насыщенность, авто-тон.
- **Цвет и тон:** тоновая кривая по RGB и каналам, HSL на 8 цветов, цветокоррекция теней, средних тонов и светов, чёрно-белое с микшером.
- **Детали:** резкость с маскированием, шумоподавление (non-local means), коррекция оптики, виньетка, зерно.
- **Кадр:** пропорции, поворот, выравнивание горизонта, перспектива, отражение.
- **Маски:** линейная, радиальная, кисть, диапазон яркости, цветовой диапазон. Внутри маски доступен свой набор коррекций.
- **Контроль пересветов:** зажмите Alt, двигая экспозицию, света, тени, белые или чёрные.

![Маски](docs/screenshots/masks.jpg)

### Эксперименты
Стек из 22 эффектов поверх основной обработки. Каждый слой можно ограничить диапазоном яркости, цветом или маской, поэтому ASCII можно оставить только в тенях, а неон только на синем.

ASCII, полутоновый растр и CMYK, дизеринг (Game Boy, CGA), плёночное зерно, свечение и ореол, сдвиг каналов, глитч, сортировка пикселей, живопись, неоновые контуры, градиентная карта, инфракрасная плёнка, тепловизор, засветка, постеризация, мозаика, VHS и кинескоп, калейдоскоп, тилт-шифт, искажения, замена цвета, ксерокопия.

![Эксперименты](docs/screenshots/experiments.jpg)

### Библиотека и пакетная работа
- Вкладка «Библиотека»: выберите папку, отметьте снимки (Ctrl, Shift, Ctrl+A) и импортируйте их в рабочую ленту.
- В ленте можно выбрать несколько фото и через правый клик скопировать настройки, вставить их, сбросить правки или экспортировать выбранные.
- Экспорт в JPEG (с лимитом размера файла), PNG или TIFF 8/16 бит, с изменением размера, резкостью для экрана или печати, водяным знаком и шаблоном имени файла.

![Библиотека](docs/screenshots/library.jpg)

### Пресеты, которыми удобно делиться
Пресет хранит только выбранные группы настроек и применяется только к ним. При наведении результат сразу виден на фото. Пресет можно сохранить в файл `.muonpreset` или скопировать как одну строку `MUON1-...` и отправить в любом мессенджере. Получатель нажимает «Вставить код». Код защищён контрольной суммой, лишний текст вокруг и переносы строк не мешают.

## Как это работает

- **Язык и графика.** Программа написана на C++17. Интерфейс сделан на [Dear ImGui](https://github.com/ocornut/imgui) и рисуется через Direct3D 11.
- **Декодирование.** RAW читает [LibRaw](https://www.libraw.org): демозаик, баланс белого камеры, линейные 16 бит без тоновой кривой. JPEG, PNG, TIFF и прочие форматы читает Windows Imaging Component, встроенный в систему.
- **Обработка на видеокарте.** Исходник загружается на видеокарту один раз. Каждая правка вызывает цепочку пиксельных шейдеров HLSL в 16-битном float: геометрия и оптика, шумоподавление, проявка (баланс белого, экспозиция, локальный тон в логарифмической шкале, мягкий «плечевой» перегиб светов, кривые, HSL, цветокоррекция), маски, резкость, виньетка и зерно, затем стек экспериментальных эффектов.
- **Превью и экспорт.** Превью считается в разрешении экрана, поэтому ползунки отзываются за миллисекунды. При увеличении и при экспорте та же цепочка считается в полном разрешении.
- **Видеокарта.** Muon сам выбирает самую мощную карту: на ноутбуке дискретную, а не встроенную. Выбрать вручную можно в меню «···» → «Видеокарта».
- **Правки.** Исходные файлы никогда не меняются. Правки хранятся рядом с фото в файле `имя.ext.muon`. Если папка защищена от записи, они хранятся в `%LOCALAPPDATA%\Muon\sidecars`. Отмена работает по шагам.
- **Особые RAW.** Режимы Nikon High Efficiency и High Efficiency★ используют закрытый кодек, который не читает ни одна открытая библиотека. Такие файлы открываются через встроенный в них полноразмерный JPEG, и программа об этом предупреждает.
- **Сбои.** Если Muon всё же упадёт, отчёт сохранится в `%LOCALAPPDATA%\Muon\crash`, а правки к этому моменту уже будут записаны.

## Горячие клавиши

| Клавиши | Действие |
|---|---|
| Ctrl+O / Ctrl+E | Открыть / экспорт |
| Ctrl+Z / Ctrl+Y | Отменить / повторить |
| Ctrl+Shift+C / V | Копировать / вставить настройки |
| G / D | Библиотека / классика |
| R / M | Кадр / маски |
| \\ | До и после |
| Z | Вписать или 100% |
| ← → | Предыдущее и следующее фото |
| Alt + ползунок | Показать пересветы и провалы |
| F1 | Все горячие клавиши |

## Сборка из исходников

Понадобятся Visual Studio 2022 с компонентом «Разработка классических приложений на C++», CMake и Git. Запустите `build.bat`: скрипт сам скачает LibRaw и Dear ImGui нужных версий и соберёт `Muon.exe` в корне проекта. CRT линкуется статически, поэтому exe не требует Visual C++ Redistributable.

Самопроверка всех стадий обработки: `Muon.exe --selftest фото.jpg папка_для_результатов`.

## Лицензия

Код Muon распространяется по лицензии MIT, см. [LICENSE](LICENSE). Сторонние библиотеки перечислены в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

<a name="english"></a>
## English

Muon is a fast RAW and JPEG photo editor for Windows. It ships as a single 2.4 MB exe with nothing to install, and all processing runs on the GPU.

![English interface](docs/screenshots/english.jpg)

**Download:** grab `Muon-1.0.0-win64.zip` from [Releases](../../releases), unzip and run `Muon.exe`.

**Requirements:** Windows 10 or 11, 64-bit, and any DirectX 11 graphics card. Without a usable GPU, Muon falls back to Windows' software renderer, which is slower but works. SmartScreen may warn on first launch because the exe is not code-signed: click "More info", then "Run anyway".

**What's inside:**
- **Classic tab.** A Lightroom-style develop module: white balance, tone, presence, curves, HSL, colour grading, B&W mixer, sharpening, noise reduction, lens corrections, crop and perspective, and masks (linear, radial, brush, luminance and colour ranges).
- **Experiments tab.** 22 stackable effects, each of which can be limited to a brightness range, a colour or a mask.
- **Library tab.** Imports photos into a working filmstrip with multi-select, copy and paste of settings, and batch export.
- **Shareable presets.** Save them as `.muonpreset` files or as one-line `MUON1-...` codes.

**How it works:**
- **Decoding.** LibRaw decodes RAW files to linear 16-bit; other formats use the Windows Imaging Component.
- **GPU pipeline.** A chain of HLSL pixel shaders runs in 16-bit float: geometry, noise reduction, develop, masks, sharpening and effects.
- **Preview and export.** The preview renders at screen resolution and export at full resolution.
- **Non-destructive edits.** Edits are stored next to each photo in a `.muon` sidecar; originals are never modified.

**Building:** install Visual Studio 2022 (C++ desktop workload), CMake and Git, then run `build.bat`.

**License:** MIT. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for LibRaw (CDDL 1.0) and Dear ImGui (MIT).
