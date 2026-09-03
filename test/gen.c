/* Generates a large .qoi for benchmarking: a gradient with slight noise, so
   it does not compress down into a degenerate run of QOI_OP_RUN chunks.

   Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT */
#define QOI_IMPLEMENTATION
#include "../qoi.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	int w = argc > 2 ? atoi(argv[2]) : 4000;
	int h = argc > 3 ? atoi(argv[3]) : 3000;
	unsigned char *px = malloc((size_t)w * h * 4);
	unsigned int s = 12345;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			size_t i = ((size_t)y * w + x) * 4;
			s = s * 1103515245u + 12345u;
			px[i+0] = (unsigned char)(x * 255 / w + ((s >> 16) & 7));
			px[i+1] = (unsigned char)(y * 255 / h);
			px[i+2] = (unsigned char)((x + y) & 0xff);
			px[i+3] = 255;
		}
	}
	qoi_desc d = { (unsigned)w, (unsigned)h, 4, QOI_SRGB };
	int n = qoi_write(argv[1], px, &d);
	printf("%s: %dx%d, %d bytes\n", argv[1], w, h, n);
	return n ? 0 : 1;
}
