/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 20 апр. 2020 г.
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

#include <lsp-plug.in/mm/OutAudioFileStream.h>

#ifdef PLATFORM_WINDOWS
    #include <private/mm/ACMStream.h>
    #include <private/mm/MMIOWriter.h>

    #include <windows.h>
    #include <mmsystem.h>
    #include <mmreg.h>
    #include <msacm.h>
#endif /* PLATFORM_WINDOWS */

#if defined(PLATFORM_MACOSX)
    #include <CoreFoundation/CFString.h>
    #include <CoreFoundation/CFURL.h>
    #include <AudioToolbox/ExtendedAudioFile.h>
#endif /* PLATFORM_MACOSX */

#ifdef USE_LIBSNDFILE
    #if (__SIZEOF_INT__ == 4)
        #define AFS_S32_CPU     mm::SFMT_S32_CPU
    #endif

    #if (__SIZEOF_SHORT__ == 2)
        #define AFS_S16_CPU     mm::SFMT_S16_CPU
    #elif ((__SIZEOF_SHORT__ == 4) && (!defined(AFS_S32_CPU)))
        #define AFS_S32_CPU     mm::SFMT_S32_CPU
    #endif
#endif /* USE_LIBSNDFILE */

namespace lsp
{
    namespace mm
    {
    #if defined(PLATFORM_WINDOWS)
        struct WAVEFILE
        {
            MMIOWriter         *pMMIO;          // MMIO writer
            ACMStream          *pACM;           // ACM stream
            WAVEFORMATEX       *pFormat;        // Actual PCM stream format
            wsize_t             nTotalFrames;   // Total frames written
            WAVEFORMATEX        sPcmFmt;        // PCM format descriptor
        };

