#ifndef WSPRSIM_UTILS_H
#define WSPRSIM_UTILS_H

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

extern int printdata;

int get_wspr_channel_symbols(char* message, char* hashtab, char*loctab, unsigned char* symbols);

#endif
