/*
Calf Box, an open source musical instrument.
Copyright (C) 2010 Krzysztof Foltman

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config-api.h"

#include <glib.h>
#include <stdlib.h>

static GKeyFile *config_keyfile;
static gchar *keyfile_name;

void cbox_config_init()
{
    const gchar *keyfiledirs[3];
    const gchar *keyfilename = ".cboxrc";
    GError *error = NULL;
    if (config_keyfile)
        return;
    
    config_keyfile = g_key_file_new();
    keyfiledirs[0] = getenv("HOME");
    keyfiledirs[1] = NULL;
    // XXXKF add proper error handling
    
    if (!g_key_file_load_from_dirs(config_keyfile, keyfilename, keyfiledirs, &keyfile_name, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &error))
    {
        g_warning("Could not read cboxrc: %s, search dir = %s, filename = %s", error->message, keyfiledirs[0], keyfilename);
        g_error_free(error);
    }
    else
    {
        g_message("Config pathname is %s", keyfile_name);
    }
}

int cbox_config_has_section(const char *section)
{
    return g_key_file_has_group(config_keyfile, section);
}

char *cbox_config_get_string(const char *section, const char *key)
{
    return g_key_file_get_string(config_keyfile, section, key, NULL);
}

char *cbox_config_get_string_with_default(const char *section, const char *key, char *def_value)
{
    if (g_key_file_has_key(config_keyfile, section, key, NULL))
    {
        return g_key_file_get_string(config_keyfile, section, key, NULL);
    }
    else
    {
        return def_value;
    }
}

int cbox_config_get_int(const char *section, const char *key, int def_value)
{
    GError *error = NULL;
    int result;
    
    result = g_key_file_get_integer(config_keyfile, section, key, &error);
    if (error)
    {
        g_error_free(error);
        return def_value;
    }
    return result;
}

float cbox_config_get_float(const char *section, const char *key, float def_value)
{
    GError *error = NULL;
    float result;
    
    result = g_key_file_get_double(config_keyfile, section, key, &error);
    if (error)
    {
        g_error_free(error);
        return def_value;
    }
    return result;
}    

void cbox_config_close()
{
    if (config_keyfile)
    {
        g_key_file_free(config_keyfile);
        config_keyfile = NULL;
    }
}

