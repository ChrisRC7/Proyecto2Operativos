savedcmd_chromascope_gpio.mod := printf '%s\n'   chromascope_gpio.o | awk '!x[$$0]++ { print("./"$$0) }' > chromascope_gpio.mod
