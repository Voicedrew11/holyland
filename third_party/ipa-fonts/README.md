# IPA Mincho: the letters of the sharper font

`port/gs/shared/font_letters_data.h` holds pictures of the English letters drawn from IPA Mincho (`ipam.ttf`, version
003.03) by `tools/make_font_letters.py`; the game shows them in place of its own font's letters
(`port/gs/shared/font_sheet.h`). The font file itself is not in the repository: the script reads it from wherever it
is installed (Fedora: the `ipa-mincho-fonts` package; elsewhere https://moji.or.jp/ipafont/).

IPA Font License Agreement v1.0 (`IPA_Font_License_Agreement_v1.0.txt`): the letters are used as pictures of text,
which Article 2 Paragraphs 2 and 3 allow to be reproduced and distributed freely. IPA Mincho is © Information-technology
Promotion Agency, Japan (IPA).

`port/gs/shared/text_pictures_data.h` holds pictures of the letters of the game's word pictures (the menu's buttons and
hints, the title's choices) drawn from `HolylandMincho.ttf` (IPA P Mincho, below) by `tools/make_text_pictures.py`; the
game shows them in place of its own words (`port/gs/shared/text_pictures.h`). Pictures of text, as above.

`port/ui/res/fonts/HolylandMincho.ttf`, the player menu's and the launcher's font, is a Derived Program of IPA P Mincho
(`ipamp.ttf`, 003.03), made by `tools/make_menu_font.py`: its Latin letters and punctuation only, any embedded bitmaps
dropped, renamed (Article 3, Paragraph 1: under this agreement, a copy beside it, a name without the original's). To
put the original back, copy IPA P Mincho's `ipamp.ttf` over it under that name.
