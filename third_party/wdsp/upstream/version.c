#include "comm.h"

PORT
int GetWDSPVersion()
{
	// WDSP Version numbers always contain exactly two digits to the right of the decimal point.
	// For easy comparison with expected version values, the value returned by this function is
	//    version_number * 100.  E.g., Version 1.14 will return 114.
	const int version = 200;
	return version;
}
