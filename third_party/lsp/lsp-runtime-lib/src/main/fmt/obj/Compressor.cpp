/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 23 июл. 2025 г.
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

#include <lsp-plug.in/fmt/obj/Compressor.h>
#include <lsp-plug.in/common/alloc.h>

namespace lsp
{
    namespace obj
    {
        typedef struct c_event_t
        {
            uint8_t code;
            uint8_t bits;
        } c_event_t;

        static const c_event_t event_codes[] =
        {
            { 0x0b, 4 },    // CEV_OBJECT
            { 0x3a, 6 },    // CEV_VERTEX2
            { 0x00, 3 },    // CEV_VERTEX3
            { 0x78, 7 },    // CEV_VERTEX4
            { 0x79, 7 },    // CEV_PVERTEX2
            { 0x1a, 5 },    // CEV_PVERTEX3
            { 0x7a, 7 },    // CEV_PVERTEX4
            { 0x3b, 6 },    // CEV_NORMAL2
            { 0x01, 3 },    // CEV_NORMAL3
            { 0x7b, 7 },    // CEV_NORMAL4
            { 0xf8, 8 },    // CEV_TEXCOORD1
            { 0x08, 4 },    // CEV_TEXCOORD2
            { 0xf9, 8 },    // CEV_TEXCOORD3
            { 0x03, 3 },    // CEV_FACE
            { 0x18, 5 },    // CEV_FACE_T
            { 0x02, 3 },    // CEV_FACE_N
            { 0x09, 4 },    // CEV_FACE_NF
            { 0x19, 5 },    // CEV_FACE_TN
            { 0x0a, 4 },    // CEV_FACE_TNF
            { 0x1b, 5 },    // CEV_LINE
            { 0x38, 6 },    // CEV_LINE_T
            { 0x39, 6 },    // CEV_POINT
            { 0xfb, 8 },    // CEV_EOF
        };

        static inline int32_t float_to_bin(float v)
        {
            union {
                float f32;
                int32_t i32;
            } cvt;

            cvt.f32 = v;
            return cvt.i32;
        }

        static inline uint32_t zigzag_encode(int32_t value)
        {
            return (value >> 31) ^ (value << 1);
        }

        Compressor::Compressor()
        {
            pOut        = NULL;
            vFloatBuf   = NULL;

            nFloatHead  = 0;
            nFloatSize  = 0;
            nFloatCap   = 0;
            nFloatBits  = 0;

            nLastEvent  = -1;

            nWFlags     = 0;
        }

        Compressor::~Compressor()
        {
            close();

            if (vFloatBuf != NULL)
            {
                free(vFloatBuf);
                vFloatBuf       = NULL;
            }
        }

        status_t Compressor::do_wrap(io::OutBitStream * & obs, size_t flags)
        {
            compressed_header_t hdr;

            hdr.signature   = COMPRESSED_SIGNATURE;
            hdr.version     = 0;
            hdr.float_bits  = uint8_t(nFloatBits);
            hdr.pad[0]      = 'L';
            hdr.pad[1]      = 'S';
            ssize_t written = obs->write(&hdr, sizeof(compressed_header_t));
            if (written != sizeof(compressed_header_t))
                return STATUS_IO_ERROR;

            pOut        = release_ptr(obs);
            nFloatHead  = 0;
            nFloatSize  = 0;
            nWFlags     = flags;

            return STATUS_OK;
        }

        status_t Compressor::open(const char *path, size_t mode)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->open(path, mode);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::open(const LSPString *path, size_t mode)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->open(path, mode);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::open(const io::Path *path, size_t mode)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->open(path, mode);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::wrap(FILE *fd, bool close)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->wrap(fd, close);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::wrap_native(fhandle_t fd, bool close)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->wrap_native(fd, close);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::wrap(io::File *fd, size_t flags)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->wrap(fd, flags);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::wrap(io::IOutStream *os, size_t flags)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            io::OutBitStream *obs = new io::OutBitStream();
            if (obs == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (obs != NULL)
                {
                    obs->close();
                    delete obs;
                }
            };

