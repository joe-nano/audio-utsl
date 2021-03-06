/* audio_utsl.c: Simple audio library you can build from source.
 * Copyright (c) 2018 Chris White (cxw/Incline).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://www.apache.org/licenses/LICENSE-2.0.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Headers ================================================================ */

#include "audio_utsl.h"

/* Implementation headers */
#include <sndfile.h>
#include <portaudio.h>

#include <pthread.h>
#include <semaphore.h>
#include <string.h>

#define _USE_MATH_DEFINES
    /* Or you don't get M_PI from math.h on my system */
#include <math.h>

#include "pa_ringbuffer.h"

/* Private definitions ==================================================== */

/* Static assert - Copyright 2008 Padraig Brady (pixelbeat.org)
 * http://www.pixelbeat.org/programming/gcc/static_assert.html
 * Copying and distribution of this file, with or without modification,
 * are permitted in any medium without royalty provided the copyright
 * notice and this notice are preserved.
*/
#define ASSERT_CONCAT_(a, b) a##b
#define ASSERT_CONCAT(a, b) ASSERT_CONCAT_(a, b)
/* These can't be used after statements in c89. */
#ifdef __COUNTER__
  #define STATIC_ASSERT(e,m) \
    ;enum { ASSERT_CONCAT(static_assert_, __COUNTER__) = 1/(int)(!!(e)) }
#else
  /* This can't be used twice on the same line so ensure if using in headers
   * that the headers are not included twice (by wrapping in #ifndef...#endif)
   * Note it doesn't cause an issue when used on same line of separate modules
   * compiled with gcc -combine -fwhole-program.  */
  #define STATIC_ASSERT(e,m) \
    ;enum { ASSERT_CONCAT(assert_line_, __LINE__) = 1/(int)(!!(e)) }
#endif

/* Assumptions --- TODO remove these */
STATIC_ASSERT( sizeof(int) == 4, "int must be 4 bytes");
STATIC_ASSERT( sizeof(short) == 2, "short must be 2 bytes");
STATIC_ASSERT( sizeof(float) == 4, "float must be 4 bytes");

/* Helper macros ------------------------------------------------ */
#define UNUSED(x) ((void)(x))

/* Internal parameters ------------------------------------------ */
/* TODO make these variables? */

/** The maximum number of channels we support */
#define PA_MAX_CHANNELS (2)

/** The number of frames in a PortAudio buffer */
#define PA_BUFFER_FRAMECOUNT (256)

/** The number of PortAudio buffers in a libsndfile ring buffer.
 * Must be a power of 2 (PortAudio requirement). */
#define PA_RING_BUFFERCOUNT (32)

/** PAPlayCallback_() State.  Sent by the SF reader to the playback
 * thread. */
typedef enum PPPS {
    /** Output audio data */
    PPPS_Playing,
    /** Stop the stream */
    PPPS_Stopped
} PPPS;

/** Counts of frames. */
typedef long int Au_FrameCount;

/* Private types ========================================================== */

/** The non-opaque counterpart of a HAU. */
typedef struct Au_Output *PAU;

/** The void* provided to PortAudio callbacks. */
typedef struct Au_Userdata {
    PAU pau;
    void *data;
} Au_Userdata, *PAU_Userdata;

/** A buffer from the file reader to the PA callback.
 * The data member is large enough to hold the largest buffer.
 * required */
typedef struct FRBuf {
    /** What the playback routine should do. */
    PPPS state;
    /** What position we're at in the file. */
    Au_FrameCount pos_frames;
    /** The audio data */
    unsigned char data[PA_BUFFER_FRAMECOUNT * PA_MAX_CHANNELS * sizeof(float)];
} FRBuf, *PFRBuf;

