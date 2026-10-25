## Muon 1.1.0: оптика, свет и карта глубины

Скачайте `Muon-1.1.0-win64.zip`, распакуйте и запустите `Muon.exe`. Папку `depth` держите рядом с программой.

### Новое: «Эксперименты» → «Оптика и свет»
- **Карта глубины.** Нейросеть Depth Anything V2 определяет, что ближе к камере, а что дальше. Считается один раз на видеокарте (около 0,1 с) и запоминается. Карту можно посмотреть и поправить кистью.
- **Стекло:** призма, линза (собирающая, рассеивающая, цилиндр), стеклянный шар, стекло (гладкое, рифлёное, волнистое, матовое), капли воды. Настоящее преломление с дисперсией, отражения, блики и каустика.
- **Свет:** точечный источник, прожектор и луч. Источник висит перед тем, что под ним: ближнее освещено сильнее дальнего, тени ложатся по рельефу, затенённые места подсвечиваются, есть блики, мягкое затенение в складках, свечение и лучи в воздухе. Луч через призму раскладывается в радугу.
- **Атмосфера:** туман и размытие по глубине с выбором фокуса кликом по фото.
- **По глубине:** маску в классической вкладке и любой экспериментальный эффект можно ограничить передним или дальним планом.
- Всё это сохраняется в правках и пресетах (группа «Оптика, свет и глубина»).

### Требования
Windows 10 или 11 (64 бит), видеокарта с DirectX 11. Для быстрой карты глубины нужна видеокарта с DirectX 12, иначе она считается на процессоре примерно за полсекунды. Без папки `depth` программа работает как раньше, а свет и стекло считают сцену плоской.

---

## Muon 1.1.0: optics, light and depth map

New **Optics & light** section in the Experiments tab: prisms, lenses, glass spheres, clear, fluted, hammered or frosted glass and water drops with real refraction, dispersion and caustics; point lights, spotlights and beams with depth falloff, soft shadows over the relief, gloss and ambient occlusion (a beam through a prism makes a spectrum); fog and depth blur. A neural depth map (Depth Anything V2 Small via ONNX Runtime and DirectML, about 0.1 s on the GPU) makes shadows and light follow the scene, and masks and effects can now be limited by depth. Keep the `depth` folder next to `Muon.exe`; without it Muon works as before.
