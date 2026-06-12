savedcmd_chromascope_lcd.mod := printf '%s\n'   chromascope_lcd.o | awk '!x[$$0]++ { print("./"$$0) }' > chromascope_lcd.mod