/** The internal details of a single output (HAU). */
typedef struct Au_Output {
    /* --- General parameters ------------------------- */

    /** A copy of the AU sample format */
    Au_SampleFormat format;

    /** The sample rate */
    int sample_rate;

    /** The number of channels */
    int channels;

    /* --- PortAudio - output ------------------------- */

    /** The PortAudio stream */
    PaStream *pa_stream;

    /** The callback that does the work.  PACallback_() dispatches to
     * this function. */
    PaStreamCallback *pa_callback;

    /* Userdata for the pa_callback */
    void *pa_callback_userdata;

    /* --- libsndfile - input ------------------------- */

    /** The thread that reads from the input file */
    pthread_t sf_reader_thread_storage;

    /** How we access the reader thread.  NULL means
     * sf_reader_thread_storage does not have a valid value. */
    pthread_t *sf_reader_thread;

    /** The semaphore that the reader thread blocks on.  Signaled when
     * the reader pulls data from the ring buffer, or when the reader
     * thread should exit. */
    sem_t sf_reader_semaphore_storage;

    /** How we refer to sf_reader_semaphore_storage.  Since it's a
     * pointer, we can check it for NULLs. */
    sem_t *sf_reader_semaphore;

    /** If TRUE, the reader should exit when it wakes up.  Set by the
     * calling thread. */
    BOOL sf_reader_should_exit;

    /** The file currently being read.  TODO refactor for playlist support. */
    SNDFILE *sf_fd;

    /* --- Playback buffer ---------------------------- */

    /** The ring buffer that is loaded by the reader thread.  Holds
     * FRBuf structures. */
    PaUtilRingBuffer sf_buffer_storage;

    /** How we access the ring buffer */
    PaUtilRingBuffer *sf_buffer;

    /** The memory area where the ring buffer lives. */
    void *sf_buffer_data;

    /** The current time in the stream, if nonnegative. */
    PaTime playback_time;

    /** The time the stream started, if nonnegative. */
    PaTime playback_start_time;

    /** Whether or not a file is playing */
    BOOL is_playing;

    /** The mutex protecting playback_time, playback_start_time,
     * and is_playing. */
    pthread_mutex_t playback_time_mutex_storage;

    /** How we access playback_time_mutex_storage. */
    pthread_mutex_t *playback_time_mutex;

    /** The current frame count in the stream.  Not mutex-protected
     * because it is only accessed by the SFFileReader_() thread. */
    Au_FrameCount playback_frames;

} Au_Output;

/** For convenience - map from the opaque HAU provided by the caller to
 * the visible Au_Output * that we use.
 */
#define POW_FAST \
    PAU pau = (PAU)handle;

/** Common entry code for PortAudio callbacks (see PACallback_()). */
#define POW_UD_FAST \
    Au_Userdata *pud = (Au_Userdata *)handle; \
    PAU pau = pud->pau;

/** Common entry code for several Au_* functions. */
#define POW \
    if(!AuInitialized_) return FALSE; \
    POW_FAST \
    if(!pau) return FALSE;


/* Globals ================================================================ */
BOOL AuInitialized_ = FALSE;

/* Internal helpers ======================================================= */

/** Get the size of a PortAudio buffer, in bytes.
 * @return The size, or -1 on error. */
int bufferSizeBytes_(PAU pau)
{
    if(!pau) return -1;
    int format_size;

    switch(pau->format) {
        case AUSF_F32: format_size = 4; break;
        case AUSF_I32: format_size = 4; break;
        case AUSF_I24: format_size = 3; /*TODO aligned?*/ break;
        case AUSF_I16: format_size = 2; break;
        case AUSF_I8: format_size = 1; break;
        case AUSF_UI8: format_size = 1; break;
        case AUSF_CUSTOM: return -1;
                            /* TODO figure this out */
                          break;
        default: return -1; break;
    }

    return PA_BUFFER_FRAMECOUNT * pau->channels * format_size;
} /* bufferSizeBytes_ */

/* PortAudio callbacks ==================================================== */

/** Main callback for all PortAudio streams.
 * The callback is a thunk to the actual callback, stored in pau.
 */
static int PACallback_(const void *input, void *output,
    unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void *handle )
{
    POW_FAST
    Au_Userdata ud = {pau, pau->pa_callback_userdata};
    return pau->pa_callback(input, output, frameCount, timeInfo,
            statusFlags, (void *)&ud);
}

static int PAEmptyCallback_(const void *input, void *output,
    unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void *handle )
{
    return paComplete;      /* no audio */
}

/* libsndfile code ======================================================== */

