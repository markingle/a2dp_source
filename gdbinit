target remote :3333
mon reset halt
flushregs
thb app_main
c
