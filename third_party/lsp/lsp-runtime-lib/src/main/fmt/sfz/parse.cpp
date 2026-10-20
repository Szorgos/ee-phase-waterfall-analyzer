/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 2 мар. 2023 г.
 *
 * lsp-runtime-lib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-runtime-lib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-runtime-lib. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/fmt/sfz/parse.h>

#include <lsp-plug.in/stdlib/locale.h>
#include <lsp-plug.in/stdlib/stdlib.h>
#include <lsp-plug.in/stdlib/string.h>

#include <errno.h>

namespace lsp
{
    namespace sfz
    {
        static const char *skip_blank(const char *src, size_t required = 0)
        {
            size_t skipped = 0;

            while (true)
            {
                switch (*src)
                {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\v':
                        ++skipped;
                        ++src;
                        break;
                    default:
                        return (skipped >= required) ? src : NULL;
                }
            }
        }

        static inline char tolower(char c)
        {
            return ((c >= 'A') && (c <= 'Z')) ? c + 'a' - 'A' : c;
        }

        static bool check_match(const char *s, const char *pat)
        {
            for (;; ++s, ++pat)
            {
                if (*s == '\0')
                    return *pat == '\0';
                else if (*pat == '\0')
                    return true;
                else if (tolower(*s) != tolower(*pat))
                    return false;
            }
            return true;
        }

        status_t parse_bool(bool *dst, const char *text)
        {
            text    = skip_blank(text);
            ssize_t value = 0;

            if (check_match(text, "true"))
            {
                value   = 1;
                text   += 4;
            }
            else if (check_match(text, "on"))
            {
                value   = 1;
                text   += 2;
            }
            else if (check_match(text, "yes"))
            {
                value   = 1;
                text   += 3;
            }
            else if (check_match(text, "t"))
            {
                value   = 1;
                text   += 1;
            }
            else if (check_match(text, "false"))
            {
                value   = 0;
                text   += 5;
            }
            else if (check_match(text, "off"))
            {
                value   = 0;
                text   += 3;
            }
            else if (check_match(text, "no"))
            {
                value   = 0;
                text   += 2;
            }
            else if (check_match(text, "f"))
            {
                value   = 0;
                text   += 1;
            }
            else
            {
                status_t res = parse_int(&value, text);
                if (res != STATUS_OK)
                    return res;
            }

            // Check that there is no data at the end
            text = skip_blank(text);
            if (*text != '\0')
                return STATUS_INVALID_VALUE;

            // Return result
            if (dst != NULL)
                *dst    = value != 0;
            return STATUS_OK;
        }

        status_t parse_int(ssize_t *dst, const char *text)
        {
            // Update locale
            SET_LOCALE_SCOPED(LC_NUMERIC, "C");

            // Parse the integer value
            errno       = 0;
            char *end   = NULL;
            long value  = ::strtol(text, &end, 10);
            if ((errno != 0) || (end == text))
                return STATUS_INVALID_VALUE;
            text        = skip_blank(end);

            // Check that we reached the end of string
            if (*text != '\0')
                return STATUS_INVALID_VALUE;

            // Return the result
            if (dst != NULL)
                *dst    = value;
            return STATUS_OK;
        }

        status_t parse_float(float *dst, const char *text)
        {
            // Update locale
            SET_LOCALE_SCOPED(LC_NUMERIC, "C");

            // Parse the floating-point value
            errno       = 0;
            char *end   = NULL;
            float value = ::strtof(text, &end);
            if ((errno != 0) || (end == text))
                return STATUS_INVALID_VALUE;
            text        = skip_blank(end);

            // Check that we reached the end of string
            if (*text != '\0')
                return STATUS_INVALID_VALUE;

            // Return the result
            if (dst != NULL)
                *dst    = value;
            return STATUS_OK;
        }

        static status_t parse_note_name(ssize_t *dst, const char *text)
        {
            text = skip_blank(text);

            // Parse the note name
            int note;
            switch (*(text++))
            {
                case 'c': case 'C': note    = 0;  break;
                case 'd': case 'D': note    = 2;  break;
                case 'e': case 'E': note    = 4;  break;
                case 'f': case 'F': note    = 5;  break;
                case 'g': case 'G': note    = 7;  break;
                case 'a': case 'A': note    = 9;  break;
                case 'b': case 'B': note    = 11; break;
                case 'h': case 'H': note    = 11; break;
                default:
                    return STATUS_INVALID_VALUE;
            }

            // Check alterations
            if (*text == '#')
            {
                ++text;
                ++note;

                // Double alteration?
                if (*text == '#')
                {
                    ++text;
                    ++note;
                }
            }
            else if (*text == 'b')
            {
                ++text;
                --note;

                // Double alteration?
                if (*text == 'b')
                {
                    ++text;
                    --note;
                }
            }

            // Parse the octave (if specified)
            text        = skip_blank(text);
            errno       = 0;
            char *end   = NULL;
            long octave = ::strtol(text, &end, 10);
            if ((errno != 0) || (end == text))
            {
                // Unsuccessful parse?
                if (end != text)
                    return STATUS_INVALID_VALUE;
                octave      = 4;
            }

            // Check that there are no extra characters at the end
            text        = skip_blank(end);
            if (*text != '\0')
                return STATUS_INVALID_VALUE;

            if (dst != NULL)
                *dst        = (octave + 1) * 12 + note;
            return STATUS_OK;
        }

        status_t parse_note(ssize_t *dst, const char *text)
        {
            ssize_t note = 0;

            // Parse the value
            status_t res = parse_note_name(&note, text);
            if (res != STATUS_OK)
                res         = parse_int(&note, text);

            // Validate the result
            if (res != STATUS_OK)
                return res;
            if ((note < 0) || (note > 127))
                return STATUS_INVALID_VALUE;

            // Return result
            if (dst != NULL)
                *dst        = note;
            return res;
        }

    } /* namespace sfz */
} /* namespace lsp */