unsigned int AU_SFFR_Count = 0;    /* for debugging */
/** The worker thread that reads data from a file. */
static void *SFFileReader_(void *handle)
{
    POW
    if(pau->format == AUSF_CUSTOM) {
        AU_SFFR_Count = 123000;
        return 0;    /* TODO */
    }

    //AU_SFFR_Count+=2;

    void *data1, *data2;
    ring_buffer_size_t buffers_avail, elems1, elems2, ok;
    sf_count_t frames_read;
    sf_count_t frames_wanted = PA_BUFFER_FRAMECOUNT;
    PFRBuf pfr;

    while(1) {
        sem_wait(pau->sf_reader_semaphore);
        //AU_SFFR_Count+=2;
        if(pau->sf_reader_should_exit) {
            AU_SFFR_Count = 123456789;
            break;  /* EXIT POINT */
        }

        /* Find out where to put the data.
         * For now, every time the file_reader thread wakes up, fill the
         * ring buffer. */
        buffers_avail =
            PaUtil_GetRingBufferWriteAvailable(pau->sf_buffer);

        /* Read the data.  For now, do one element at a time so I don't
         * have to deal with data1/data2. */
        for( ; buffers_avail>0 ; --buffers_avail) {
            ++AU_SFFR_Count;
            ok = PaUtil_GetRingBufferWriteRegions( pau->sf_buffer, 1,
                    &data1, &elems1, &data2, &elems2);
            if(ok <= 0 || elems1 <= 0) {
                AU_SFFR_Count = 123001;
                break;
            }

            pfr = (PFRBuf)data1;
            //pfr->pos_frames = sf_seek(pau->sf_fd, SEEK_CUR, 0);
                /* Get the offset of the beginning of what we are
                 * reading */
                // TODO cache this?

            /* NOTE: we currently use the STATIC_ASSERT checks above
             * to guarantee that, e.g., sf_read_float is giving us
             * 32 bits at a time.  If those checks ever go away, this
             * switch will need to change correspondingly. */

            switch(pau->format) {   /* TODO optimize this */
                case AUSF_F32:
                    frames_read = sf_readf_float(pau->sf_fd, (float *)pfr->data,
                            PA_BUFFER_FRAMECOUNT);
                    if(frames_read > 0 && frames_read != frames_wanted) {
                        AU_SFFR_Count = 123002;
                        return 0;   /* EXIT POINT */
                    }
                    break;

                case AUSF_I32:
                    frames_read = sf_readf_int(pau->sf_fd, (int *)pfr->data,
                            PA_BUFFER_FRAMECOUNT);
                    if(frames_read > 0 && frames_read != frames_wanted) {
                        AU_SFFR_Count = 123003;
                        return 0;   /* EXIT POINT */
                        // TODO fill out the rest of the buffer with
                        // zeros instead
                    }
                    break;

                case AUSF_I24:
                    AU_SFFR_Count = 123004;
                    return 0;   /* EXIT POINT */ /* TODO handle this */
                    break;

                case AUSF_I16:
                    frames_read = sf_readf_short(pau->sf_fd, (short *)pfr->data,
                            PA_BUFFER_FRAMECOUNT);
                    if(frames_read > 0 && frames_read != frames_wanted) {
                        AU_SFFR_Count = 123005;
                        return 0;   /* EXIT POINT */
                    }
                    break;

                case AUSF_I8:
                    AU_SFFR_Count = 123006;
                    return 0;   /* EXIT POINT */ /* TODO handle this */
                    break;
                case AUSF_UI8:
                    AU_SFFR_Count = 123007;
                    return 0;   /* EXIT POINT */ /* TODO handle this */
                    break;
                default:
                    AU_SFFR_Count = 123008;
                    return 0;
                    break;
            } /* switch(format) */

            /* Sync info */
            pfr->pos_frames = pau->playback_frames;
            pau->playback_frames += frames_read;

            if(frames_read == 0) {       /* Report EOF */
                pfr->state = PPPS_Stopped;
                AU_SFFR_Count |= 0x01;
            } else {                    /* Play it */
                pfr->state = PPPS_Playing;
                AU_SFFR_Count &= (unsigned long int)(-2);
            }

            /* Send pfr to the PortAudio callback */
            pfr = NULL;
            PaUtil_AdvanceRingBufferWriteIndex(pau->sf_buffer, 1);
        } /* while buffers_avail */

        //AU_SFFR_Count+=2;
    } /* thread main loop */

    AU_SFFR_Count = 987654321;
    return 0;
} /* SFFileReader_ */

