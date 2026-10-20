/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 24 апр. 2020 г.
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

#include <lsp-plug.in/common/types.h>

#ifdef PLATFORM_WINDOWS

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/runtime/LSPString.h>
#include <lsp-plug.in/mm/sample.h>
#include <private/mm/ACMStream.h>
#include <stdlib.h>

#include <windows.h>
#include <mmreg.h>
#include <msacm.h>

namespace lsp
{
    namespace mm
    {
        
        ACMStream::ACMStream()
        {
            pFmtIn      = NULL;
            pFmtOut     = NULL;
            hDriver     = NULL;
            hStream     = NULL;
            pHeader     = NULL;
        }
        
        ACMStream::~ACMStream()
        {
            close();
        }

        void ACMStream::acm_query_formats(drv_lookup_t *s)
        {
            ACMFORMATTAGDETAILSW *ftd = &s->ftd;
            ACMFORMATDETAILSW *fdw = &s->fd;

            // Iterate all formats
            for (size_t i=0, n=ftd->cStandardFormats; i < n; ++i)
            {
                ::ZeroMemory(fdw, sizeof(ACMFORMATDETAILSW));

                fdw->cbStruct       = sizeof(ACMFORMATDETAILSW);
                fdw->dwFormatTag    = ftd->dwFormatTag;
                fdw->dwFormatIndex  = i;
                fdw->pwfx           = s->req;
                fdw->cbwfx          = s->req_sz;

                if (::acmFormatDetailsW(s->hd, fdw, ACM_FORMATDETAILSF_INDEX) != 0)
                    continue;

                // Create copy and add to driver list
                if ((s->pfmt = s->ptag->vfmt.add()) == NULL)
                    continue;

                s->pfmt->id         = fdw->dwFormatIndex;
                s->pfmt->fdw        = fdw->fdwSupport;
                s->pfmt->wfex       = copy_fmt(fdw->pwfx);
            }
        }

        void ACMStream::acm_query_format_tags(drv_lookup_t *s)
        {
            ACMFORMATTAGDETAILSW *ftd = &s->ftd;

            for (size_t i=0, n=s->dd.cFormatTags; i < n; ++i)
            {
                // Prepare the structure
                ::ZeroMemory(ftd, sizeof(ACMFORMATTAGDETAILSW));
                ftd->cbStruct = sizeof(ACMFORMATTAGDETAILSW);
                ftd->dwFormatTagIndex = i;

                // Get information
                if (::acmFormatTagDetailsW(s->hd, ftd, ACM_FORMATTAGDETAILSF_INDEX) != 0)
                    continue;

                // Create tag descriptor
                if ((s->ptag = new fmt_tag_t()) == NULL)
                    continue;
                if (!s->pdrv->vtag.add(s->ptag))
                {
                    delete s->ptag;
                    continue;
                }
                s->ptag->id     = ftd->dwFormatTag;
                s->ptag->fdw    = ftd->fdwSupport;
                s->ptag->name.set_utf16(ftd->szFormatTag);

                // Query formats
                acm_query_formats(s);
            }
        }

        WINBOOL CALLBACK ACMStream::acm_driver_enum_cb(HACMDRIVERID hadid, DWORD_PTR dwInstance, DWORD fdwSupport)
        {
            drv_lookup_t *s = reinterpret_cast<drv_lookup_t *>(dwInstance);

            // Get driver information
            ACMDRIVERDETAILSW *dd = &s->dd;
            ::ZeroMemory(dd, sizeof(ACMDRIVERDETAILSW));
            dd->cbStruct    = sizeof(ACMDRIVERDETAILSW);
            if (::acmDriverDetailsW(hadid, dd, 0) != 0)
                return TRUE;

            // Open driver
            if (::acmDriverOpen(&s->hd, hadid, 0) != 0)
                return TRUE;

            // Allocate driver object
            if ((s->pdrv = new drv_t()) == NULL)
            {
                ::acmDriverClose(s->hd, 0);
                return TRUE;
            }
            if (!s->vdrv.add(s->pdrv))
            {
                delete s->pdrv;
                ::acmDriverClose(s->hd, 0);
                return TRUE;
            }
            s->pdrv->drv_id = hadid;
            s->pdrv->short_name.set_utf16(dd->szShortName);
            s->pdrv->full_name.set_utf16(dd->szLongName);
            s->pdrv->copyright.set_utf16(dd->szCopyright);
            s->pdrv->license.set_utf16(dd->szLicensing);
            s->pdrv->features.set_utf16(dd->szFeatures);

            // Query format metrics
            s->req_sz           = 0;
            ::acmMetrics((HACMOBJ)s->hd, ACM_METRIC_MAX_SIZE_FORMAT, &s->req_sz);
            s->req_sz           = align_size(s->req_sz, DEFAULT_ALIGN);
            s->pdrv->pwfx_sz    = s->req_sz;
            s->req              = reinterpret_cast<WAVEFORMATEX *>(::malloc(s->req_sz));
            if (s->req != NULL)
            {
                // Query format tags
                acm_query_format_tags(s);

                ::free(s->req);
                s->req      = NULL;
            }

            // Close driver
            ::acmDriverClose(s->hd, 0);
            s->hd = NULL;

            return TRUE;
        }

