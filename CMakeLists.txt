# notepad — a text editor for the FREE-WILi 2: an on-screen touch keyboard
# (plus WiliBSP's two-press chord keys on the colour buttons) and voice memos
# from the microphones, saved to the SD card in the MAIN processor's slot. Builds for the emulator and, with the
# Pico SDK and Arm GCC, into a UF2 for the board.
add_executable(notepad main.c)
target_link_libraries(notepad freewili2_bsp onewili_fwgui)
fw2_display_app(notepad
    POWER_ZONES DISPLAY SDCARD SENSORS          # SENSORS: WiliBSP's hello_mics asks for it with the mics
    VERSION 002
    DESCRIPTION "Notepad with voice memos, saved to the SD card")