/* Init/termination ======================================================= */

/** Initialize AU.  Must be called before any other functions.
 * @return TRUE on success; FALSE on failure.
 */
BOOL Au_Startup()
{
    if(AuInitialized_) return TRUE;     /* idempotent */

    /* Initialize PortAudio */
    PaError err = Pa_Initialize();
    if( err != paNoError ) return FALSE;
        /* TODO figure out error reporting - Pa_GetErrorText(err) */

    AuInitialized_ = TRUE;
    return TRUE;
} /* Au_Startup */

/** Shutdown AU.  Call this after closing any outputs you have open.
 * @return TRUE on success; FALSE on failure
 */
BOOL Au_Shutdown()
{
    if(!AuInitialized_) return TRUE;    /* idempotent */

    /* Shutdown PortAudio */
    PaError err = Pa_Terminate();
    if( err != paNoError ) return FALSE;
        /* TODO figure out error reporting - Pa_GetErrorText(err) */

    AuInitialized_ = FALSE;
    return TRUE;
} /* Au_Shutdown */

/** Create a new output.
 * @param handle {HAU} The output to shut down
 * @return non-NULL on success; NULL on failure
 */
HAU Au_New(Au_SampleFormat format, int sample_rate, int channels,
        void *user_data)
{
    PAU pau;
    PaError pa_err;
    PaSampleFormat pa_format;

    (void)user_data;    /* not yet used */

    if(!AuInitialized_) return FALSE;
    /* Map the format, since we don't directly expose the implementation
     * types to the caller.*/
    switch(format) {
        case AUSF_F32: pa_format = paFloat32; break;
        case AUSF_I32: pa_format = paInt32; break;
        case AUSF_I24: pa_format = paInt24; break;
        case AUSF_I16: pa_format = paInt16; break;
        case AUSF_I8: pa_format = paInt8; break;
        case AUSF_UI8: pa_format = paUInt8; break;
        case AUSF_CUSTOM: return NULL;  /* pa_format = paCustomFormat; */
                            /* TODO figure this out */
                          break;
        default: return NULL;
    }

    if(sample_rate < 1.0) return NULL;

    do {    /* init with rollback */
        pau = (PAU)malloc(sizeof(Au_Output));
        if(!pau) break;
        memset(pau, 0, sizeof(Au_Output));

        pau->format = format;
        pau->sample_rate = sample_rate;
        pau->channels = channels;

        /* PortAudio init */

        pau->pa_callback = PAEmptyCallback_;
        pau->pa_callback_userdata = NULL;

        pa_err = Pa_OpenDefaultStream(
            &pau->pa_stream,
            0,              /* no input channels */
            channels,
            pa_format,
            sample_rate,
            PA_BUFFER_FRAMECOUNT,
                /* frames per buffer, i.e. the number of sample frames
                 * that PortAudio will request from the callback. Many
                 * apps may want to use paFramesPerBufferUnspecified,
                 * which tells PortAudio to pick the best, possibly
                 * changing, buffer size.*/
            PACallback_,    /* dispatches to pau->pa_callback */
            pau );      /*This is a pointer that will be passed to
                            your callback*/
        if(pa_err != paNoError) break;

        return (HAU)pau;    /* Success exit */
    } while(0);

    /* If we got here, rollback whatever was done. */
    Au_Delete((HAU)pau);

    return NULL;
} /* Au_New */

/** Close an output.  If this succeeds, any memory associated witht that
 * output has been freed.
 * @param handle {HAU} The output to shut down
 * @return TRUE on success; FALSE on failure
 */
BOOL Au_Delete(HAU handle)
{
    POW

    if(pau->pa_stream) Pa_StopStream(pau->pa_stream);      /* just in case */

    /* TODO shutdown the reader thread */

    if(pau->sf_fd) {                    /* close the reader file */
        sf_close(pau->sf_fd);
        pau->sf_fd = NULL;
    }

    if(pau->pa_stream) {                /* close the output stream */
        Pa_CloseStream(pau->pa_stream);
        pau->pa_stream = NULL;
    }

    free(pau);
    return TRUE;
}

