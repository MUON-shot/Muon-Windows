MUON 1.1.0, фоторедактор RAW и JPEG для Windows
=================================================

Запуск
  Распакуйте архив в любую папку и запустите Muon.exe. Устанавливать ничего не нужно.

  Если Windows покажет «Windows защитила ваш компьютер», нажмите «Подробнее»,
  затем «Выполнить в любом случае». Так бывает с программами без платной
  цифровой подписи, это нужно сделать один раз.

Требования
  Windows 10 или 11, 64 бит, видеокарта с DirectX 11.
  Без видеокарты программа работает медленнее, на процессоре.

Папка depth
  Нейросеть для карты глубины (вкладка «Эксперименты» → «Оптика и свет»).
  Держите её рядом с Muon.exe. Без неё программа работает, но свет и стекло
  считают сцену плоской.

Где что хранится
  Правки:    рядом с фото, файл имя.ext.muon (исходники не меняются)
  Пресеты:   папка presets рядом с Muon.exe
  Настройки: %LOCALAPPDATA%\Muon

Горячие клавиши: F1 в программе.
Исходный код и вопросы: страница проекта на GitHub.


MUON 1.1.0, RAW and JPEG photo editor for Windows
=================================================

Unzip anywhere and run Muon.exe. Nothing to install.
If SmartScreen says "Windows protected your PC", click "More info", then "Run anyway"
(the program is not code-signed; this is needed only once).

Requirements: Windows 10 or 11 (64-bit), a DirectX 11 graphics card.
Edits are saved next to each photo in a .muon file; originals are never changed.
Keep the depth folder next to Muon.exe: it holds the neural network for the depth map
used by "Optics & light". Without it Muon still works and treats the scene as flat.
Press F1 in the program for keyboard shortcuts.

License: MIT. Third-party licenses are in THIRD_PARTY_NOTICES.md and the licenses folder.
