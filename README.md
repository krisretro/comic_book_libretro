RetroArch Comic Book Reader Core
A custom libretro core for RetroArch designed to read comic books and documents.

Features
Format Support: Native reading capabilities for CBZ, CBR, and PDF files.

Audio Support: Plays a page turn sfx
Core Options
You can adjust the following parameters within the RetroArch core options menu (accessible via selection or free typing):

Start at page: Defines the starting page. Defaults to 0 (starts at the very first page). Increases in increments of 1.

End at page: Defines the ending page. Defaults to 0 (ends at the very last page). Increases in increments of 1.

Note on page numbering: The core reads page 1 exactly as "page 1" of your physical or digital book, bypassing 0-index confusion (e.g., inputting page 2 will take you exactly to page 2, not page 1).

Building from Source
This core includes a Makefile for straightforward compilation.
Build the core using make:

Bash
make
Once compiled, move the resulting core file into your RetroArch cores directory.

Dependencies & Credits

This project is built using the following, public domain/MIT-licensed C libraries. :
stb_image: Used for robust image decoding and loading.

miniz: Used for fast, lightweight data compression and extraction (essential for handling CBZ/CBR archives).

dr_wav: Used for WAV audio decoding.