        status_t ACMStream::acm_enum_drivers(lltl::parray<drv_t> *res)
        {
            drv_lookup_t lk;
            MMRESULT result = ::acmDriverEnum(acm_driver_enum_cb, (DWORD_PTR)&lk, ACM_DRIVERENUMF_DISABLED);
            if (result == 0)
            {
                res->swap(&lk.vdrv);
                acm_destroy_drivers(&lk.vdrv);
            }

        #if 0
            for (size_t i=0, n=res->size(); i<n; ++i)
            {
            #ifdef LSP_TRACE
                LSPString info, idx;
                drv_t *drv = res->uget(i);
                info.fmt_utf8(
                    "DRV #%-4d : %p\n"
                    "Short Name: %s\n"
                    "Long Name : %s\n"
                    "Copyright : %s\n"
                    "License   : %s\n"
                    "Features  : %s\n"
                    "Formats   :\n",
                    int(i),
                    drv->drv_id,
                    drv->short_name.get_utf8(),
                    drv->full_name.get_utf8(),
                    drv->copyright.get_utf8(),
                    drv->license.get_utf8(),
                    drv->features.get_utf8()
                );
                for (size_t j=0, m=drv->vtag.size(); j<m; ++j)
                {
                    fmt_tag_t *tag = drv->vtag.uget(j);
                    info.fmt_append_utf8("  %s [0x%x]: %d items\n",
                            tag->name.get_utf8(),
                            int(tag->id),
                            int(tag->vfmt.size())
                        );

                    for (size_t k=0, l=tag->vfmt.size(); k<l; ++k)
                    {
                        fmt_t *fmt = tag->vfmt.uget(k);
                        idx.fmt_ascii("%d/%d", int(k), int(j));
                        info.fmt_append_utf8(
                            "%10s: %s %d Hz/%d ch @%d bit [0x%x:%d]\n",
                            idx.get_utf8(),
                            tag->name.get_utf8(),
                            int(fmt->wfex->nSamplesPerSec),
                            int(fmt->wfex->nChannels),
                            int(fmt->wfex->wBitsPerSample),
                            int(fmt->wfex->wFormatTag),
                            int(fmt->wfex->cbSize)
                        );
                    }
                }
                lsp_trace("\n%s\n", info.get_native());
            #endif
            }
        #endif

            return (result == 0) ? STATUS_OK : STATUS_UNKNOWN_ERR;
        }

        void ACMStream::acm_destroy_drivers(lltl::parray<drv_t> *res)
        {
            // Drop drivers
            for (size_t i=0, n=res->size(); i<n; ++i)
            {
                drv_t *pdrv = res->uget(i);

                // Drop tags
                for (size_t j=0, m=pdrv->vtag.size(); j<m; ++j)
                {
                    fmt_tag_t *ptag = pdrv->vtag.uget(j);
                    // Drop formats
                    for (size_t k=0, l=ptag->vfmt.size(); k<l; ++k)
                    {
                        fmt_t *pfmt = ptag->vfmt.uget(k);
                        if (pfmt->wfex != NULL)
                        {
                            ::free(pfmt->wfex);
                            pfmt->wfex = NULL;
                        }
                    }

                    delete ptag;
                }

                delete pdrv;
            }

            res->flush();
        }

