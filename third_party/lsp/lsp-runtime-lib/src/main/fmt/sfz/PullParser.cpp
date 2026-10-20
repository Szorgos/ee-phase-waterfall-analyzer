/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 29 янв. 2023 г.
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

#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/fmt/sfz/PullParser.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InMemoryStream.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/io/InSequence.h>
#include <lsp-plug.in/io/OutMemoryStream.h>

namespace lsp
{
    namespace sfz
    {

        PullParser::PullParser()
        {
            pIn             = NULL;
            nWFlags         = 0;
            sCurrent.type   = EVENT_NONE;
            sSample.type    = EVENT_NONE;
            nUnget          = 0;
        }

        PullParser::~PullParser()
        {
            close();
        }

        status_t PullParser::open(const char *path)
        {
            if (pIn != NULL)
                return STATUS_OPENED;

            io::InFileStream *ifs = new io::InFileStream();
            if (ifs == NULL)
                return STATUS_NO_MEM;

            status_t res = ifs->open(path);
            if (res == STATUS_OK)
                res         = wrap(ifs, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                ifs->close();
                delete ifs;
            }

            return res;
        }

        status_t PullParser::open(const LSPString *path)
        {
            if (pIn != NULL)
                return STATUS_OPENED;

            io::InFileStream *ifs = new io::InFileStream();
            if (ifs == NULL)
                return STATUS_NO_MEM;

            status_t res = ifs->open(path);
            if (res == STATUS_OK)
                res         = wrap(ifs, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                ifs->close();
                delete ifs;
            }

            return res;
        }

        status_t PullParser::open(const io::Path *path)
        {
            if (pIn != NULL)
                return STATUS_OPENED;

            io::InFileStream *ifs = new io::InFileStream();
            if (ifs == NULL)
                return STATUS_NO_MEM;

            status_t res = ifs->open(path);
            if (res == STATUS_OK)
                res         = wrap(ifs, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                ifs->close();
                delete ifs;
            }

            return res;
        }

        status_t PullParser::wrap(const char *str)
        {
            return wrap(str, strlen(str));
        }

        status_t PullParser::wrap(const void *buf, size_t len)
        {
            if (pIn != NULL)
                return STATUS_OPENED;

            io::InMemoryStream *ifs = new io::InMemoryStream(buf, len);
            if (ifs == NULL)
                return STATUS_NO_MEM;

            status_t res = wrap(ifs, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                ifs->close();
                delete ifs;
            }

            return res;
        }

        status_t PullParser::wrap(const LSPString *str)
        {
            if (pIn != NULL)
                return STATUS_OPENED;

            return wrap(str->get_utf8());
        }

        status_t PullParser::wrap(io::IInStream *is, size_t flags)
        {
            if (pIn != NULL)
                return STATUS_OPENED;

            pIn             = is;
            nWFlags         = flags;
            sCurrent.type   = EVENT_NONE;
            sUnget.truncate();
            nUnget          = 0;

            return STATUS_OK;
        }

        status_t PullParser::close()
        {
            status_t res    = STATUS_OK;
            if (pIn == NULL)
                return res;

            if (nWFlags & WRAP_CLOSE)
                res = pIn->close();
            if (nWFlags & WRAP_DELETE)
                delete pIn;

            pIn             = NULL;
            sCurrent.type   = EVENT_NONE;
            sUnget.truncate();
            nUnget          = 0;

            return res;
        }

        const event_t *PullParser::current() const
        {
            return &sCurrent;
        }

        status_t PullParser::next(event_t *event)
        {
            if (pIn == NULL)
                return STATUS_CLOSED;

            // Read the next event
            status_t res = read_next_event(&sCurrent);
            if (res != STATUS_OK)
                return res;

            // Return event data if operation was successful
            if (event != NULL)
            {
                LSPString name, value;
                if (!name.set(&sCurrent.name))
                    return STATUS_NO_MEM;
                if (!value.set(&sCurrent.value))
                    return STATUS_NO_MEM;

                // Commit changes
                event->type         = sCurrent.type;
                event->name.swap(&name);
                event->value.swap(&value);
                event->blob.wrap(sCurrent.blob, 0);
            }

            return STATUS_OK;
        }

