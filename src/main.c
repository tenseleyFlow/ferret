/* ferret — entry point (M0 skeleton). Real argv/parse/walk lands in sprint 01. */
#include "version.h"

#include <stdio.h>
#include <string.h>

static void print_version(void)
{
	printf("ferret %s\n", FRT_VERSION);
	printf("a fast, byte-for-byte GNU find(1) clone (parity target: findutils 4.10.0)\n");
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			print_version();
			return 0;
		}
		if (strcmp(argv[i], "--help") == 0) {
			printf("usage: ferret [path...] [expression]\n");
			return 0;
		}
	}
	fprintf(stderr, "ferret: not yet implemented (M0 skeleton)\n");
	return 1;
}
