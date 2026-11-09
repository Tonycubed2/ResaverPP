# Translating ReSaver Plus

ReSaver Plus can show its whole interface in another language: Options > Language (or the Language box at the top of
Options > Settings). The choice is saved as `language=` in `resaverpp.ini` and applies at once, no restart.

Built in: English and Russian (Русский). Corrections and new languages are welcome.

## The file

Each language is one UTF-8 text file, `lang/<code>.txt` (`ru.txt` for Russian). Every text of the program is a pair
of lines:

```
# AppTools.cpp:1145  done(cf::SetPlayerGold(*_save, _pvGold), Fmt(tr("Set the player's gold to %d"), _pvGold));
en "Set the player's gold to %d"
tr "Золото игрока: %d"
```

- `en` is the English text exactly as the program has it. Do not change it: it is how the program finds the text.
- `tr` is your translation. Leave it empty (`tr ""`) and the English is shown instead.
- Lines starting with `#` are comments: where the text is used in the code, to give you its context.
- The first line names the language as it appears in the Language menu: `language "Русский"`.

Rules:

- Texts are in "quotes". Inside them write `\"` for a quote, `\n` for a new line, `\\` for a backslash. Write
  everything else (Cyrillic letters, «ёлочки», dashes) as it is: the file is UTF-8.
- `%s`, `%d`, `%zu`, `%u`, `%.1f`, `%08X` and the like are filled in by the program (a number, a name, a file).
  Keep every one of them, in the same order as in the English. You may move them around in the sentence, but not swap
  two of them. `%%` is a plain percent sign. A test checks this; a wrong one would print garbage or crash.
- Many texts are pieces of a sentence that the program glues to a name or number, e.g.
  `"Removed "` + `name` + `" from the player"`. The comment line shows how they are glued. Keep the spaces at the
  start or end of such pieces.
- Keep names as they are: ReSaver Plus, ReSaverHelper, SKSE, F4SE, MO2 / Mod Organizer, Vortex, Papyrus, Buffout 4,
  xEdit, Creation Kit, file names (`resaverpp.ini`, `Papyrus.0.log`), console commands (`setstage`, `prid`) and menu
  paths of other programs.
- Texts may get 20-30 % longer than the English without trouble, but buttons and table headers have limited room:
  prefer short words there.
- A key that starts with a word and a `|` (`home|Remove unattached instances`) is a variant of a text for one place
  only (here: the shorter label of a home screen button). Without it the normal translation is used.

## Testing your edits without a rebuild

1. Make a folder `lang` next to `ReSaverPP.exe` and put your edited `ru.txt` (or a new `<code>.txt`) in it.
2. Start ReSaver Plus, choose the language in Options > Language.
3. After editing the file again, press **Reload language files** in Options > Settings: the change shows at once.

Next to the Language box the program says how many texts it read from your file, and the line of the first error if
there is one (texts up to that line are used).

A file next to the exe wins over the built-in translation, text by text; texts missing from it keep the built-in one.
For a new language, copy `ru.txt`, rename it (`de.txt`, `pl.txt`...), change the `language` line and replace every
`tr` line (or empty them: `tr ""`).

## Chinese (and Japanese, Korean)

A ready file for Simplified Chinese is in the source: `lang/template/zh-Hans.txt` (download it from GitHub,
Tonycubed2/ResaverPP, or ask on the Nexus page). It has every text with an empty `tr ""`.

1. Translate as many `tr ""` lines as you like (an empty one shows the English, so a half-done file works too).
   Write the Chinese as it is: the file is UTF-8 (in Notepad: Save as > Encoding: UTF-8). Keep the first line
   `language "简体中文"`.
2. Test it: make a folder `lang` next to `ReSaverPP.exe`, put the file in it as `zh-Hans.txt`, start ReSaver Plus and
   choose 简体中文 in Options > Language. Edit, then press **Reload language files** in Options > Settings.
3. Send the file back (see "Sending it" below). Tell us which texts you were unsure of and where the Chinese is too
   long for its button; a screenshot helps.

Chinese characters are drawn with a font of your Windows (Microsoft YaHei, else SimSun; on Linux / Wine Noto Sans CJK
or WenQuanYi): nothing to install. The font is loaded when the first Chinese character shows, at the size of the
interface font. The language code decides which font shapes come first: `zh-Hans` (Simplified), `zh-Hant`
(Traditional: Microsoft JhengHei), `ja` (Japanese: Yu Gothic / Meiryo), `ko` (Korean: Malgun Gothic). For Traditional
Chinese, Japanese or Korean, copy the template to `zh-Hant.txt`, `ja.txt` or `ko.txt` and change the `language` line.

Not translated (they stay English in every language): texts from the save check's findings that come from the core,
change form labels, error messages from the core, AI prompts, logs, the command line tool, reports and system info,
and messages shown in the game.

## Sending it

Post the file on the ReSaver Plus Nexus page (Posts or Bugs, or a private message to tonycubed2), or open a pull
request on GitHub (Tonycubed2/ResaverPP) that changes `lang/<code>.txt`. It is then built into the next version.

## For the developer

- Code: every text the GUI shows goes through `tr("...")` (or `trId` for window titles, popups, tabs and headers: the
  English stays the ImGui id; `trc("context", "...")` for a variant; `N_("...")` marks texts in static tables that are
  translated where they are shown). See `src/core/Lang.h`.
- `python3 tools/i18n/tr_keys.py --update lang/ru.txt` adds new texts to the catalogue (with `tr ""`), keeps the
  translations and moves texts the code no longer uses to the end. `--check` fails when a text is missing (CTest
  `i18n_keys`); `rs_lang_test` checks the `%` conversions.
- `lang/*.txt` are built into the exe by `cmake/EmbedLang.cmake`. `lang/template/*.txt` are not (empty templates for
  volunteers): keep them in step with `python3 tools/i18n/tr_keys.py --update lang/template/zh-Hans.txt` (CTest
  `i18n_template_zh`). A returned translation goes to `lang/zh-Hans.txt` and is built in from then on.
- Language codes may have capitals (`zh-Hans`): letters, digits, `-` and `_`.