        static ssize_t decode_sample_format(WAVEFORMATEX *wfe)
        {
            if (wfe->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                return SFMT_F32_LE;

            if (wfe->wFormatTag == WAVE_FORMAT_PCM)
            {
                switch (wfe->wBitsPerSample)
                {
                    case 8: return SFMT_U8_CPU;
                    case 16: return SFMT_S16_LE;
                    case 24: return SFMT_S24_LE;
                    case 32: return SFMT_S32_LE;
                    default:
                        break;
                }
            }

            return -1;
        }
    #elif defined(PLATFORM_MACOSX)
        static inline ExtAudioFileRef eaf_ref(void *handle)
        {
            return static_cast<ExtAudioFileRef>(handle);
        }
    
        status_t OutAudioFileStream::decode_os_status(uint32_t code)
        {
            return (code == kAudio_NoError) ? STATUS_OK : STATUS_UNKNOWN_ERR;
        }

        uint32_t OutAudioFileStream::select_file_format(size_t codec)
        {
            // Choose audio format
            switch (codec & AFMT_MASK)
            {
                case AFMT_WAV:      return kAudioFileWAVEType;
                case AFMT_AIFF:     return kAudioFileAIFFType;
                case AFMT_AU:       return kAudioFileNextType;
                case AFMT_W64:      return kAudioFileWave64Type;
                case AFMT_SD2:      return kAudioFileSoundDesigner2Type;
                case AFMT_FLAC:     return kAudioFileFLACType;
                case AFMT_CAF:      return kAudioFileCAFType;
                case AFMT_RF64:     return kAudioFileRF64Type;
                default:
                    return 0;
            }
        }

        static bool select_sample_format(AudioStreamBasicDescription *info, const audio_stream_t *fmt, size_t codec)
        {
            bzero(info, sizeof(AudioStreamBasicDescription));

            info->mSampleRate       = fmt->srate;
            info->mFormatFlags      = 0;
            info->mChannelsPerFrame = uint32_t(fmt->channels);

            // Choose codec
            switch (codec & CFMT_MASK)
            {
                case CFMT_PCM:
                    info->mFormatID         = kAudioFormatLinearPCM;
                    break;

                case CFMT_ULAW:
                    info->mFormatID         = kAudioFormatULaw;
                    info->mFormatFlags     |= kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
                    info->mBytesPerPacket   = 0;
                    info->mFramesPerPacket  = 1;
                    info->mBytesPerFrame    = uint32_t(8 * sizeof(int8_t) * fmt->channels);
                    info->mBitsPerChannel   = 8 * sizeof(int8_t);
                    break;

                case CFMT_ALAW:
                    info->mFormatID         = kAudioFormatALaw;
                    info->mFormatFlags     |= kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
                    info->mBytesPerPacket   = 0;
                    info->mFramesPerPacket  = 1;
                    info->mBytesPerFrame    = uint32_t(8 * sizeof(int8_t) * fmt->channels);
                    info->mBitsPerChannel   = 8 * sizeof(int8_t);
                    break;

                default:
                    switch (codec & AFMT_MASK)
                    {
                        case AFMT_WAV:
                            info->mFormatID     = kAudioFormatLinearPCM;
                            break;
                        case AFMT_FLAC:
                            info->mFormatID     = kAudioFormatFLAC;
                            break;
                        default:
                            info->mFormatID     = kAudioFormatLinearPCM;
                            break;
                    }
                    break;
            }

            // Choose endianess
            switch (sformat_endian(fmt->format))
            {
                case SFMT_DFL:
                    if ((codec & AFMT_MASK) == AFMT_AU)
                        info->mFormatFlags     |= kAudioFormatFlagIsBigEndian;
                    break;
                case SFMT_LE:
                    break;
                case SFMT_BE:
                    info->mFormatFlags     |= kAudioFormatFlagIsBigEndian;
                    break;
                default:
                    return false;
            }

            if (info->mFormatID == kAudioFormatLinearPCM)
            {
                info->mFormatFlags         |= kAudioFormatFlagIsPacked;

                // Set sample format
                switch (sformat_format(fmt->format))
                {
                    case SFMT_U8:
                        info->mBytesPerPacket   = uint32_t(sizeof(uint8_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(uint8_t);
                        break;

                    case SFMT_S8:
                        info->mFormatFlags     |= kAudioFormatFlagIsSignedInteger;
                        info->mBytesPerPacket   = uint32_t(sizeof(int8_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(int8_t);
                        break;

                    case SFMT_U16:
                        info->mBytesPerPacket   = uint32_t(sizeof(uint16_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(uint16_t);
                        break;

                    case SFMT_S16:
                        info->mFormatFlags     |= kAudioFormatFlagIsSignedInteger;
                        info->mBytesPerPacket   = uint32_t(sizeof(int16_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(int16_t);
                        break;

                    case SFMT_U24:
                        info->mBytesPerPacket   = uint32_t(3 * sizeof(uint8_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 24 * sizeof(uint8_t);
                        break;

                    case SFMT_S24:
                        info->mFormatFlags     |= kAudioFormatFlagIsSignedInteger;
                        info->mBytesPerPacket   = uint32_t(3 * sizeof(int8_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 24 * sizeof(int8_t);
                        break;

                    case SFMT_U32:
                        info->mBytesPerPacket   = uint32_t(sizeof(uint32_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(uint32_t);
                        break;

                    case SFMT_S32:
                        info->mFormatFlags     |= kAudioFormatFlagIsSignedInteger;
                        info->mBytesPerPacket   = uint32_t(sizeof(int32_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(int32_t);
                        break;

                    case SFMT_F32:
                        info->mFormatFlags     |= kAudioFormatFlagIsFloat;
                        info->mBytesPerPacket   = uint32_t(sizeof(f32_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(f32_t);
                        break;

                    case SFMT_F64:
                        info->mFormatFlags     |= kAudioFormatFlagIsFloat;
                        info->mBytesPerPacket   = uint32_t(sizeof(f64_t) * fmt->channels);
                        info->mFramesPerPacket  = 1;
                        info->mBytesPerFrame    = info->mBytesPerPacket;
                        info->mBitsPerChannel   = 8 * sizeof(f64_t);
                        break;

                    default:
                        return false;
                }
            }

            return true;
        }
    #else
        status_t OutAudioFileStream::decode_sf_error(SNDFILE *fd)
        {
            switch (sf_error(fd))
            {
                case SF_ERR_NO_ERROR:
                    return STATUS_OK;
                case SF_ERR_UNRECOGNISED_FORMAT:
                    return STATUS_BAD_FORMAT;
                case SF_ERR_MALFORMED_FILE:
                    return STATUS_CORRUPTED_FILE;
                case SF_ERR_UNSUPPORTED_ENCODING:
                    return STATUS_BAD_FORMAT;
                default:
                    return STATUS_UNKNOWN_ERR;
            }
        }

        bool OutAudioFileStream::select_sndfile_format(SF_INFO *info, audio_stream_t *fmt, size_t codec)
        {
            int res = 0;

            // Choose audio format
            switch (codec & AFMT_MASK)
            {
                case AFMT_WAV:          res |= SF_FORMAT_WAV; break;
                case AFMT_AIFF:         res |= SF_FORMAT_AIFF; break;
                case AFMT_AU:           res |= SF_FORMAT_AU; break;
                case AFMT_RAW:          res |= SF_FORMAT_RAW; break;
                case AFMT_PAF:          res |= SF_FORMAT_PAF; break;
                case AFMT_SVX:          res |= SF_FORMAT_SVX; break;
                case AFMT_NIST:         res |= SF_FORMAT_NIST; break;
                case AFMT_VOC:          res |= SF_FORMAT_VOC; break;
                case AFMT_IRCAM:        res |= SF_FORMAT_IRCAM; break;
                case AFMT_W64:          res |= SF_FORMAT_W64; break;
                case AFMT_MAT4:         res |= SF_FORMAT_MAT4; break;
                case AFMT_MAT5:         res |= SF_FORMAT_MAT5; break;
                case AFMT_PVF:          res |= SF_FORMAT_PVF; break;
                case AFMT_XI:           res |= SF_FORMAT_XI; break;
                case AFMT_HTK:          res |= SF_FORMAT_HTK; break;
                case AFMT_SDS:          res |= SF_FORMAT_SDS; break;
                case AFMT_AVR:          res |= SF_FORMAT_AVR; break;
                case AFMT_WAVEX:        res |= SF_FORMAT_WAVEX; break;
                case AFMT_SD2:          res |= SF_FORMAT_SD2; break;
                case AFMT_FLAC:         res |= SF_FORMAT_FLAC; break;
                case AFMT_CAF:          res |= SF_FORMAT_CAF; break;
                case AFMT_WVE:          res |= SF_FORMAT_WVE; break;
                case AFMT_OGG:          res |= SF_FORMAT_OGG; break;
                case AFMT_MPC2K:        res |= SF_FORMAT_MPC2K; break;
                case AFMT_RF64:         res |= SF_FORMAT_RF64; break;
                default:
                    return false;
            }

            // Choose codec
            switch (codec & CFMT_MASK)
            {
                case CFMT_PCM:
                {
                    // Set sample format
                    switch (sformat_format(fmt->format))
                    {
                        case SFMT_U8:   res |= SF_FORMAT_PCM_U8; break;
                        case SFMT_S8:   res |= SF_FORMAT_PCM_S8; break;
                        case SFMT_U16:
                        case SFMT_S16:
                            res |= SF_FORMAT_PCM_16;
                            break;
                        case SFMT_S24:
                        case SFMT_U24:
                            res |= SF_FORMAT_PCM_24;
                            break;
                        case SFMT_S32:
                        case SFMT_U32:
                            res |= SF_FORMAT_PCM_32;
                            break;

                        case SFMT_F32:  res |= SF_FORMAT_FLOAT; break;
                        case SFMT_F64:  res |= SF_FORMAT_DOUBLE; break;
                        default:
                            return false;
                    }
                }
                break;

                case CFMT_ULAW:         res |= SF_FORMAT_ULAW; break;
                case CFMT_ALAW:         res |= SF_FORMAT_ALAW; break;
                case CFMT_IMA_ADPCM:    res |= SF_FORMAT_IMA_ADPCM; break;
                case CFMT_MS_ADPCM:     res |= SF_FORMAT_MS_ADPCM; break;
                case CFMT_GSM610:       res |= SF_FORMAT_GSM610; break;
                case CFMT_VOX_ADPCM:    res |= SF_FORMAT_VOX_ADPCM; break;
                case CFMT_G721_32:      res |= SF_FORMAT_G721_32; break;
                case CFMT_G723_24:      res |= SF_FORMAT_G723_24; break;
                case CFMT_G723_40:      res |= SF_FORMAT_G723_40; break;
                case CFMT_DWVW_12:      res |= SF_FORMAT_DWVW_12; break;
                case CFMT_DWVW_16:      res |= SF_FORMAT_DWVW_16; break;
                case CFMT_DWVW_24:      res |= SF_FORMAT_DWVW_24; break;
                case CFMT_DWVW_N:       res |= SF_FORMAT_DWVW_N; break;
                case CFMT_DPCM_8:       res |= SF_FORMAT_DPCM_8; break;
                case CFMT_DPCM_16:      res |= SF_FORMAT_DPCM_16; break;
                case CFMT_VORBIS:       res |= SF_FORMAT_VORBIS; break;
            #ifdef LIBSNDFILE_HAS_ALAC
                case CFMT_ALAC_16:      res |= SF_FORMAT_ALAC_16; break;
                case CFMT_ALAC_20:      res |= SF_FORMAT_ALAC_20; break;
                case CFMT_ALAC_24:      res |= SF_FORMAT_ALAC_24; break;
                case CFMT_ALAC_32:      res |= SF_FORMAT_ALAC_32; break;
            #endif /* LIBSNDFILE_HAS_ALAC */

                default:
                    return false;
            }

            // Choose endianess
            switch (sformat_endian(fmt->format))
            {
                case SFMT_DFL:          res |= SF_ENDIAN_FILE; break;
                case SFMT_LE:           res |= SF_ENDIAN_LITTLE; break;
                case SFMT_BE:           res |= SF_ENDIAN_BIG; break;
                default:
                    return false;
            }

            info->frames        = fmt->frames;
            info->samplerate    = fmt->srate;
            info->channels      = fmt->channels;
            info->format        = res;
            info->sections      = 0;
            info->seekable      = 0;

            return true;
        }
    #endif

        OutAudioFileStream::OutAudioFileStream()
        {
            hHandle         = NULL;
            nCodec          = 0;
            bSeekable       = false;
        }

        OutAudioFileStream::~OutAudioFileStream()
        {
            IOutAudioStream::close();
            do_close();
        }

        status_t OutAudioFileStream::open(const char *path, const audio_stream_t *fmt, size_t codec)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);
            LSPString xpath;
            if (!xpath.set_utf8(path))
                return set_error(STATUS_NO_MEM);
            return open(&xpath, fmt, codec);
        }

        status_t OutAudioFileStream::open(const io::Path *path, const audio_stream_t *fmt, size_t codec)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);
            return open(path->as_string(), fmt, codec);
        }

        status_t OutAudioFileStream::open(const LSPString *path, const audio_stream_t *fmt, size_t codec)
        {
            if (!is_closed())
                return set_error(STATUS_OPENED);
            if (fmt == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

        #if defined(PLATFORM_WINDOWS)
            if ((codec & AFMT_MASK) != AFMT_WAV)
                return set_error(STATUS_UNSUPPORTED_FORMAT);

            WAVEFORMATEX rfmt;

            ::ZeroMemory(&rfmt, sizeof(WAVEFORMATEX));
            rfmt.cbSize             = 0;
            rfmt.nChannels          = fmt->channels;
            rfmt.nSamplesPerSec     = fmt->srate;
            rfmt.wBitsPerSample     = sformat_size_of(fmt->format) * 8;
            rfmt.nBlockAlign        = fmt->channels * sformat_size_of(fmt->format);
            rfmt.nAvgBytesPerSec    = fmt->srate * rfmt.nBlockAlign;

            switch (codec & CFMT_MASK)
            {
                case CFMT_PCM:
                    rfmt.wFormatTag  = (sformat_format(fmt->format) == SFMT_F32) ?
                            WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
                    break;

                case CFMT_ULAW:         rfmt.wFormatTag = WAVE_FORMAT_MULAW; break;
                case CFMT_ALAW:         rfmt.wFormatTag = WAVE_FORMAT_ALAW; break;
                case CFMT_MS_ADPCM:     rfmt.wFormatTag = WAVE_FORMAT_ADPCM; break;
                case CFMT_IMA_ADPCM:    rfmt.wFormatTag = WAVE_FORMAT_IMA_ADPCM; break;

                case CFMT_GSM610:       rfmt.wFormatTag = WAVE_FORMAT_GSM610; break;

                case CFMT_G721_32:      rfmt.wFormatTag = WAVE_FORMAT_G721_ADPCM; break;
                case CFMT_G723_24:      rfmt.wFormatTag = WAVE_FORMAT_G723_ADPCM; break;
                case CFMT_G723_40:      rfmt.wFormatTag = WAVE_FORMAT_G723_ADPCM; break;

                default:
                    return set_error(STATUS_UNSUPPORTED_FORMAT);
            }

            // Allocate handle
            handle_t *h     = static_cast<handle_t *>(malloc(sizeof(handle_t)));
            if (h == NULL)
                return set_error(STATUS_NO_MEM);
            h->pMMIO        = NULL;
            h->pACM         = NULL;
            h->pFormat      = NULL;
            h->nTotalFrames = 0;
            lsp_finally { close_handle(h);  };

            // Create MMIO
            status_t res;
            h->pMMIO        = new MMIOWriter();
            if (h->pMMIO == NULL)
                return set_error(STATUS_NO_MEM);

            if ((rfmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                (rfmt.wFormatTag == WAVE_FORMAT_PCM))
            {
                // Just open MMIO stream with specified format
                if ((res = h->pMMIO->open(path, &rfmt, fmt->frames)) != STATUS_OK)
                    return set_error(res);

                // Update state
                h->nTotalFrames     = 0;
                h->pACM             = NULL;
                h->sPcmFmt          = rfmt;
                h->pFormat          = &h->sPcmFmt;

                sFormat             = *fmt;

                nCodec              = codec;
                nOffset             = 0;
                bSeekable           = h->pMMIO->seekable();

                // Commit the new handle and return success
                lsp::swap(hHandle, h);
                return set_error(STATUS_OK);
            }

            // Create ACM stream first and initialize it
            h->pACM         = new ACMStream();
            if (h->pACM == NULL)
                return set_error(STATUS_NO_MEM);
            if ((res = h->pACM->write_pcm(&rfmt)) != STATUS_OK)
                return set_error(res);

            // Detect format
            h->pFormat          = h->pACM->in_format();
            ssize_t sfmt        = decode_sample_format(h->pFormat);
            if (sfmt <= 0)
                return set_error(STATUS_UNSUPPORTED_FORMAT);

            // Now open MMIO with specified output format
            if ((res = h->pMMIO->open(path, h->pACM->out_format(), fmt->frames)) != STATUS_OK)
                return set_error(res);

            // All is OK, update state
            h->nTotalFrames     = 0;

            sFormat.srate       = h->pFormat->nSamplesPerSec;
            sFormat.channels    = h->pFormat->nChannels;
            sFormat.frames      = fmt->frames;
            sFormat.format      = sfmt;

            nCodec              = codec;
            nOffset             = 0;
            bSeekable           = false;

            // Commit the new handle and return success
            lsp::swap(hHandle, h);
            return set_error(STATUS_OK);
        #elif defined(PLATFORM_MACOSX)
            // Form the URL to open
            CFStringRef str_ref = CFStringCreateWithCString(
                kCFAllocatorDefault,
                path->get_utf8(),
                kCFStringEncodingUTF8);
            lsp_finally { CFRelease(str_ref); };
            CFURLRef url_ref = CFURLCreateWithFileSystemPath(
                kCFAllocatorDefault,
                str_ref,
                kCFURLPOSIXPathStyle,
                false);
            lsp_finally { CFRelease(url_ref); };

            const AudioFileTypeID file_type = select_file_format(codec);
            if (file_type == 0)
                return set_error(STATUS_UNSUPPORTED_FORMAT);

            AudioStreamBasicDescription info;
            if (!select_sample_format(&info, fmt, codec))
                return set_error(STATUS_UNSUPPORTED_FORMAT);

            // Open file
            ExtAudioFileRef eaf = NULL;
            OSStatus os_res = ExtAudioFileCreateWithURL(
                url_ref,
                file_type,
                &info,
                NULL,
                kAudioFileFlags_EraseFile,
                &eaf);
            if (os_res != kAudio_NoError)
                return set_error(decode_os_status(os_res));
            lsp_finally {
                if (eaf != NULL)
                    ExtAudioFileDispose(eaf);
            };

            // Decode sample format
            size_t format       = 0;
            size_t be_flag      = (info.mFormatFlags & kAudioFormatFlagIsBigEndian) ? mm::SFMT_BE : mm::SFMT_LE;
            bool need_convert   = info.mFormatID != kAudioFormatLinearPCM;
            if (!need_convert)
            {
                if (info.mFormatFlags & kAudioFormatFlagIsFloat)
                {
                    if (info.mBitsPerChannel == sizeof(float) * 8)
                        format          = mm::SFMT_F32 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(double) * 8)
                        format          = mm::SFMT_F64 | be_flag;
                    else
                        need_convert    = true;
                }
                else if (info.mFormatFlags & kAudioFormatFlagIsSignedInteger)
                {
                    if (info.mBitsPerChannel == sizeof(int8_t) * 8)
                        format          = mm::SFMT_S8 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(int16_t) * 8)
                        format          = mm::SFMT_S16 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(int8_t) * 24)
                        format          = mm::SFMT_S24 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(int32_t) * 8)
                        format          = mm::SFMT_S32 | be_flag;
                    else
                        need_convert    = true;
                }
                else
                {
                    if (info.mBitsPerChannel == sizeof(uint8_t) * 8)
                        format          = mm::SFMT_U8 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(uint16_t) * 8)
                        format          = mm::SFMT_U16 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(uint8_t) * 24)
                        format          = mm::SFMT_U24 | be_flag;
                    else if (info.mBitsPerChannel == sizeof(uint32_t) * 8)
                        format          = mm::SFMT_U32 | be_flag;
                    else
                        need_convert    = true;
                }
            }

            // Initialize audio converter if needed to match the selected sample format
            if (need_convert)
            {
                AudioStreamBasicDescription cvt;
                bzero(&cvt, sizeof(AudioStreamBasicDescription));
                cvt.mSampleRate       = info.mSampleRate;
                cvt.mFormatID         = kAudioFormatLinearPCM;
                cvt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;

                if (info.mBitsPerChannel <= sizeof(f32_t)*8)
                {
                    cvt.mBytesPerPacket   = sizeof(f32_t) * info.mChannelsPerFrame;
                    cvt.mFramesPerPacket  = 1;
                    cvt.mBytesPerFrame    = sizeof(f32_t) * info.mChannelsPerFrame;
                    cvt.mChannelsPerFrame = info.mChannelsPerFrame;
                    cvt.mBitsPerChannel   = sizeof(f32_t) * 8;
                    format                = mm::SFMT_F32 | be_flag;
                }
                else
                {
                    cvt.mBytesPerPacket   = sizeof(f64_t) * info.mChannelsPerFrame;
                    cvt.mFramesPerPacket  = 1;
                    cvt.mBytesPerFrame    = sizeof(f64_t) * info.mChannelsPerFrame;
                    cvt.mChannelsPerFrame = info.mChannelsPerFrame;
                    cvt.mBitsPerChannel   = sizeof(f64_t) * 8;
                    format                = mm::SFMT_F64 | be_flag;
                }

                os_res = ExtAudioFileSetProperty(
                    eaf,
                    kExtAudioFileProperty_ClientDataFormat,
                    sizeof(AudioStreamBasicDescription),
                    &cvt);

                if (os_res != kAudio_NoError)
                    return set_error(decode_os_status(os_res));
            }

            // Update state and return
            sFormat.srate       = info.mSampleRate;
            sFormat.channels    = info.mChannelsPerFrame;
            sFormat.frames      = fmt->frames;
            sFormat.format      = format;

            nOffset             = 0;
            bSeekable           = false;
            hHandle             = release_ptr(eaf);

            return set_error(STATUS_OK);
        #else
            audio_stream_t tmp;
            SF_INFO info;
            tmp     = *fmt;

            if (!select_sndfile_format(&info, &tmp, codec))
                return set_error(STATUS_UNSUPPORTED_FORMAT);

            // Open handle
            SNDFILE *sf = sf_open(path->get_native(), SFM_WRITE, &info);
            if (sf == NULL)
                return -set_error(decode_sf_error(sf));

            // Commit new state
            sFormat             = tmp;
            hHandle             = sf;
            nOffset             = 0;
            bSeekable           = info.seekable;

            return set_error(STATUS_OK);
        #endif
        }

        status_t OutAudioFileStream::close_handle(handle_t *h)
        {
            if (h == NULL)
                return STATUS_OK;
            lsp_finally { h = NULL; };

        #if defined(PLATFORM_WINDOWS)
            status_t res = STATUS_OK;

            // Flush all previously encoded data and close ACM
            if (h->pACM != NULL)
            {
                res         = update_status(res, h->pACM->close());
                delete h->pACM;
                h->pACM     = NULL;
            }

            // Close MMIO
            if (h->pMMIO != NULL)
            {
                h->pMMIO->set_frames(h->nTotalFrames);
                res         = update_status(res, h->pMMIO->close());
                delete h->pMMIO;
                h->pMMIO    = NULL;
            }

            h->pFormat  = NULL;

            free(h);

            return res;
        #elif defined(PLATFORM_MACOSX)
            OSStatus res = ExtAudioFileDispose(eaf_ref(h));
            return decode_os_status(res);
        #else
            int res     = sf_close(h);
            return (res == 0) ? STATUS_OK : STATUS_IO_ERROR;
        #endif
        }

        status_t OutAudioFileStream::do_close()
        {
            if (hHandle == NULL)
                return STATUS_OK;

            status_t res = flush_internal(true);
            status_t res2= close_handle(hHandle);

            hHandle     = NULL;
            bSeekable   = false;
            nOffset     = -1;       // Mark as closed
            nCodec      = 0;

            return set_error((res == STATUS_OK) ? res2 : res);
        }

    #if defined(PLATFORM_WINDOWS)
        status_t OutAudioFileStream::flush_handle(handle_t *hHandle, bool eof)
        {
            if (hHandle->pACM == NULL)
                return STATUS_OK;

            void *dptr;

            while (true)
            {
                // Perform pull of portion of buffer
                ssize_t count       = hHandle->pACM->pull(&dptr, IO_BUF_SIZE, eof);
                if (count < 0)
                    return (count == -STATUS_EOF) ? STATUS_OK : -count;
                else if (count == 0)
                    break;

                // Write pulled data to underlying storage
                count               = hHandle->pMMIO->write(dptr, count);
                if (count < 0)
                    return count;
            }

            return STATUS_OK;
        }
    #endif /* PLATFORM_WINDOWS */

        status_t OutAudioFileStream::flush_internal(bool eof)
        {
        #if defined(PLATFORM_WINDOWS)
            status_t res = flush_handle(hHandle, eof);

            return (res == STATUS_OK) ? hHandle->pMMIO->flush() : res;
        #elif defined(PLATFORM_MACOSX)
            return STATUS_OK;
        #else
            sf_write_sync(hHandle);
            return STATUS_OK;
        #endif /* USE_LIBSNDFILE */
        }

        status_t OutAudioFileStream::flush()
        {
            if (is_closed())
                return -set_error(STATUS_CLOSED);

            return set_error(flush_internal(false));
        }

        status_t OutAudioFileStream::close()
        {
            IOutAudioStream::close();
            return do_close();
        }

        ssize_t OutAudioFileStream::direct_write(const void *src, size_t nframes, size_t fmt)
        {
        #if defined(PLATFORM_WINDOWS)
            size_t fsize    = hHandle->pFormat->nBlockAlign;
            if (hHandle->pMMIO != NULL)
            {
                if (hHandle->pACM != NULL)
                    return write_acm_convert(src, nframes);

                ssize_t nwritten    = hHandle->pMMIO->write(src, fsize * nframes);
                return (nwritten < 0) ? nwritten : nwritten / fsize;
            }

            return -STATUS_NOT_SUPPORTED;
        #elif defined(PLATFORM_MACOSX)
            const size_t fsize          = sformat_size_of(sFormat.format) * sFormat.channels;

            AudioBufferList list;
            AudioBuffer *buf = &list.mBuffers[0];

            list.mNumberBuffers         = 1;
            buf->mNumberChannels        = UInt32(sFormat.channels);
            buf->mDataByteSize          = UInt32(fsize * nframes);
            buf->mData                  = const_cast<void *>(src);

            UInt32 count                = UInt32(nframes);
            OSStatus os_res             = ExtAudioFileWrite(eaf_ref(hHandle), count, &list);
            if (os_res != kAudio_NoError)
                return -set_error(decode_os_status(os_res));
            return count;
        #else
            sf_count_t count;
            status_t res;

            switch (sformat_format(fmt))
            {
                case SFMT_S32:
                    count   = sf_writef_int(hHandle, static_cast<const int *>(src), nframes);
                    break;

                case SFMT_S16:
                    count   = sf_writef_short(hHandle, static_cast<const short *>(src), nframes);
                    break;

                case SFMT_F32:
                    count   = sf_writef_float(hHandle, static_cast<const float *>(src), nframes);
                    break;

                case SFMT_F64:
                    count   = sf_writef_double(hHandle, static_cast<const double *>(src), nframes);
                    break;

                default: // Force SFMT_F32 sample format
                    count   = sf_writef_float(hHandle, static_cast<const float *>(src), nframes);
                    break;
            }

            // Is there data written?
            if (count > 0)
                return count;

            res = decode_sf_error(hHandle);
            return -((res == STATUS_OK) ? STATUS_EOF : res);
        #endif
        }

    #if defined(PLATFORM_WINDOWS)
        ssize_t OutAudioFileStream::conv_write(const void *src, size_t nframes, size_t fmt)
        {
            ssize_t res = IOutAudioStream::conv_write(src, nframes, fmt);
            if (size_t(nOffset) > hHandle->nTotalFrames)
                hHandle->nTotalFrames   = nOffset;
            return res;
        }

        ssize_t OutAudioFileStream::write_acm_convert(const void *src, size_t nframes)
        {
            size_t nwritten     = 0;
            void *dptr;
            const uint8_t *sptr = static_cast<const uint8_t *>(src);
            size_t fsize        = sformat_size_of(sFormat.format) * sFormat.channels;
            nframes            *= fsize;

            while (nwritten < nframes)
            {
                // Perform push if possible
                ssize_t count       = hHandle->pACM->push(&dptr);
                if (count > 0) // We're still able to push data
                {
                    // Copy data to buffer
                    ssize_t can_push    = nframes - nwritten;
                    if (count > can_push)
                        count   = can_push;
                    ::memcpy(dptr, sptr, count);

                    // Update pointers
                    sptr       += count;
                    nwritten   += count;
                    hHandle->pACM->commit(count);
                    continue;
                }
                else if (count < 0) // Error occurred?
                {
                    if (nwritten > 0)
                        break;
                    return -set_error(STATUS_UNKNOWN_ERR);
                }

                // Need to perform flush
                status_t res    = flush_internal(false);
                if (res != STATUS_OK)
                {
                    if (nwritten > 0)
                        break;
                    return -set_error(res);
                }
            }

            return nwritten / fsize;
        }

    #endif /* PLATFORM_WINDOWS */

        size_t OutAudioFileStream::select_format(size_t rfmt)
        {
        #if defined(PLATFORM_WINDOWS)
            if (hHandle->pFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                return SFMT_F32_LE;

            if (hHandle->pFormat->wFormatTag == WAVE_FORMAT_PCM)
            {
                switch (hHandle->pFormat->wBitsPerSample)
                {
                    case 8: return SFMT_U8_CPU;
                    case 16: return SFMT_S16_LE;
                    case 24: return SFMT_S24_LE;
                    case 32: return SFMT_S32_LE;
                    default:
                        break;
                }
            }

            return -1;
        #elif defined(PLATFORM_MACOSX)
            return sFormat.format;
        #else
            switch (sformat_format(rfmt))
            {
            #ifdef AFS_S32_CPU
                case SFMT_S32:
                case SFMT_U32:
                case SFMT_S24:
                case SFMT_U24:
                    return SFMT_S32_CPU;
            #endif

            #ifdef AFS_S16_CPU
                case SFMT_S16:
                case SFMT_U16:
                case SFMT_S8:
                case SFMT_U8:
                    return SFMT_S16_CPU;
            #endif

                case SFMT_F32:
                    return SFMT_F32_CPU;

                case SFMT_F64:
                    return SFMT_F64_CPU;

                default:
                    break;
            }

            return SFMT_F32_CPU;
        #endif
        }

        wssize_t OutAudioFileStream::seek(wsize_t nframes)
        {
            if (is_closed())
                return -set_error(STATUS_CLOSED);

        #if defined(PLATFORM_WINDOWS)
            size_t fsize    = hHandle->pFormat->nBlockAlign;
            if ((hHandle->pMMIO != NULL) && (bSeekable))
            {
                wssize_t res = hHandle->pMMIO->seek(nframes * fsize);
                if (res < 0)
                {
                    set_error(-res);
                    return res;
                }

                set_error(STATUS_OK);
                return res / fsize;
            }

            return -set_error(STATUS_NOT_IMPLEMENTED);
        #elif defined(PLATFORM_MACOSX)
            OSStatus os_res             = ExtAudioFileSeek(eaf_ref(hHandle), nframes);
            if (os_res != kAudio_NoError)
                return -set_error(decode_os_status(os_res));

            nOffset     = nframes;
            set_error(STATUS_OK);
            return nframes;
        #else
            sf_count_t offset = sf_seek(hHandle, nframes, SEEK_SET);
            if (offset < 0)
                return -set_error(decode_sf_error(hHandle));

            set_error(STATUS_OK);
            return nOffset = offset;
        #endif /* USE_LIBSNDFILE */
        }
    } /* namespace mm */
} /* namespace lsp */