        WAVEFORMATEX *ACMStream::copy_fmt(const WAVEFORMATEX *src)
        {
            /*
             * cbSize
             *   Size, in bytes, of extra format information appended to the end
             *   of the WAVEFORMATEX structure. This information can be used by
             *   non-PCM formats to store extra attributes for the wFormatTag. If
             *   no extra information is required by the wFormatTag, this member must
             *   be set to 0. For WAVE_FORMAT_PCM formats (and only WAVE_FORMAT_PCM formats),
             *   this member is ignored. When this structure is included in a WAVEFORMATEXTENSIBLE
             *   structure, this value must be at least 22.
             */
            size_t cbsize   = (src->wFormatTag == WAVE_FORMAT_PCM) ? 0 : src->cbSize;
            size_t bytes    = sizeof(WAVEFORMATEX) + cbsize;
            WAVEFORMATEX *res = static_cast<WAVEFORMATEX *>(::malloc(align_size(bytes, DEFAULT_ALIGN)));
            if (res != NULL)
            {
                ::memcpy(res, src, bytes);
                res->cbSize     = cbsize;   // Override original cbSize for PCM
            }
            return res;
        }

        ACMStream::fmt_tag_t *ACMStream::acm_find_tag(drv_t *drv, size_t fmt_tag)
        {
            for (size_t i=0, n=drv->vtag.size(); i<n; ++i)
            {
                fmt_tag_t *tag = drv->vtag.uget(i);
                if (tag == NULL)
                    continue;
                if ((tag->id == fmt_tag) && (tag->fdw & ACMDRIVERDETAILS_SUPPORTF_CODEC))
                    return tag;
            }
            return NULL;
        }

        status_t ACMStream::acm_configure_stream(WAVEFORMATEX *dst, WAVEFORMATEX *src)
        {
            MMRESULT res;

            // Succeeded, initialize stream
            // Estimate size of conversion buffers
            DWORD src_length = IO_BUF_SIZE, dst_length = IO_BUF_SIZE;
            if ((dst->wFormatTag == WAVE_FORMAT_PCM) || (dst->wFormatTag == WAVE_FORMAT_IEEE_FLOAT))
            {
                res = ::acmStreamSize(hStream, IO_BUF_SIZE, &dst_length, ACM_STREAMSIZEF_SOURCE);
                if ((res != 0) || (dst_length <= 0))
                    return STATUS_SKIP;

                res = ::acmStreamSize(hStream, dst_length, &src_length, ACM_STREAMSIZEF_DESTINATION);
                if ((res != 0) || (src_length <= 0))
                    return STATUS_SKIP;
            }
            else
            {
                res = ::acmStreamSize(hStream, IO_BUF_SIZE, &src_length, ACM_STREAMSIZEF_SOURCE);
                if ((res != 0) || (src_length <= 0))
                    return STATUS_SKIP;

                res = ::acmStreamSize(hStream, src_length, &dst_length, ACM_STREAMSIZEF_DESTINATION);
                if ((res != 0) || (src_length <= 0))
                    return STATUS_SKIP;
            }

            // Allocate stream header
            size_t hdr_alloc            = align_size(sizeof(ACMSTREAMHEADER), DEFAULT_ALIGN);
            size_t src_alloc            = align_size(src_length, DEFAULT_ALIGN);
            size_t dst_alloc            = align_size(dst_length, DEFAULT_ALIGN);
            size_t to_alloc             = hdr_alloc + src_alloc + dst_alloc;
            BYTE *buf                   = static_cast<BYTE *>(::malloc(to_alloc));
            if (buf == NULL)
                return STATUS_NO_MEM;

            ::ZeroMemory(buf, to_alloc);
            pHeader                     = reinterpret_cast<ACMSTREAMHEADER *>(buf);
            buf                        += hdr_alloc;
            pHeader->pbSrc              = buf;
            buf                        += src_alloc;
            pHeader->pbDst              = buf;
            buf                        += dst_alloc;

            pHeader->cbStruct           = sizeof(ACMSTREAMHEADER);
            pHeader->dwSrcUser          = src_length;   // maximum size
            pHeader->cbSrcLength        = src_length;   // This is required for initialization
            pHeader->cbSrcLengthUsed    = 0;
            pHeader->dwDstUser          = 0;            // read offset
            pHeader->cbDstLength        = dst_length;
            pHeader->cbDstLengthUsed    = 0;

            // Prepare stream header
            if ((res = ::acmStreamPrepareHeader(hStream, pHeader, 0)) != 0)
                return STATUS_UNKNOWN_ERR;

            // Reset size of input buffer
            pHeader->cbSrcLength        = 0;

            return STATUS_OK;
        }

