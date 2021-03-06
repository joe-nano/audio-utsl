/* examples/check_file.c: Au_InspectFile example for audio-utsl.
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

#include <stdio.h>
#include "audio_utsl.h"

int main(int argc, char **argv)
{
    int samplerate, channels;
    Au_SampleFormat format;

    printf("Sizes: short %d int %d long %d float %d double %d\n",
            (int)sizeof(short), (int)sizeof(int), (int)sizeof(long),
            (int)sizeof(float), (int)sizeof(double));

    if(argc<2) return 1;
    if(!Au_Startup()) return 2;

    if(!Au_InspectFile(argv[1], &samplerate, &channels, &format, NULL))
        return 3;
    printf("File %s: %d channels @ %d Hz, format ", argv[1],
            channels, samplerate);
    switch(format) {
        case AUSF_F32: printf("float32\n"); break;
        case AUSF_I32: printf("int32\n"); break;
        case AUSF_I24: printf("int24\n"); break;
        case AUSF_I16: printf("int16\n"); break;
        case AUSF_I8: printf("int8\n"); break;
        case AUSF_UI8: printf("uint8\n"); break;
        default: printf("unknown\n"); break;
    }

    if(!Au_Shutdown()) return 4;

    return 0;
}

/* vi: set ts=4 sts=4 sw=4 et ai: */