/* Playback from a file =================================================== */

/** Get the sample rate and format from a file.
 * @return TRUE on success; FALSE on failure. */
BOOL Au_InspectFile(const char *filename, int *samplerate, int *channels,
        Au_SampleFormat *format, long int *len)
{
    SF_INFO sf_info;
    SNDFILE *sf_fd;
    sf_count_t framecount;

    memset(&sf_info, 0, sizeof(sf_info));
    sf_fd = sf_open(filename, SFM_READ, &sf_info);
    if(!sf_fd) return FALSE;

    *samplerate = sf_info.samplerate;
    *channels = sf_info.channels;
    switch(sf_info.format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8: *format = AUSF_I8; break;
        case SF_FORMAT_PCM_U8: *format = AUSF_UI8; break;
        case SF_FORMAT_PCM_16: *format = AUSF_I16; break;
        case SF_FORMAT_PCM_24: *format = AUSF_I24; break;

        case SF_FORMAT_PCM_32: *format = AUSF_I32; break;
        case SF_FORMAT_FLOAT:
        case SF_FORMAT_VORBIS:  /* sf src/ogg_vorbis.c uses float inside */
            *format = AUSF_F32; break;
        default: *format = AUSF_CUSTOM; break;
    }

    if( (len != NULL) && sf_info.seekable ) {
        framecount = sf_seek(sf_fd, 0, SEEK_END);
        *len = (long int)framecount;
    }

    sf_close(sf_fd);
    return TRUE;
} /* Au_InspectFile */

unsigned int AU_PAPC_Count = 0;     /* For debugging */

/** PortAudio callback to play data received from a file. */
static int PAPlayCallback_(const void *input, void *output,
    unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void *handle )
{
#define MARK_NOT_PLAYING \
    do { if(0 == pthread_mutex_lock(pau->playback_time_mutex)) { \
        pau->is_playing = FALSE; \
        pthread_mutex_unlock(pau->playback_time_mutex); \
    } } while(0)

    void *data1, *data2;
    ring_buffer_size_t read_avail, elems1, elems2, ok;
    POW_UD_FAST

    ++AU_PAPC_Count;

    /* Let the reader get working on more data */
    sem_post(pau->sf_reader_semaphore);

    if(frameCount != PA_BUFFER_FRAMECOUNT) {    /* shouldn't happen, right? */
        MARK_NOT_PLAYING;
        return paComplete; /* for now */
    }

    /* Get the next block of info, if any */
    read_avail = PaUtil_GetRingBufferReadAvailable(pau->sf_buffer);
    if(read_avail <= 0) {
        MARK_NOT_PLAYING;
        return paComplete;  /* for now */
    }
    ok = PaUtil_GetRingBufferReadRegions(pau->sf_buffer, 1,
                    &data1, &elems1, &data2, &elems2);
    if(ok <= 0 || elems1 <= 0) {
        MARK_NOT_PLAYING;
        return paComplete;
    }

    PFRBuf pfr = (PFRBuf)data1;
    PPPS state = pfr->state;

    /* Initialize the sync information if necessary. */
    if(pau->playback_start_time == -1.0) {
        /* block if necessary for this one, so we have a reliable
         * sync-start indication. */
        if(0 != pthread_mutex_lock(pau->playback_time_mutex)) {
            // Couldn't lock the mutex, so can't change pau->is_playing
            return paComplete;  /* for now */
        }
        pau->playback_start_time = pau->playback_time = 0;
        pau->is_playing = TRUE;
        pthread_mutex_unlock(pau->playback_time_mutex);
    }

    /* Update the sync information, if we can.  If we can't get the
     * mutex, the caller is already reading it, so is going to miss
     * this update anyway.  Therefore, don't bother updating it now.
     *
     * TODO is there a faster way than a mutex?
     */
    else if(0 == pthread_mutex_trylock(pau->playback_time_mutex)) {
        pau->playback_time = (PaTime)pfr->pos_frames/pau->sample_rate;
        pau->is_playing = TRUE;
        pthread_mutex_unlock(pau->playback_time_mutex);
    }

    /* Output the data */
    memcpy(output, (void *)pfr->data, bufferSizeBytes_(pau));

    /* Release the info block */
    pfr = NULL;     /* because it's invalid once we advance the read index */
    PaUtil_AdvanceRingBufferReadIndex(pau->sf_buffer, 1);

    if(state == PPPS_Stopped) {
        MARK_NOT_PLAYING;
        return paComplete;
    } else {
        return paContinue;
    }
#undef MARK_NOT_PLAYING
} /* PAPlayCallback_ */