        status_t ACMStream::acm_try_format_dec(WAVEFORMATEX *to, WAVEFORMATEX *from)
        {
            status_t res;
            size_t ftag = ((to->wFormatTag == WAVE_FORMAT_PCM) || (to->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)) ?
                            from->wFormatTag : to->wFormatTag;

            for (size_t i=0, n=vDrv.size(); i<n; ++i)
            {
                // Work with drivers that ONLY support the requested tag
                drv_t *drv = vDrv.uget(i);
                if (drv == NULL)
                    continue;
                fmt_tag_t *tag = acm_find_tag(drv, ftag);
                if (tag == NULL)
                    continue;

                // Open the specific ACM driver
                if (::acmDriverOpen(&hDriver, drv->drv_id, 0) != 0)
                    continue;

                // Try to open stream
                if (::acmStreamOpen(&hStream, hDriver, from, to, NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME) == 0)
                {
                    res = acm_configure_stream(to, from);
                    if (res != STATUS_OK)
                        return res;

                    // Copy format descriptors
                    if ((pFmtIn = copy_fmt(from)) == NULL)
                        return STATUS_NO_MEM;
                    if ((pFmtOut = copy_fmt(to)) == NULL)
                        return STATUS_NO_MEM;

                    return STATUS_OK;
                }

                // Close ACM driver
                ::acmDriverClose(hDriver, 0);
                hDriver     = NULL;
            }

            return STATUS_OK;
        }

        ACMStream::fmt_t *ACMStream::acm_select_format(fmt_t *best, fmt_t *curr, WAVEFORMATEX *req)
        {
            if (curr == NULL)
                return best;
            if (best == NULL)
                return curr;

            // Minimize sample rate difference
            size_t d1 = lsp_abs(ssize_t(best->wfex->nSamplesPerSec) - ssize_t(req->nSamplesPerSec));
            size_t d2 = lsp_abs(ssize_t(curr->wfex->nSamplesPerSec) - ssize_t(req->nSamplesPerSec));
            if ((d2 < d1) &&
                (curr->wfex->nSamplesPerSec > best->wfex->nSamplesPerSec) &&
                (curr->wfex->nSamplesPerSec <= req->nSamplesPerSec))
                return curr;

            // Optimize number of channels
            d1  = lsp_abs(ssize_t(best->wfex->nChannels) - ssize_t(req->nChannels));
            d2  = lsp_abs(ssize_t(curr->wfex->nChannels) - ssize_t(req->nChannels));
            if (d1 > d2)
                return curr;

            // Optimize sample format
            if ((best->wfex->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) &&
                (curr->wfex->wFormatTag == WAVE_FORMAT_IEEE_FLOAT))
                return curr;
            if (best->wfex->wBitsPerSample < curr->wfex->wBitsPerSample)
                return curr;

            return best;
        }

