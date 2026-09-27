# notepad

A text editor for the [FREE-WILi 2](https://freewili.com): type with an
on-screen keyboard or the colour buttons, and your note is saved to the SD
card. Built on WiliBSP, and tested on every push in the
[FREE-WILi 2 emulator](https://dfdarty.github.io/freewili2-emu/).

![notepad in the emulator](docs/notepad.png)

## Using it

| To | Do |
|---|---|
| type | tap the keys; **shift** is for one capital, **123** / **abc** switch to numbers and symbols |
| type without touching the screen | the five colour buttons: WiliBSP's two-press chord keyboard. The strip at the top right shows what each button types next; **PAGE** switches between letter, capital, number and symbol pages |
| move the cursor | tap in the text, or use the D-pad (held keys repeat) |
| new line | **enter**, or the centre of the D-pad |
| delete | **<del**, or **CANCEL** (both repeat when held) |
| save | **OK**. It also saves by itself two seconds after you stop typing |
| leave | hold **HOME** for 5 s. Hold **PAGE** for 5 s for the About screen |

The top bar shows how many characters the note has and whether it is
**saved** or **edited**. The note is `/notes/notes.txt` on the SD card in
the MAIN processor's slot (up to 8 KB). If there is no card it says **no SD
card** and you can still type, but nothing is kept.

## Run it on your PC

Once, get the emulator (Linux, WSL2 on Windows, or a Codespace):

```sh
git clone --recurse-submodules https://github.com/dfdarty/freewili2-emu ~/freewili2-emu
```

Then, from this folder:

```sh
~/freewili2-emu/tools/fw2emu run .                  # in a window; the SD card is ./sdcard
~/freewili2-emu/tools/fw2emu test .                 # run test.txt: PASS or FAIL
~/freewili2-emu/tools/fw2emu hwcheck --fetch-toolchain .   # fits the chip? also builds the UF2
```

Keys in the emulator window: click the screen to touch it; arrows and Enter
for the D-pad; H O C P for HOME, OK, CANCEL, PAGE; 1–5 for the colour buttons.

## How it's built

- `main.c`: one file. Drawing goes to a framebuffer in PSRAM, and only the
  rows that changed are sent to the LCD, by DMA, between frames.
- The SD card belongs to the MAIN processor and is reached over WiliBSP's
  OneWili link. Saves are written in 512-byte pieces, because the OneWili
  client WiliBSP ships can overrun the MAIN processor's receive buffer with
  one long write.
- `test.txt` types with touches, the symbol page and a chord, edits in the
  middle, deletes, and saves both ways; `test.expect` checks the file that
  ended up on the SD card.

On the real chip (`fw2emu hwcheck`): 46 KB image, 114 KB of SRAM, 600 KB of
PSRAM, 1.2 KB of stack.

Unofficial; not affiliated with FREE-WILi LLC.