/** Play audio file #filename on output #handle. */
BOOL Au_Play(HAU handle, const char *filename)
{
    POW
    if(!pau->pa_stream) return FALSE;

    if(pau->sf_reader_thread) return FALSE;
        /* For now --- TODO enqueue files */

    Pa_StopStream(pau->pa_stream);      /* just in case */

    do { /* once */

        /* sf_fd */
        SF_INFO sf_info;
        memset(&sf_info, 0, sizeof(sf_info));
        pau->sf_fd = sf_open(filename, SFM_READ, &sf_info);
        if(!pau->sf_fd) break;

        if( (sf_info.samplerate != (int)pau->sample_rate) ||    /* sanity check */
            (sf_info.channels != 2) ) {
            break;
        }

        /* Sync */
        pau->is_playing = FALSE;
        pau->playback_frames = 0;
        pau->playback_time = -1.0;
        pau->playback_start_time = -1.0;
            /* negative => the player callback will initialize it. */

        pau->playback_time_mutex = &pau->playback_time_mutex_storage;
        if(0 != pthread_mutex_init(pau->playback_time_mutex, NULL)) break;

        /* Ring buffer */
        //int bufbytes = bufferSizeBytes_(pau);
        //if(bufbytes == -1) break;

        if((pau->sf_buffer_data =
                    malloc(sizeof(FRBuf) * PA_RING_BUFFERCOUNT)) == NULL) break;
        pau->sf_buffer = &pau->sf_buffer_storage;
        if(-1 == PaUtil_InitializeRingBuffer(pau->sf_buffer,
                    sizeof(FRBuf), PA_RING_BUFFERCOUNT, pau->sf_buffer_data)) {
            break;
        }

        /* Threading */
        pau->sf_reader_semaphore = &pau->sf_reader_semaphore_storage;
        if(sem_init(pau->sf_reader_semaphore, 0, 1) == -1) {
            /* initial value 1, so the reader will start to load data */
            break;
        }
        pau->sf_reader_should_exit = FALSE;

        pau->sf_reader_thread = &pau->sf_reader_thread_storage;
        if(pthread_create(pau->sf_reader_thread, NULL,
                    SFFileReader_, pau) != 0) {
            break;
        }

        pau->pa_callback_userdata = NULL;   /* everything's in pau */
        pau->pa_callback = PAPlayCallback_;

        /* Fire away! */
        if(Pa_StartStream(pau->pa_stream) != paNoError) break;
        sched_yield();
        Pa_Sleep(0);
            /* hopefully this will let the initial sync in PAPlayCallback_
             * happen before the caller does anything.
             * No guarantees, though! */

        return TRUE;
    } while(0);

    /* Failure: roll back changes */
    Au_Stop((HAU)pau);
    return FALSE;
} /* Au_Play */

BOOL Au_IsPlaying(HAU handle)
{
    POW
    BOOL retval = FALSE;
    if(0 == pthread_mutex_lock(pau->playback_time_mutex)) {
        retval = pau->is_playing;
        pthread_mutex_unlock(pau->playback_time_mutex);
    }
    return retval;
} /* Au_IsPlaying */

double Au_GetTimeInPlayback(HAU handle)
{
    PaTime start, curr;
    POW_FAST
    if(!pau) {
        return -1.0;
    }
    if(!pau->playback_time_mutex) {
        return -2.0;
    }

    if(0 == pthread_mutex_lock(pau->playback_time_mutex)) {
        start = pau->playback_start_time;
        curr = pau->playback_time;
        pthread_mutex_unlock(pau->playback_time_mutex);
    } else {
        return -3.0;
    }

    return (double)(curr-start);
} /* Au_GetTimeInPlayback */

