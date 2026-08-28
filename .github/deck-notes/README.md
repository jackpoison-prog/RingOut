# Deck notes

The two plain-text files the test Deck keeps on its Desktop, kept here so they
are not lost when the Deck is wiped — which is routine, since resetting it to
stock is how the build instructions get tested the way a new user meets them.

* `INSTALL-TOOLCHAIN.txt` — section 6 of `BUILD-ON-THE-DECK.md` on its own,
  because it is the one part that needs a sudo password and so cannot be run
  over ssh.
* `REMOVE-TOOLCHAIN.txt` — puts the Deck back to stock, including the
  registered-but-stripped `/usr/include` state that makes `pacman -S --needed`
  silently install nothing.

Neither is shipped. `BUILD-ON-THE-DECK.md` in `dist/RingOut-1.0-deck/` is the
one players get, and it is self-contained; a second shipped copy of the same
instructions is exactly the drift this project keeps getting bitten by.