            status_t res = obs->wrap(os, flags);
            if (res == STATUS_OK)
                res = do_wrap(obs);

            return res;
        }

        status_t Compressor::wrap(io::OutBitStream *obs, size_t flags)
        {
            if (vFloatBuf == NULL)
                return STATUS_BAD_STATE;
            if (pOut != NULL)
                return STATUS_OPENED;

            return do_wrap(obs, flags);
        }

        status_t Compressor::close()
        {
            if (pOut == NULL)
                return STATUS_OK;

            // Write End-of-File signature
            status_t res = write_event(CEV_EOF);

            if (nWFlags & WRAP_CLOSE)
                res = update_status(res, pOut->close());
            if (nWFlags & WRAP_DELETE)
                delete pOut;

            pOut        = NULL;
            return res;
        }

        status_t Compressor::set_buffer_size(size_t float_bits)
        {
            if ((float_bits > MAX_FLOAT_BUF_BITS) || (float_bits < MIN_FLOAT_BUF_BITS))
                return STATUS_INVALID_VALUE;

            // We can only change buffer size if there is no active operations.
            if (pOut != NULL)
                return STATUS_BAD_STATE;

            // Check that there is nothing to change
            const size_t float_cap  = 1 << float_bits;
            if (float_cap != nFloatCap)
            {
                // Re-allocate data
                float *ptr              = static_cast<float *>(realloc(vFloatBuf, float_cap * sizeof(float)));
                if (ptr == NULL)
                    return STATUS_NO_MEM;
                vFloatBuf               = ptr;
            }

            nFloatCap               = uint32_t(float_cap);
            nFloatBits              = uint32_t(float_bits);

            return STATUS_OK;
        }

        status_t Compressor::write_event(uint32_t event)
        {
            const c_event_t *code = &event_codes[event];
            if (nLastEvent == code->code)
                return pOut->bwrite(true);

            nLastEvent = code->code;
            return pOut->writev(code->code, code->bits + 1);
        }

        status_t Compressor::write_varint(size_t value)
        {
            do
            {
                const uint8_t b     = (value >= 0x40) ? 0x40 | (value & 0x3f) : value;
                value     >>= 6;

                status_t res        = pOut->writev(b, 7);
                if (res != STATUS_OK)
                    return res;
            } while (value > 0);

            return STATUS_OK;
        }

        status_t Compressor::write_varint_icount(size_t value)
        {
            size_t bits = 3;
            size_t max  = 1 << bits;
            do
            {
                const size_t b      = (value >= max) ? max | (value & (max - 1)) : value;
                status_t res        = pOut->writev(fixed_int(b), bits + 1);
                if (res != STATUS_OK)
                    return res;

                // Update count
                value             >>= bits;
                bits               += 2;
                max               <<= 2;
            } while (value > 0);

            return STATUS_OK;
        }

        status_t Compressor::write_utf8(const char *text)
        {
            const size_t bytes = strlen(text);
            status_t res    = write_varint(bytes);
            if (res == STATUS_OK)
            {
                ssize_t written = pOut->write(text, bytes);
                if (written < 0)
                    return status_t(-written);
                else if (size_t(written) != bytes)
                    return STATUS_IO_ERROR;
            }
            return res;
        }