        bool PullParser::is_identifier(lsp_wchar_t ch, bool first)
        {
            if ((ch >= 'a') && (ch <= 'z'))
                return true;
            if ((ch >= 'A') && (ch <= 'Z'))
                return true;
            if (ch == '_')
                return true;
            if ((!first) && (ch >= '0') && (ch <= '9'))
                return true;
            return false;
        }

        bool PullParser::is_space(lsp_wchar_t ch)
        {
            switch (ch)
            {
                case '\n':
                case '\r':
                case '\t':
                case '\v':
                case ' ':
                    return true;
                default:
                    break;
            }
            return false;
        }

        void PullParser::trim_right(LSPString *value)
        {
            ssize_t index = ssize_t(value->length()) - 1;
            for (; index >= 0; --index)
            {
                if (!is_space(value->char_at(index)))
                    break;
            }
            value->set_length((index >= 0) ? index + 1 : 0);
        }

        lsp_swchar_t PullParser::get_char()
        {
            if (nUnget < sUnget.length())
            {
                lsp_wchar_t ch = sUnget.char_at(nUnget++);
                if (nUnget >= sUnget.length())
                {
                    sUnget.truncate();
                    nUnget   = 0;
                }
                return ch;
            }
            return lsp_swchar_t(pIn->read_byte());
        }

        status_t PullParser::peek_pending_event(event_t *ev)
        {
            if (sSample.type != EVENT_NONE)
            {
                ev->type = sSample.type;
                ev->name.take(&sSample.name);
                ev->value.take(sSample.value);
                ev->blob.take(sSample.blob);

                sSample.type    = EVENT_NONE;

                return STATUS_OK;
            }

            return STATUS_EOF;
        }

        status_t PullParser::read_next_event(event_t *ev)
        {
            lsp_swchar_t ch;

            while ((ch = get_char()) >= 0)
            {
                switch (ch)
                {
                    case '<':
                        return set_error(read_header(ev));
                    case '/':
                        return set_error(read_comment(ev));
                    case '#':
                        return set_error(read_preprocessor(ev));
                    default:
                    {
                        if (is_space(ch))
                            break;
                        status_t res = read_opcode(ch, ev);
                        if (res == STATUS_SKIP) // Special case?
                            break;
                        return set_error(res);
                    }
                }
            }

            // Check if there is pending event
            if (ch == -STATUS_EOF)
                return set_error(peek_pending_event(ev));

            return set_error(-ch);
        }

        status_t PullParser::read_header(event_t *ev)
        {
            lsp_swchar_t ch;
            LSPString header;

            // If there is pending event, return it first
            status_t res = peek_pending_event(ev);
            if (res == STATUS_OK)
                return (sUnget.append('<')) ? STATUS_OK : STATUS_NO_MEM;

            while ((ch = get_char()) >= 0)
            {
                if (ch == '>')
                {
                    ev->type        = EVENT_HEADER;
                    ev->name.swap(&header);
                    ev->value.clear();
                    ev->blob.close();

                    // Special case: trigger pending event on '<sample>' header
                    if (ev->name.equals_ascii(HDR_SAMPLE))
                        sSample.type    = EVENT_SAMPLE;

                    return STATUS_OK;
                }
                else if (!is_identifier(ch, header.is_empty()))
                    return STATUS_CORRUPTED;

                if (!header.append(ch))
                    return STATUS_NO_MEM;
            }

            return (ch == -STATUS_EOF) ? STATUS_CORRUPTED : -ch;
        }

        status_t PullParser::read_opcode_name(lsp_swchar_t ch, LSPString *name)
        {
            if (!is_identifier(ch, true))
                return STATUS_CORRUPTED;
            if (!name->append(ch))
                return STATUS_NO_MEM;

            while ((ch = get_char()) >= 0)
            {
                if (ch == '=')
                    return STATUS_OK;
                else if ((ch != '$') && (!is_identifier(ch, name->is_empty())))
                    return STATUS_CORRUPTED;

                if (!name->append(ch))
                    return STATUS_NO_MEM;
            }

            return (ch == -STATUS_EOF) ? STATUS_CORRUPTED : -ch;
        }

