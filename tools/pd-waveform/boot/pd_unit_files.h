#ifndef AETHER_PD_UNIT_FILES_H
#define AETHER_PD_UNIT_FILES_H
/* Internal fixed definitions from generated trusted source, never request data.
 * Both names must fit80bytes including the temporary prefix/suffix. The caller
 * authenticates operation state before publishing/removing these files. */
struct pd_unit_files {const char* const* names;const char* const* text;};
int pd_unit_files_arm(int units,const struct pd_unit_files* definitions);
int pd_unit_files_verify(int units,const struct pd_unit_files* definitions);
int pd_unit_files_remove(int units,const struct pd_unit_files* definitions);
#endif
