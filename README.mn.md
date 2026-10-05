# aegidub

**aegidub** бол [Aegisub](https://github.com/TypesettingTools/Aegisub) дээр суурилсан, хиймэл оюун ухаантай дубляжийн програм. Видео болон хадмалыг аваад дубляж хийсэн видео болгоно. Мөрүүдийг орчуулж, мөр бүрийг хэн хэлж байгааг тогтоож, дүр бүрт хоолой оноож, сэтгэл хөдлөлийг найруулж, [ElevenLabs](https://elevenlabs.io)-ээр яриулаад, дубляжийг видеоны өөрийн арын дуун дээр холино.

Кино, цувралыг **монгол хэл** рүү дубляж хийхэд зориулагдсан. Гэхдээ таны AI загвар болон ElevenLabs-ийн хоолой дэмждэг ямар ч хэлээр ажиллана.

[Read in English](README.md)

## Aegisub-тай ижил тал

aegidub нь Aegisub-ийн бүх боломжийг хэвээр хадгалдаг. Aegisub мэддэг бол aegidub-ийг мэднэ гэсэн үг.

- Хадмал засварлагч нь ижил: мөрийн хүснэгт, засах талбар, цаг тааруулах, style, аудионы долгион ба спектр, libass-аар хадмал зурдаг видео тоглуулагч, visual typesetting, караоке, Lua/MoonScript automation, Aegisub-ийн уншиж бичдэг бүх формат.
- `.ass` файл хоёр талдаа нийцтэй. aegidub-ийн нэмдэг мэдээлэл (хоолой, сэтгэл хөдлөл, эх бичвэр) нь Aegisub хадгалдаг ч хэрэглэдэггүй хэсэгт бичигддэг. Тиймээс aegidub-ийн файлыг Aegisub болон бусад тоглуулагч хэвийн нээнэ.
- Товчлол, цэс, тохиргоо ижил, дээр нь шинэ зүйлс нэмэгдсэн.
- Лиценз болон зохиогчид ижил. aegidub нь fork тул Aegisub-ийн зохиогчид зохиогчийн эрхээ хадгална.

## Ялгаатай тал

| Боломж | Юу хийдэг вэ |
|---|---|
| **Projects** | CapCut шиг нүүр дэлгэц. Видео бүр зурагтай карт, огноо болон hash-аар нэрлэгдэнэ, жишээ нь `20261005-a1b2c3d4e5f6`. Project-ийн хавтсанд хадмал, үүсгэсэн яриа, render хийсэн видео хадгалагдана. |
| **AI Translate** | OpenAI-тай нийцтэй загвараар орчуулна. Эх бичвэр *Original* баганад үлдэх тул сэргээх эсвэл дахин орчуулах боломжтой. |
| **AI Detect Speakers** | Харилцан яриа болон цувралын дүрүүдээс мөр бүрийг хэн хэлж байгааг тааж *Character* баганыг бөглөнө. |
| **Voice Cast** | Дүр бүрт ElevenLabs-ийн хоолой онооно. *Auto-detect* нь ElevenLabs монгол хэлээр баталгаажуулсан хоолойнуудаас хүйс, нас, зан чанарт тохируулж сонгоно. *Play* нь тухайн дүрийн өөрийн нэг мөрийг сонсгоно. |
| **Цувралын cast** | Цувралын бүх ангид нэг `.cast.json` файл. Ингэснээр дүрүүд анги бүрт ижил хоолойтой байна. |
| **AI Detect Emotions** | *Emotion* баганыг `[sad]`, `[whispers]` гэх мэт ElevenLabs-ийн audio tag-аар бөглөнө. Нүдэн дээр дарж солино. |
| **Уншигдах бичвэр (Spoken)** | Тоо, тэмдэгт, гадаад үгийг яриа үүсгэгчид зориулж үгээр бичнэ (`10:30` → "арван цаг гучин минут"). Дэлгэц дээр `10:30` хэвээр харагдана. |
| **Generate Dub Track** | Мөр бүрийг дүрийн хоолой, сэтгэл хөдлөлөөр яриулж, хадмалын цагт тааруулсан нэг аудио track болгоно. Өөрчлөгдөөгүй мөрийг дахин ашигладаг тул зөвхөн өөрчилсөн хэсэгтээ төлбөр төлнө. |
| **Fit багана** | CPS шиг, гэхдээ бодит ярианы уртаар хэмжинэ: дараагийн мөр эхлэх хүртэлх хугацааны хэдэн хувийг эзэлж байгааг харуулна. Улаан бол давж байна гэсэн үг. |
| **AI Shorten Long Lines** | Багтахгүй мөрүүдийг утгыг нь хадгалан богиносгоно. Мөр бүр одоо хэр урт, хэр урт байх ёстойг AI-д хэлж өгнө. |
| **Render Dubbed Video** | Видеоны ярианы дууг хөгжим, эффектээс **htdemucs**-ээр салгана. Дубляжийг арын дуун дээр холино (хүсвэл жинхэнэ яриаг аяархан үлдээнэ), чангыг remaster хийж болно. Дубляжийг үндсэн track болгосон шинэ видео гаргана. |
| **Дотоод хоолой салгагч** | htdemucs нь програм дотор CPU дээр ажилладаг тул юу ч суулгах шаардлагагүй. Python + demucs болон NVIDIA GPU байвал түүнийг ашиглаж ~10 дахин хурдан ажиллана. |
| **Шинэчлэл** | Зөвхөн энэ repo-ийн GitHub release-ийг шалгана. Шинэчлэхээсээ өмнө заавал асууна. Таны компьютерын тухай ямар ч мэдээлэл илгээхгүй. |

## Дубляжийн дараалал

1. **Projects → New project**: видеогоо сонгоод, байгаа бол хадмалаа импортлоно.
2. **AI Translate**: хадмал таны хэл дээр биш бол.
3. **AI Detect Speakers**, дараа нь *Character* баганын буруу нэрсийг засна.
4. **Voice Cast → Auto-detect voices**, *Play*-ээр сонсож шалгаад цувралын cast-ыг хадгална.
5. **AI Detect Emotions**, дараа нь **Play Dub of Line**-ээр сонсож *Emotion* баганыг тааруулна.
6. **Generate Dub Track**, *Fit* баганад улаан байвал **AI Shorten Long Lines**.
7. **Render Dubbed Video**.

## Суулгах

[Releases](https://github.com/tuvshinorg/aegidub/releases) хуудаснаас `aegidub-…-x64-setup.exe`-г татаж ажиллуулна. Компьютерт Microsoft Visual C++ runtime байхгүй эсвэл хуучин бол installer өөрөө суулгана.

Шаардлага:

- Windows 10 эсвэл 11, 64-bit. Дотоод хоолой салгагчид AVX2 дэмждэг процессор хэрэгтэй (2013 оноос хойшхи ихэнх процессор).
- **OpenAI** API түлхүүр (эсвэл OpenAI-тай нийцтэй үйлчилгээ): орчуулга, яригч, cast, сэтгэл хөдлөл, богиносгоход. *Preferences → AI Translation*.
- **ElevenLabs** API түлхүүр: хоолойд. *Preferences → Voice Cast*.
- Заавал биш, хурдан салгахад: NVIDIA GPU болон `demucs`, `torch` (CUDA), `soundfile` суусан Python.

Хоолой салгах загвар (84 MB) анх хэрэгтэй болоход нэг л удаа татагдана.

## Нууцлал ба зардал

- API түлхүүрүүд зөвхөн таны өөрийн тохиргоонд (`%APPDATA%\aegidub\config.json`) эсвэл орчны хувьсагчид (`OPENAI_API_KEY`, `ELEVENLABS_API_KEY`) хадгалагдана. Програм болон project-ийн файлд хэзээ ч ордоггүй.
- AI-тай боломжууд хадмалын бичвэрийг таны тохируулсан AI үйлчилгээ рүү, хоолойн боломжууд ElevenLabs руу илгээдэг. Хоёулаа хэрэглээгээр төлбөр авдаг. aegidub багц бүрийн өмнө асуух бөгөөд үүсгэсэн яриаг дахин ашигладаг тул нэг зүйлд хоёр удаа төлөхгүй.
- Render болон хоолой салгалт таны компьютер дээр хийгдэнэ.

## Эх кодоос build хийх (Windows)

Хэрэгтэй: Windows SDK-тай Visual Studio 2022 (эсвэл Build Tools), Python 3, Meson (`pip install meson`). Ninja нь Visual Studio-той хамт ирдэг.

"x64 Native Tools Command Prompt" дээрээс:

```
meson setup build-release --buildtype=release -Ddefault_library=static
ninja -C build-release aegidub.exe
```

FFmpeg, wxWidgets, Eigen, htdemucs-ийн C++ хувилбар зэрэг бусад бүх хамаарлыг Meson өөрөө татаж build хийнэ.

Installer хийхдээ [Inno Setup 6](https://jrsoftware.org/isinfo.php) суулгаад:

```
powershell tools\build-aegidub-installer.ps1
```

### Release гаргах

Хувилбарын tag push хийхэд үлдсэнийг нь GitHub Actions хийнэ:

```
git tag v1.0.0
git push origin v1.0.0
```

*Release* workflow нь tag-аас aegidub-ийг build хийнэ (ингэснээр програм өөрийн хувилбарыг мэднэ), installer хийгээд түүнийг хавсаргасан GitHub release нийтэлнэ. Суулгасан aegidub-ууд дараа нь шинэчлэлийг санал болгоно.

Програм болон installer-т гарын үсэг зурж, Windows SmartScreen "үл мэдэгдэх нийтлэгч" гэж анхааруулахгүй болгохын тулд repo-д хоёр secret нэмнэ: `WINDOWS_SIGN_PFX_BASE64` (code signing гэрчилгээгээ base64 болгосон `.pfx`) болон `WINDOWS_SIGN_PFX_PASSWORD`. Компьютер дээрээ бол installer-ийн script-ийг ажиллуулахаас өмнө `SIGN_PFX`, `SIGN_PFX_PASSWORD`-ийг тохируулна. Гэрчилгээгүй бол бүх зүйл гарын үсэггүйгээр build хийгдэнэ.

Logo солих бол `docs/art-sources/aegidubLogo.png`-г солиод `python tools/generate_logo_assets.py` ажиллуулна.

## Лиценз ба талархал

aegidub нь [Aegisub](https://github.com/TypesettingTools/Aegisub) дээр суурилсан. © 2005–2026 Rodrigo Braz Monteiro, Niels Martin Hansen, Thomas Goyne болон Aegisub-ийн баг, [LICENCE](LICENCE) дахь BSD лицензээр.

Дотоод хоолой салгагч нь Meta-гийн [htdemucs](https://github.com/facebookresearch/demucs) загварыг ажиллуулдаг [demucs.cpp](https://github.com/sevagh/demucs.cpp) (MIT). Яриаг [ElevenLabs](https://elevenlabs.io) үүсгэдэг. Бусад сангуудыг *Help → About*-оос харна уу.

Алдаа, санал: [github.com/tuvshinorg/aegidub/issues](https://github.com/tuvshinorg/aegidub/issues).
