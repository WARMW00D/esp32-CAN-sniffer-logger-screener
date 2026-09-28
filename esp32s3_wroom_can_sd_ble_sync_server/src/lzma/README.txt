LZMA encoder from the LZMA SDK by Igor Pavlov (https://github.com/ip7z/7zip, folder C/).
License: public domain.

Local changes for the sniffer:
  - Precomp.h: Z7_ST defined (single-threaded build, no LzFindMt);
  - LzmaEnc.c: RC_BUF_SIZE reduced from 64 KB to 4 KB, so compressed data
    reaches the SD card more often (less data lost on a crash).
Only the encoder is used; logs are written as .lzma (LZMA "alone" format).
Arduino IDE compiles these files automatically (src/ folder of the sketch).