        status_t PullParser::read_opcode_value(LSPString *value)
        {
            lsp_swchar_t ch;

            while ((ch = get_char()) >= 0)
            {
                switch (ch)
                {
                    case '<': // header?
                    case '/': // comment?
                        nUnget = 0;
                        sUnget.clear();
                        if (!sUnget.append(ch))
                            return STATUS_NO_MEM;
                        return STATUS_OK;

                    default:
                        if (is_space(ch))
                            return STATUS_OK;
                        if (!value->append(ch))
                            return STATUS_NO_MEM;
                        break;
                }
            }

            return (ch == -STATUS_EOF) ? STATUS_OK : -ch;
        }

        status_t PullParser::read_string_opcode(LSPString *value)
        {
            lsp_swchar_t ch;

            while ((ch = get_char()) >= 0)
            {
                if (ch == '=')
                {
                    // Special case from SFZ specification:
                    // Long names and names with blank spaces and other special characters
                    // (excepting the = character) are allowed in the sample definition.
                    // sample=some file name.ext opcode2= <- we are here
                    if (!value->append(ch))
                        return STATUS_NO_MEM;

                    // Look for the first space character from the end
                    ssize_t index = ssize_t(value->length()) - 2;
                    for (; index >= 0; --index)
                    {
                        ch = value->char_at(index);
                        if (is_space(ch))
                            break;
                    }

                    // Fill the unget buffer
                    if (!sUnget.set(value, (index >= 0) ? index + 1 : 0))
                        return STATUS_NO_MEM;
                    nUnget      = 0;

                    // Trim the value from the right
                    value->set_length((index >= 0) ? index : 0);
                    trim_right(value);

                    return STATUS_OK;
                }
                else if (ch == '\n')
                {
                    trim_right(value);
                    return STATUS_OK;
                }

                if (!value->append(ch))
                    return STATUS_NO_MEM;
            }

            return (ch == -STATUS_EOF) ? STATUS_OK : -ch;
        }

        status_t PullParser::read_sample_data(io::IOutStream *os)
        {
            lsp_swchar_t ch;
            status_t res;

            // The decoding algorithm is as follows:
            // Loop while there is an input byte b1
            //    if b1 is the end marker $24, stop reading
            //    if b1 is ‘\r’ ($0D) or ‘\n’ ($0A), discard it
            //    if b1 is the escape character ‘=’ ($3D),
            //        extract the next byte b2, and compute the next output byte as (b2+$C0)%256
            //    otherwise, compute the next output byte as (b1+$D6)%256

            while ((ch = get_char()) >= 0)
            {
                switch (ch)
                {
                    // If the character is the end marker 0x24, stop reading
                    case '$':
                        return STATUS_OK;

                    // If the character is ‘\r’ (0x0D) or ‘\n’ (0x0A), discard it
                    case '\n':
                    case '\r':
                        break;

                    // If the character is the escape character ‘=’
                    case '=':
                        // Extract the next byte and compute the next output byte as (value + 0xC0)&0xff
                        if ((ch = get_char()) < 0)
                            return (ch == -STATUS_EOF) ? STATUS_CORRUPTED : -ch;
                        if ((res = os->write_byte((ch + 0xc0) & 0xff)) != STATUS_OK)
                            return res;
                        break;

                    // Otherwise, compute the next output byte as (value + 0xD6)&0xff
                    default:
                        if ((res = os->write_byte((ch + 0xd6) & 0xff)) != STATUS_OK)
                            return res;
                        break;
                }
            }

            return (ch == -STATUS_EOF) ? STATUS_CORRUPTED : -ch;
        }