        status_t ACMStream::acm_find_standard_dec(WAVEFORMATEX *fmt)
        {
            status_t res;
            lltl::parray<fmt_t> blist; // List of all best-matching solutions
            fmt_t *best = NULL;

            for (size_t i=0, n=vDrv.size(); i<n; ++i)
            {
                // Work with drivers that ONLY support the requested tag
                drv_t *drv = vDrv.uget(i);
                if (drv == NULL)
                    continue;
                fmt_tag_t *tag = acm_find_tag(drv, fmt->wFormatTag);
                if (tag == NULL)
                    continue;

                best = NULL;

                // Possible conversion to floating-point PCM ?
                if ((tag = acm_find_tag(drv, WAVE_FORMAT_IEEE_FLOAT)) != NULL)
                {
                    for (size_t j=0, m=tag->vfmt.size(); j<m; ++j)
                        best = acm_select_format(best, tag->vfmt.uget(j), fmt);
                }

                // Possible conversion to integer PCM ?
                if ((tag = acm_find_tag(drv, WAVE_FORMAT_PCM)) != NULL)
                {
                    for (size_t j=0, m=tag->vfmt.size(); j<m; ++j)
                        best = acm_select_format(best, tag->vfmt.uget(j), fmt);
                }

                // Add to list
                if (best != NULL)
                    blist.add(best);
            }

            // Select the best matching format across all and make it first for test
            best = NULL;
            for (size_t j=0, m=blist.size(); j<m; ++j)
                best = acm_select_format(best, blist.uget(j), fmt);
            if (best != NULL)
            {
                size_t idx = blist.index_of(best);
                if (idx != 0)
                    blist.xswap(0, idx);
            }

            // Try each format
            for (size_t i=0, n=blist.size(); i<n; ++i)
            {
                fmt_t *curr = blist.uget(i);
                WAVEFORMATEX *pcm = (curr != NULL) ? curr->wfex : NULL;
                if (pcm == NULL)
                    continue;

                if ((pcm->nChannels != fmt->nChannels) ||
                    (pcm->nSamplesPerSec != fmt->nSamplesPerSec))
                {
                    WAVEFORMATEX tmp = *pcm;
                    tmp.nChannels       = fmt->nChannels;
                    tmp.nSamplesPerSec  = fmt->nSamplesPerSec;
                    tmp.nBlockAlign     = (tmp.wBitsPerSample * tmp.nChannels) >> 3;
                    tmp.nAvgBytesPerSec = tmp.nBlockAlign * tmp.nSamplesPerSec;

                    // Try modified mode
                    if ((res = acm_try_format_dec(pcm, fmt)) != STATUS_OK)
                        return res;
                    if (hStream != NULL)
                        return STATUS_OK;
                }

                // Try non-modified mode
                if ((res = acm_try_format_dec(pcm, fmt)) != STATUS_OK)
                    return res;
                if (hStream != NULL)
                    return STATUS_OK;
            }

            return STATUS_OK;
        }

        status_t ACMStream::acm_find_nonstandard_dec(WAVEFORMATEX *fmt)
        {
            status_t res;
            WAVEFORMATEX pcm;

            pcm.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
            pcm.nChannels       = fmt->nChannels;
            pcm.nSamplesPerSec  = fmt->nSamplesPerSec;
            pcm.nAvgBytesPerSec = sizeof(f32_t) * fmt->nChannels * fmt->nSamplesPerSec;
            pcm.nBlockAlign     = sizeof(f32_t) * fmt->nChannels;
            pcm.wBitsPerSample  = sizeof(f32_t) * 8;
            pcm.cbSize          = 0;

            // Try IEEE Float first as the best output format
            if ((res = acm_try_format_dec(&pcm, fmt)) != STATUS_OK)
                return res;
            else if (hStream != NULL)
                return STATUS_OK;

            // No success, try 32, 24, 16, 8 bits per sample
            for (size_t bps=4; bps > 0; --bps)
            {
                pcm.wFormatTag      = WAVE_FORMAT_PCM;
                pcm.nChannels       = fmt->nChannels;
                pcm.nSamplesPerSec  = fmt->nSamplesPerSec;
                pcm.nAvgBytesPerSec = bps * fmt->nChannels * fmt->nSamplesPerSec;
                pcm.nBlockAlign     = bps * fmt->nChannels;
                pcm.wBitsPerSample  = bps * 8;
                pcm.cbSize          = 0;

                // Try non-standard modes
                if ((res = acm_try_format_dec(&pcm, fmt)) != STATUS_OK)
                    return res;
                if (hStream != NULL)
                    return STATUS_OK;
            }

            return STATUS_OK;
        }

