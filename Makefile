TARGET = comic_book_libretro.dll
CC = gcc

# Added MuPDF to pkg-config if available, otherwise kept manual includes
CFLAGS = -O2 -I. -DOPJ_STATIC $(shell pkg-config --cflags libarchive libwebp freetype2)

# The "Bridge": Mapping DLL calls to Static calls
OPJ_FIX = -Wl,--defsym,__imp_opj_set_default_decoder_parameters=opj_set_default_decoder_parameters \
          -Wl,--defsym,__imp_opj_create_decompress=opj_create_decompress \
          -Wl,--defsym,__imp_opj_set_info_handler=opj_set_info_handler \
          -Wl,--defsym,__imp_opj_set_warning_handler=opj_set_warning_handler \
          -Wl,--defsym,__imp_opj_set_error_handler=opj_set_error_handler \
          -Wl,--defsym,__imp_opj_setup_decoder=opj_setup_decoder \
          -Wl,--defsym,__imp_opj_stream_default_create=opj_stream_default_create \
          -Wl,--defsym,__imp_opj_stream_set_read_function=opj_stream_set_read_function \
          -Wl,--defsym,__imp_opj_stream_set_skip_function=opj_stream_set_skip_function \
          -Wl,--defsym,__imp_opj_stream_set_seek_function=opj_stream_set_seek_function \
          -Wl,--defsym,__imp_opj_stream_set_user_data=opj_stream_set_user_data \
          -Wl,--defsym,__imp_opj_stream_set_user_data_length=opj_stream_set_user_data_length \
          -Wl,--defsym,__imp_opj_read_header=opj_read_header \
          -Wl,--defsym,__imp_opj_decode=opj_decode \
          -Wl,--defsym,__imp_opj_stream_destroy=opj_stream_destroy \
          -Wl,--defsym,__imp_opj_destroy_codec=opj_destroy_codec \
          -Wl,--defsym,__imp_opj_image_destroy=opj_image_destroy

LDFLAGS = -shared -static-libgcc -static $(OPJ_FIX)

# --- THE KEY CHANGE IS HERE ---
# 1. We list freetype, then harfbuzz, then freetype AGAIN to solve the circular dependency.
# 2. We move system/low-level libs (zlib, bz2, etc.) to the very end.
LIBS = -lmupdf -lmupdf-third \
       -lgumbo -lopenjp2 -ljbig2dec -ljpeg \
       -lfreetype -lharfbuzz -lgraphite2 -lfreetype \
       -lpng -lbrotlidec -lbrotlicommon \
       $(shell pkg-config --libs --static libarchive libwebp) \
       -lz -lbz2 -llzma -lzstd -llz4 \
       -lcrypto -lbcrypt -lws2_32 -lgdi32 -lcomdlg32 -ldwrite -lrpcrt4 -lcrypt32 -liconv -lstdc++ -lm

SOURCES = comic_book.c

all:
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)