        status_t Compressor::write_float(float value)
        {
            // Step 1
            // Find index of float in buffer
            int32_t index       = -1;
            const uint32_t base = nFloatHead + nFloatCap - 1;
            const int32_t image = float_to_bin(value);
            for (size_t i=0; i<nFloatSize; ++i)
            {
                const uint32_t idx  = (base - i) % nFloatCap;
                if (vFloatBuf[idx] == value)
                {
                    index               = int32_t(i);
                    break;
                }
            }

            // Depending on index value do the stuff
            if (index > 0)
            {
                // Advance position of item one step forward if it is not the first one
                const uint32_t idx  = (base - index) % nFloatCap;
                lsp::swap(vFloatBuf[idx], vFloatBuf[(idx + 1) % nFloatCap]);

                // We're ready to emit index
                return pOut->writev(index, nFloatBits);
            }
            else if (index == 0)
            {
                // We're ready to emit index
                return pOut->writev(index, nFloatBits);
            }

            // Step 2
            // Find nearest relative float
            uint32_t delta      = 0x7fffffff;
            index               = -1;
            for (size_t i=0; i<nFloatSize; ++i)
            {
                const int32_t diff  = image - vIntBuf[(base - i) % nFloatCap];
                if ((diff > 0x1ffff) || (diff < -0x20000))
                    continue;

                // Convert to zigzag representation and compare with previous value for minimization
                const uint32_t dval = zigzag_encode(diff);
                if (dval < delta)
                {
                    index               = int32_t(i);
                    delta               = dval;
                    break;
                }
            }

            // Emit new floating-point value if we can do an incremental coding
            status_t res;
            if (index >= 0)
            {
                res = pOut->writev(nFloatSize, nFloatBits);  // Indicate that new incremental value has been added
                if (res == STATUS_OK)
                    res     = pOut->writev(index, nFloatBits);  // Write index of original floating-point
                if (res == STATUS_OK)
                    res     = write_varint(delta);              // Write delta
            }
            else
            {
                res = pOut->writev(nFloatSize + 1, nFloatBits);  // Indicate that new value has been added
                if (res == STATUS_OK)
                    res     = pOut->writev(CPU_TO_LE(image));
            }

            // Push item to buffer
            vFloatBuf[nFloatHead]   = value;
            nFloatHead              = (nFloatHead + 1) % nFloatCap;
            if (nFloatSize < (nFloatCap - 2))
                ++nFloatSize;

            return res;
        }

        status_t Compressor::write_indices(const index_t *value, size_t count)
        {
            int32_t delta   = int32_t(value[0]);
            status_t res    = write_varint_icount(zigzag_encode(delta));
            if (res != STATUS_OK)
                return res;

            for (size_t i=1; i<count; ++i)
            {
                const int32_t diff      = int32_t(value[i] - value[0]);
                res                     = write_varint_icount(zigzag_encode(diff));
                if (res != STATUS_OK)
                    return res;
            }
            return STATUS_OK;
        }

        bool Compressor::has_nonempty_index(const index_t *v, size_t count)
        {
            for (size_t i=0; i<count; ++i)
                if (v[i] >= 0)
                    return true;
            return false;
        }

        bool Compressor::has_equal_indices(const index_t *v, size_t count)
        {
            for (size_t i=0; i<count; ++i)
            if (v[i] != v[0])
                return false;

            return true;
        }

        status_t Compressor::begin_object(const char *name)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            status_t res = write_event(CEV_OBJECT);
            if (res == STATUS_OK)
                res         = write_utf8(name);