        status_t ACMStream::read_pcm(WAVEFORMATEX *in)
        {
            status_t res = close();
            if (res != STATUS_OK)
                return res;

            // We do not support PCM and IEEE FLOAT format since it doesn't require decoding/encoding
            if ((in->wFormatTag == WAVE_FORMAT_PCM) || (in->wFormatTag == WAVE_FORMAT_IEEE_FLOAT))
                return STATUS_UNSUPPORTED_FORMAT;

            // Enumerate all drivers available in the system
            acm_enum_drivers(&vDrv);

            if ((res = acm_find_nonstandard_dec(in)) != STATUS_OK)
                return close(res);
            if (hStream == NULL)
            {
                if ((res = acm_find_standard_dec(in)) != STATUS_OK)
                    return close(res);
            }

            acm_destroy_drivers(&vDrv);

            return (hStream != NULL) ? STATUS_OK : close(STATUS_UNSUPPORTED_FORMAT);
        }

        status_t ACMStream::acm_try_format_enc(HACMDRIVER drv, WAVEFORMATEX *to, WAVEFORMATEX *from, size_t szof)
        {
            size_t flags        = ACM_FORMATSUGGESTF_NCHANNELS |
                                  ACM_FORMATSUGGESTF_NSAMPLESPERSEC |
                                  ACM_FORMATSUGGESTF_WFORMATTAG;

            // Suggest format
            if ((::acmFormatSuggest(drv, from, to, szof, flags)) != 0)
                return STATUS_OK;

            // Try to open stream
            if (::acmStreamOpen(&hStream, hDriver, from, to, NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME) != 0)
                return STATUS_OK;

            // Configure ACM stream
            status_t res = acm_configure_stream(to, from);
            if (res != STATUS_OK)
                return res;

            // Copy format descriptors
            if ((pFmtIn = copy_fmt(from)) == NULL)
                return STATUS_NO_MEM;
            pFmtOut = to;   // reuse 'to' pointer;

            return STATUS_OK;
        }

        status_t ACMStream::acm_suggest_format_enc(WAVEFORMATEX *fmt)
        {
            status_t res;

            for (size_t i=0, n=vDrv.size(); i<n; ++i)
            {
                // Work with drivers that ONLY support the requested tag
                drv_t *drv = vDrv.uget(i);
                if (drv == NULL)
                    continue;
                fmt_tag_t *tag = acm_find_tag(drv, fmt->wFormatTag);
                if (tag == NULL)
                    continue;

                WAVEFORMATEX pcm, *dst;
                dst     = reinterpret_cast<WAVEFORMATEX *>(::malloc(drv->pwfx_sz));
                if (dst == NULL)
                    return STATUS_NO_MEM;
                ::ZeroMemory(dst, drv->pwfx_sz);
                dst->wFormatTag     = fmt->wFormatTag;
                dst->nChannels      = fmt->nChannels;
                dst->nSamplesPerSec = fmt->nSamplesPerSec;
                dst->cbSize         = 0;

                // Open the specific ACM driver
                if (::acmDriverOpen(&hDriver, drv->drv_id, 0) != 0)
                {
                    ::free(dst);
                    continue;
                }

                // Try PCM IIIE Float first
                pcm.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
                pcm.nChannels       = fmt->nChannels;
                pcm.nSamplesPerSec  = fmt->nSamplesPerSec;
                pcm.nAvgBytesPerSec = sizeof(f32_t) * fmt->nChannels * fmt->nSamplesPerSec;
                pcm.nBlockAlign     = sizeof(f32_t) * fmt->nChannels;
                pcm.wBitsPerSample  = sizeof(f32_t) * 8;
                pcm.cbSize          = 0;

                if ((res = acm_try_format_enc(hDriver, dst, &pcm, drv->pwfx_sz)) != STATUS_OK)
                {
                    ::free(dst);
                    return res;
                }
                else if (hStream != NULL)
                    return STATUS_OK; // dst will be saved as format descriptor

                // Try other PCM formats: 32, 24, 16, 8 bits per sample
                for (size_t bps=4; bps > 0; --bps)
                {
                    pcm.wFormatTag      = WAVE_FORMAT_PCM;
                    pcm.nChannels       = fmt->nChannels;
                    pcm.nSamplesPerSec  = fmt->nSamplesPerSec;
                    pcm.nAvgBytesPerSec = bps * fmt->nChannels * fmt->nSamplesPerSec;
                    pcm.nBlockAlign     = bps * fmt->nChannels;
                    pcm.wBitsPerSample  = bps * 8;
                    pcm.cbSize          = 0;

                    // Try non-standard modes
                    if ((res = acm_try_format_enc(hDriver, dst, &pcm, drv->pwfx_sz)) != STATUS_OK)
                    {
                        ::free(dst);
                        return res;
                    }
                    if (hStream != NULL)
                        return STATUS_OK; // dst will be saved as format descriptor
                }

                // Close ACM driver
                ::free(dst);
                ::acmDriverClose(hDriver, 0);
                hDriver     = NULL;
            }

            return STATUS_OK;
        }

