# vtty_pair - Virtual Straigh-Through TTY Pair Driver

Adaption of Tiny TTY driver for creating a straight-through/AT serial cable 
interface between 2 virtual serial ports.

**begin note** 

While using tty\_driver and trying to use TIOCMBIS/TIOCMBIC, the kernel blocks 
all TIOCM flags except DTR and RTS before they reach the driver.

I am assuming, that tty\_driver does not contain support for software side
toggling of them lines. Going to try experimenting with Tiny _Serial_ driver
instead. See if the more direct UART driver has support??? Otherwise, I'm at a
loss on how to impliment this lol.

**end note**