        bool PullParser::is_string_opcode(const LSPString *name)
        {
            ssize_t first, last;

            // sorted list of parameter names
            static const char *names[] =
            {
                "default_path",
                "delay_filter",
                "filter_type",
                "global_label",
                "group_label",
                "image",
                "master_label",
                "region_label",
                "sample",
                "script",
                "static_filter",
                "sw_label",
                "vendor_specific"
            };

            // sorted list of parameter prefixes
            static const char *prefixes[] =
            {
                "label_cc",
                "label_key"
            };

            // Perform binary search by full match
            first = 0;
            last = sizeof(names) / sizeof(names[0]) - 1;
            while (first <= last)
            {
                ssize_t mid = (first + last) / 2;
                const char *text = names[mid];

                ssize_t res = name->compare_to_ascii(text);
                if (res == 0)
                    return true;
                if (res < 0)
                    last  = mid - 1;
                else
                    first = mid + 1;
            }

            // Perform binary search by prefx match
            first = 0;
            last = sizeof(prefixes) / sizeof(prefixes[0]) - 1;
            while (first <= last)
            {
                ssize_t mid = (first + last) / 2;
                const char *text = prefixes[mid];

                if (name->starts_with_ascii(text))
                    return true;
                if (name->compare_to_ascii(text) < 0)
                    last  = mid - 1;
                else
                    first = mid + 1;
            }

            return false;
        }

        status_t PullParser::read_opcode(lsp_wchar_t ch, event_t *ev)
        {
            status_t res;
            LSPString name, value;

            if ((res = read_opcode_name(ch, &name)) != STATUS_OK)
                return res;

            if (is_string_opcode(&name))
            {
                if ((res = read_string_opcode(&value)) != STATUS_OK)
                    return res;
            }
            else if (sSample.type == EVENT_SAMPLE)
            {
                // Special case: the <sample> header is pending
                if ((is_string_opcode(&name)) || (name.equals_ascii("name")))
                {
                    if ((res = read_string_opcode(&value)) != STATUS_OK)
                        return res;
                    sSample.name.swap(&value);
                    return STATUS_SKIP;
                }
                else if (name.equals_ascii("data"))
                {
                    io::OutMemoryStream os;
                    if ((res = read_sample_data(&os)) != STATUS_OK)
                        return res;

                    sSample.blob.take(&os);
                    return STATUS_SKIP;
                }
                else if ((res = read_opcode_value(&value)) != STATUS_OK)
                    return res;
            }
            else
            {
                if ((res = read_opcode_value(&value)) != STATUS_OK)
                    return res;
            }

            // Commit the result
            ev->type        = EVENT_OPCODE;
            ev->name.swap(&name);
            ev->value.swap(&value);
            ev->blob.close();

            return STATUS_OK;
        }

        status_t PullParser::read_comment(event_t *ev)
        {
            lsp_swchar_t ch;
            if ((ch = get_char()) < 0)
                return -ch;
            if (ch != '/')
                return STATUS_CORRUPTED;

            // Read the entire string until end of line
            LSPString text;
            while ((ch = get_char()) >= 0)
            {
                if (ch == '\n')
                {
                    // Patch for Windows line endings
                    if (text.last() == '\r')
                        text.remove_last();
                    break;
                }

                if (!text.append(ch))
                    return STATUS_NO_MEM;
            }

            ev->type        = EVENT_COMMENT;
            ev->name.clear();
            ev->value.swap(&text);
            ev->blob.close();

            return STATUS_OK;
        }

        status_t PullParser::expect_string(const char *text)
        {
            // Read the expected string from the file
            lsp_swchar_t ch;
            for (const char *p=text; *p != '\0'; ++p)
            {
                if ((ch = get_char()) < 0)
                    return (ch == -STATUS_EOF) ? STATUS_CORRUPTED : -ch;
                if (ch != *p)
                    return STATUS_CORRUPTED;
            }

            return STATUS_OK;
        }

        status_t PullParser::expect_char(lsp_swchar_t expected)
        {
            lsp_swchar_t ch;
            while ((ch = get_char()) >= 0)
            {
                if (ch == expected)
                    return STATUS_OK;
                if (!is_space(ch))
                    return STATUS_CORRUPTED;
            }

            return (ch == -STATUS_EOF) ? STATUS_CORRUPTED : -ch;
        }