            return res;
        }

        status_t Compressor::begin_object(const LSPString *name)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            const char *text  = name->get_utf8();
            if (text == NULL)
                return STATUS_NO_MEM;

            status_t res = write_event(CEV_OBJECT);
            if (res == STATUS_OK)
                res         = write_utf8(text);

            return res;
        }

        status_t Compressor::end_object()
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            return STATUS_OK;
        }

        status_t Compressor::end_of_data()
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            return STATUS_OK;
        }

        ssize_t Compressor::add_vertex(float x, float y, float z, float w)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const uint32_t ev   = (w != 1.0f) ? CEV_VERTEX4 :
                                  (z != 0.0f) ? CEV_VERTEX3 :
                                  CEV_VERTEX2;

            status_t res    = write_event(ev);
            if (res == STATUS_OK)
                res             = write_float(x);
            if (res == STATUS_OK)
                res             = write_float(y);
            if ((res == STATUS_OK) && (ev >= CEV_VERTEX3))
                res             = write_float(z);
            if ((res == STATUS_OK) && (ev >= CEV_VERTEX4))
                res             = write_float(w);

            return res;
        }

        ssize_t Compressor::add_param_vertex(float x, float y, float z, float w)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const uint32_t ev   = (w != 1.0f) ? CEV_PVERTEX4 :
                                  (z != 0.0f) ? CEV_PVERTEX3 :
                                  CEV_PVERTEX2;

            status_t res    = write_event(ev);
            if (res == STATUS_OK)
                res             = write_float(x);
            if (res == STATUS_OK)
                res             = write_float(y);
            if ((res == STATUS_OK) && (ev >= CEV_PVERTEX3))
                res             = write_float(z);
            if ((res == STATUS_OK) && (ev >= CEV_PVERTEX4))
                res             = write_float(w);

            return res;
        }

        ssize_t Compressor::add_normal(float nx, float ny, float nz, float nw)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const uint32_t ev   = (nw != 0.0f) ? CEV_NORMAL4 :
                                  (nz != 0.0f) ? CEV_NORMAL3 :
                                  CEV_NORMAL2;

            status_t res    = write_event(ev);
            if (res == STATUS_OK)
                res             = write_float(nx);
            if (res == STATUS_OK)
                res             = write_float(ny);
            if ((res == STATUS_OK) && (ev >= CEV_NORMAL3))
                res             = write_float(nz);
            if ((res == STATUS_OK) && (ev >= CEV_NORMAL4))
                res             = write_float(nw);

            return res;
        }

        ssize_t Compressor::add_texture_vertex(float u, float v, float w)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const uint32_t ev   = (w != 0.0f) ? CEV_TEXCOORD3 :
                                  (v != 0.0f) ? CEV_TEXCOORD2 :
                                  CEV_TEXCOORD1;

            status_t res    = write_event(ev);
            if (res == STATUS_OK)
                res             = write_float(u);
            if ((res == STATUS_OK) && (ev >= CEV_TEXCOORD2))
                res             = write_float(v);
            if ((res == STATUS_OK) && (ev >= CEV_TEXCOORD3))
                res             = write_float(w);

            return res;
        }

        ssize_t Compressor::add_face(const index_t *vv, const index_t *vn, const index_t *vt, size_t n)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const bool equal_normals= has_equal_indices(vn, n);
            const bool has_normals  = (vn[0] >= 0) || (!equal_normals);
            const bool has_texcoord = has_nonempty_index(vt, n);

            const compressed_event_type_t ev =
                (has_texcoord)
                    ? ((has_normals) ? ((equal_normals) ? CEV_FACE_TNF : CEV_FACE_TN) : CEV_FACE_T)
                    : ((has_normals) ? ((equal_normals) ? CEV_FACE_NF : CEV_FACE_N) : CEV_FACE);

            status_t res            = write_event(ev);
            if (res == STATUS_OK)
                res                     = write_varint_icount(n);
            if (res == STATUS_OK)
                res                     = write_indices(vv, n);
            if ((res == STATUS_OK) && (has_texcoord))
                res                     = write_indices(vt, n);
            if ((res == STATUS_OK) && (has_normals))
            {
                if (equal_normals)
                    res                     = write_varint_icount(vn[0]); // Write the fill index
                else
                    res                     = write_indices(vn, n);
            }

            return res;
        }

        ssize_t Compressor::add_points(const index_t *vv, size_t n)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const uint32_t ev       = CEV_POINT;
            status_t res            = write_event(ev);
            if (res == STATUS_OK)
                res                     = write_varint_icount(n);
            if (res == STATUS_OK)
                res                     = write_indices(vv, n);

            return res;
        }

        ssize_t Compressor::add_line(const index_t *vv, const index_t *vt, size_t n)
        {
            if ((pOut == NULL) || (vFloatBuf == NULL))
                return STATUS_BAD_STATE;

            const bool has_texcoord = has_nonempty_index(vt, n);

            const uint32_t ev       = (has_texcoord) ? CEV_LINE_T : CEV_LINE;
            status_t res            = write_event(ev);
            if (res == STATUS_OK)
                res                     = write_varint_icount(n);
            if (res == STATUS_OK)
                res                     = write_indices(vv, n);
            if ((res == STATUS_OK) && (has_texcoord))
                res                     = write_indices(vt, n);

            return res;
        }

    } /* namespace obj */
} /* namespace lsp */


