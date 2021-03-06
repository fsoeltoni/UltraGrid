#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
        if (argc != 5) {
                fprintf(stderr, "Converts UYVY files to yuv422p format recognized by FFmpeg/libav\n\n");
                fprintf(stderr, "Usage:\n");
                fprintf(stderr, "\t%s <in_filename> <out_filename> <width> <height>\n\n", argv[0]);
                return EXIT_FAILURE;
        }

        char *in_name = argv[1];
        char *out_name = argv[2];
        int width = atoi(argv[3]);
        int height = atoi(argv[4]);

        FILE *in = fopen(in_name, "r");
        FILE *out = fopen(out_name, "w");
        assert(in != NULL && out != NULL);

        // luma
        for(int y = 0; y < height; ++y) {
                for(int x = 0; x < width * 2; x += 2) {
                        getc(in); // unused
                        int y = getc(in); // unused
                        putc(y, out);
                }
        }

        // Cb
        fseek(in, 0L, SEEK_SET);
        for(int y = 0; y < height; ++y) {
                for(int x = 0; x < width * 2; x += 4) {
                        int cb = getc(in); // unused
                        putc(cb, out);
                        getc(in); // unused
                        getc(in); // unused
                        getc(in); // unused
                }
        }

        // Cr
        fseek(in, 2L, SEEK_SET);
        for(int y = 0; y < height; ++y) {
                for(int x = 0; x < width * 2; x += 4) {
                        int cr = getc(in); // unused
                        putc(cr, out);
                        getc(in); // unused
                        getc(in); // unused
                        getc(in); // unused
                }
        }

        fclose(in);
        fclose(out);

        return EXIT_SUCCESS;
}