        status_t PullParser::read_variable_name(LSPString *value)
        {
            lsp_swchar_t ch;
            if (!value->append('$'))
                return STATUS_NO_MEM;

            while ((ch = get_char()) >= 0)
            {
                if (is_space(ch))
                    break;
                if (!is_identifier(ch, value->length() == 1))
                    return STATUS_CORRUPTED;
                if (!value->append(ch))
                    return STATUS_NO_MEM;
            }
            if ((ch < 0) && (ch != -STATUS_EOF))
                return -ch;

            // The variable name should be of at least 1 character after '$' sign
            return (value->length() >= 2) ? STATUS_OK : STATUS_CORRUPTED;
        }

        status_t PullParser::read_variable_value(LSPString *value)
        {
            lsp_swchar_t ch;

            // Read first character
            while ((ch = get_char()) >= 0)
            {
                if (!is_space(ch))
                    break;
            }
            if (ch < 0)
                return (ch != -STATUS_EOF) ? -ch : STATUS_OK;

            if (!value->append(ch))
                return STATUS_NO_MEM;

            // Read the last variable definition
            size_t slashes = 0;

            while ((ch = get_char()) >= 0)
            {
                switch (ch)
                {
                    case '<': // header?
                        nUnget = 0;
                        sUnget.clear();
                        if (!sUnget.append(ch))
                            return STATUS_NO_MEM;
                        return STATUS_OK;

                    case '/': // possible comment?
                        if ((++slashes) >= 2)
                        {
                            nUnget = 0;
                            sUnget.clear();
                            if (!sUnget.append_ascii("//", 2))
                                return STATUS_NO_MEM;
                            return STATUS_OK;
                        }
                        if (!value->append(ch))
                            return STATUS_NO_MEM;
                        break;

                    default:
                        slashes = 0;
                        if (is_space(ch))
                            return STATUS_OK;
                        if (!value->append(ch))
                            return STATUS_NO_MEM;
                        break;
                }
            }

            return (ch == -STATUS_EOF) ? STATUS_OK : -ch;
        }

        status_t PullParser::read_define(event_t *ev)
        {
            status_t res;
            if ((res = expect_string("efine")) != STATUS_OK) // 'd' already was read
                return res;
            if ((res = expect_char('$')) != STATUS_OK)
                return res;

            // Read the variable name
            LSPString name;
            if ((res = read_variable_name(&name)) != STATUS_OK)
                return res;

            // Read the variable value
            LSPString value;
            if ((res = read_variable_value(&value)) != STATUS_OK)
                return res;

            // Commit the result
            ev->type        = EVENT_DEFINE;
            ev->name.swap(&name);
            ev->value.swap(&value);
            ev->blob.close();

            return STATUS_OK;
        }

        status_t PullParser::read_include(event_t *ev)
        {
            status_t res;
            if ((res = expect_string("nclude")) != STATUS_OK) // 'i' already was read
                return res;
            if ((res = expect_char('\"')) != STATUS_OK)
                return res;

            // Read the entire string until the end quote occurs
            lsp_swchar_t ch;
            LSPString text;
            while (true)
            {
                if ((ch = get_char()) < 0)
                    return (ch == -STATUS_EOF) ? -STATUS_CORRUPTED : -ch;

                if (ch == '\"')
                    break;
                if (!text.append(ch))
                    return STATUS_NO_MEM;
            }

            // Commit the result
            ev->type        = EVENT_INCLUDE;
            ev->name.swap(&text);
            ev->value.clear();
            ev->blob.close();

            return STATUS_OK;
        }

        status_t PullParser::read_preprocessor(event_t *ev)
        {
            lsp_swchar_t ch = get_char();
            switch (ch)
            {
                case 'd': return read_define(ev);
                case 'i': return read_include(ev);
                default: break;
            }

            return ((ch < 0) && (ch != -STATUS_EOF)) ? -ch : STATUS_CORRUPTED;
        }

        status_t PullParser::set_error(status_t code)
        {
            if (code != STATUS_OK)
            {
                sCurrent.type       = EVENT_NONE;
                sCurrent.name.truncate();
                sCurrent.value.truncate();
            }

            return code;
        }

    } /* namespace sfz */
} /* namespace lsp */