        status_t ACMStream::write_pcm(WAVEFORMATEX *out)
        {
            status_t res = close();
            if (res != STATUS_OK)
                return res;

            // Enumerate all drivers available in the system
            acm_enum_drivers(&vDrv);

            if ((res = acm_suggest_format_enc(out)) != STATUS_OK)
                return close(res);

            return (hStream != NULL) ? STATUS_OK : close(STATUS_UNSUPPORTED_FORMAT);
        }

        status_t ACMStream::close(status_t res)
        {
            // Destroy information about all drivers
            acm_destroy_drivers(&vDrv);

            // Close stream
            if (hStream != NULL)
            {
                // Unprepare ACM header first
                if ((pHeader != NULL) && (pHeader->fdwStatus != 0))
                    ::acmStreamUnprepareHeader(hStream, pHeader, 0);

                ::acmStreamClose(hStream, 0);
                hStream     = NULL;
            }

            // Close driver
            if (hDriver != NULL)
            {
                ::acmDriverClose(hDriver, 0);
                hDriver     = NULL;
            }

            // Free allocated memory
            if (pFmtIn != NULL)
            {
                ::free(pFmtIn);
                pFmtIn = NULL;
            }

            if (pFmtOut != NULL)
            {
                ::free(pFmtOut);
                pFmtOut = NULL;
            }

            if (pHeader != NULL)
            {
                ::free(pHeader);
                pHeader = NULL;
            }

            return res;
        }

        ssize_t ACMStream::push(void **buf)
        {
            if (pHeader == NULL)
                return -STATUS_CLOSED;
            size_t avail    = pHeader->dwSrcUser - pHeader->cbSrcLength;
            if (buf != NULL)
                *buf            = &pHeader->pbSrc[pHeader->cbSrcLength];
            return avail;
        }

        ssize_t ACMStream::pull(void **buf, size_t size, bool force)
        {
            if (hStream == NULL)
                return -STATUS_CLOSED;

            // Check if there is enough data in data buffer
            size_t avail = pHeader->cbDstLengthUsed - pHeader->dwDstUser;
            if (avail <= 0)
            {
                // Check buffer size
                if (pHeader->cbSrcLength <= 0)
                    return (force) ? -STATUS_EOF : 0;
                pHeader->cbSrcLengthUsed    = 0;
                pHeader->cbDstLengthUsed    = 0;
                // We consider that output buffer is block-aligned and can perform conversion now
                // Perform conversion
                size_t flags = (force) ? ACM_STREAMCONVERTF_BLOCKALIGN : 0;
                if (::acmStreamConvert(hStream, pHeader, flags) != 0)
                    return -STATUS_IO_ERROR;

                // Move the source buffer
                size_t tail                 = pHeader->cbSrcLength - pHeader->cbSrcLengthUsed;
                pHeader->cbSrcLength        = tail;
                if (tail > 0)
                    ::memmove(pHeader->pbSrc, &pHeader->pbSrc[pHeader->cbSrcLengthUsed], tail);

                // Update buffer information and check again
                pHeader->dwDstUser          = 0;
                avail                       = pHeader->cbDstLengthUsed;
                if (avail <= 0)
                    return (force) ? -STATUS_EOF : 0;
            }

            // Copy data to the output buffer
            if (size > avail)
                size = avail;
            if (buf != NULL)
                *buf    = &pHeader->pbDst[pHeader->dwDstUser];
            pHeader->dwDstUser         += size;
            return size;
        }

        size_t ACMStream::avail()
        {
            if (pHeader == NULL)
                return 0;
            return pHeader->cbDstLengthUsed;
        }
    
    } /* namespace mm */
} /* namespace lsp */

#endif /* PLATFORM_WINDOWS */