BOOL Au_Stop(HAU handle)
{
    POW

    if(pau->pa_stream) Pa_StopStream(pau->pa_stream);      /* just in case */

    /* TODO? protect the callback with a mutex?  If the stream is
     * stopped, we shouldn't need to.
     */
    pau->pa_callback = PAEmptyCallback_;
    pau->pa_callback_userdata = NULL;

    if(pau->sf_reader_thread) {
        /* Tell the thread to exit */
        pau->sf_reader_should_exit = TRUE;
        sem_post(pau->sf_reader_semaphore);

        /* Wait for the thread to exit */
        if(pthread_join(*pau->sf_reader_thread, NULL) != 0) {
            pthread_cancel(*pau->sf_reader_thread);
            /* TODO is there a better way? */
        }
        pau->sf_reader_thread = NULL;
    }

    if(pau->sf_reader_semaphore) {
        sem_destroy(pau->sf_reader_semaphore);
        pau->sf_reader_semaphore = NULL;
    }

    if(pau->sf_buffer) {
        PaUtil_FlushRingBuffer(pau->sf_buffer);
        pau->sf_buffer = NULL;
    }

    if(pau->sf_buffer_data) {
        free(pau->sf_buffer_data);
        pau->sf_buffer_data = NULL;
    }

    if(pau->playback_time_mutex) {
        pthread_mutex_destroy(pau->playback_time_mutex);
        pau->playback_time_mutex = NULL;
    }
    pau->playback_time = -1.0;
    pau->playback_start_time = -1.0;
    pau->is_playing = FALSE;

    if(pau->sf_fd) {
        sf_close(pau->sf_fd);
        pau->sf_fd = NULL;
    }

    return TRUE;
} /* Au_Stop */

/* Utility functions ====================================================== */
void Au_msleep(long ms)
{
    Pa_Sleep(ms);
}

/* High-level functions =================================================== */

/** Callback to make a sine wave.  Currently only supports paFloat32
 * format.
 */
unsigned long Sine_Most_Recent_frameCount = 0;

static int PA_HL_Sine_Callback_(const void *input, void *output,
    unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void *handle )
{
    POW_UD_FAST

    /* A debugging tool */
    Sine_Most_Recent_frameCount = frameCount;

    double freq_rad = *((double *)pud->data);
    float *out = (float*)output;
    unsigned int i;
    double d;
    PaTime t = timeInfo->outputBufferDacTime;   /* time for sample 0 */
    double time_step = 1.0/pau->sample_rate;

    UNUSED(input);
    UNUSED(statusFlags);

    for( i=0; i<frameCount; i++ )
    {
        d = sin(freq_rad * t);
        *out++ = d; /* left */
        *out++ = d; /* right */
        t += time_step;
    }
    return paContinue;
} /* PA_HL_Sine_Callback_ */

BOOL Au_HL_Sine(HAU handle, double freq_Hz, int secs)
{
    double freq_rad = 2.0 * M_PI * freq_Hz;
    PaError pa_err;
    void *old_userdata;
    PaStreamCallback *old_pacallback;

    POW
    if(pau->format != AUSF_F32) return FALSE;
    if(pau->channels != 2) return FALSE;

    Pa_StopStream(pau->pa_stream);      /* just in case */

    const PaStreamInfo *strinfo;
    if(!(strinfo=Pa_GetStreamInfo(pau->pa_stream))) return FALSE;
    /* TODO? pass the stream params to the callback */

    old_userdata = pau->pa_callback_userdata;
    pau->pa_callback_userdata = (void *)&freq_rad;
    old_pacallback = pau->pa_callback;
    pau->pa_callback = PA_HL_Sine_Callback_;

    pa_err = Pa_StartStream(pau->pa_stream);
    if(pa_err != paNoError) return FALSE;

    Pa_Sleep(secs*1000);

    Pa_StopStream(pau->pa_stream);

    pau->pa_callback_userdata = old_userdata;
    pau->pa_callback = old_pacallback;

    return TRUE;
}

/* vi: set ts=4 sts=4 sw=4 et ai tw=72: */
