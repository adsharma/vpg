#ifndef VPG_SETUP_H
#define VPG_SETUP_H

int         vpg_setup(const char *username,
                      const char *system_constraints_file,
                      const char *system_functions_file,
                      const char *system_views_file,
                      const char *dictionary_file,
                      const char *info_schema_file,
                      const char *features_file,
                      const char *infoversion);

const char *vpg_setup_error(void);

#endif
