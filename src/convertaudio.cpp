// Avisynth v2.5.  Copyright 2002 Ben Rudiak-Gould et al.
// http://www.avisynth.org

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .
//
// Linking Avisynth statically or dynamically with other modules is making a
// combined work based on Avisynth.  Thus, the terms and conditions of the GNU
// General Public License cover the whole combination.
//
// As a special exception, the copyright holders of Avisynth give you
// permission to link Avisynth with independent modules that communicate with
// Avisynth solely through the interfaces defined in avisynth.h, regardless of the license
// terms of these independent modules, and to copy and distribute the
// resulting combined work under terms of your choice, provided that
// every copy of the combined work is accompanied by a complete copy of
// the source code of Avisynth (the version of Avisynth used to produce the
// combined work), being distributed under the terms of the GNU General
// Public License plus this exception.  An independent module is a module
// which is not derived from or based on Avisynth, such as 3rd-party filters,
// import and export plugins, or graphical user interfaces.

// ConvertAudio classes
// Copyright (c) Klaus Post 2001 - 2004
// Copyright (c) Ian Brabham 2005

#include <malloc.h>

#include "avisynth.h"
#include "convertaudio.h"

// There are two type parameters. Acceptable sample types and a prefered sample type.
// If the current clip is already one of the defined types in sampletype, this will be returned.
// If not, the current clip will be converted to the prefered type.
PClip ConvertAudio::Create(PClip clip, int sample_type, int prefered_type) 
{
  if ((!clip->GetVideoInfo().HasAudio()) || clip->GetVideoInfo().SampleType()&(sample_type|prefered_type)) {  // Sample type is already ok!
    return clip;
  }
  else 
    return new ConvertAudio(clip,prefered_type);
}


int __stdcall ConvertAudio::SetCacheHints(int cachehints,int frame_range)
{   // We do pass cache requests upwards, to the next filter.
  return child->SetCacheHints(cachehints, frame_range);
}


/*******************************************
 *******   Convert Audio -> Arbitrary ******
 ******************************************/

// Optme: Could be made onepass, but that would make it immensely complex
ConvertAudio::ConvertAudio(PClip _clip, int _sample_type) 
  : GenericVideoFilter(_clip)
{
  dst_format=_sample_type;
  src_format=vi.SampleType();
  // Set up convertion matrix
  src_bps=vi.BytesPerChannelSample();  // Store old size
  vi.sample_type=dst_format;
  tempbuffer_size=0;
  floatbuffer_size=0;
}

ConvertAudio::~ConvertAudio() {
  if (tempbuffer_size) {
    _aligned_free(tempbuffer); 
    tempbuffer_size=0;
  }
  if (floatbuffer_size) {
    _aligned_free(floatbuffer); 
    floatbuffer_size=0;
  }
}

/*******************************************/

void convert24To16(char* inbuf, void* outbuf, int count) {
    unsigned char*  in  = (unsigned char*)inbuf;
    unsigned short* out = (unsigned short*)outbuf;

    for (int i=0;i<count;i++)
      out[i] = in[i*3+1] | (in[i*3+2] << 8); 
}

/*******************************************/

void convert16To8(char* inbuf, void* outbuf, int count) {
    signed short*  in  = (signed short*)inbuf;
    unsigned char* out = (unsigned char*)outbuf;

    for (int i=0;i<count;i++) 
      out[i] = (in[i] >> 8) + 128;
}

/*******************************************/

void convert8To16(char* inbuf, void* outbuf, int count) {
    unsigned char* in  = (unsigned char*)inbuf;
    signed short*  out = (signed short*)outbuf;

    // 8 Bit data is stored += 128

    // signed 16 bit data is composed of signed 8 bit data
    // times 256 + (signed 8 bit data + 128)

    // This make 0x7f(255-128) -> 0x7fff & 0x80(0-128) -> 0x8000

    for (int i=0;i<count;i++)
      out[i] = ((in[i]-128) << 8) | in[i];
}

/*******************************************/

void __stdcall ConvertAudio::GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env) 
{
  int channels=vi.AudioChannels();

  if (tempbuffer_size<count) {
    if (tempbuffer_size) _aligned_free(tempbuffer);
    tempbuffer = (char *) _aligned_malloc((int)count*src_bps*channels, 16);
    tempbuffer_size=(int)count;
  }

  child->GetAudio(tempbuffer, start, count, env);

  // Special fast cases
  if (src_format == SAMPLE_INT24 && dst_format == SAMPLE_INT16) {
      convert24To16(tempbuffer, buf, (int)count*channels);

	return;
  }
  if (src_format == SAMPLE_INT8 && dst_format == SAMPLE_INT16) {
      convert8To16(tempbuffer, buf, (int)count*channels);

	return;
  }
  if (src_format == SAMPLE_INT16 && dst_format == SAMPLE_INT8) {
      convert16To8(tempbuffer, buf, (int)count*channels);
	return;
  }

  float* tmp_fb;
  if (dst_format == SAMPLE_FLOAT)  // Skip final copy, if samples are to be float
	tmp_fb = (float*)buf;
  else {
    if (floatbuffer_size < count) {
      if (floatbuffer_size) _aligned_free(floatbuffer);
      floatbuffer = (SFLOAT*)_aligned_malloc((int)count*channels*sizeof(SFLOAT),16);
      floatbuffer_size=(int)count;
    }
	tmp_fb = floatbuffer;
  }

  if (src_format != SAMPLE_FLOAT) {  // Skip initial copy, if samples are already float
	//if ((((*(int*)tmp_fb) & 3) == 0) && (env->GetCPUFlags() & CPUF_SSE2)) {
      //convertToFloat_SSE2(tempbuffer, tmp_fb, src_format, (int)count*channels);
    //} else if ((env->GetCPUFlags() & CPUF_SSE)) {
      //convertToFloat_SSE(tempbuffer, tmp_fb, src_format, (int)count*channels);
    //} else {
      convertToFloat(tempbuffer, tmp_fb, src_format, (int)count*channels);
    //}
  } else {
    tmp_fb = (float*)tempbuffer;
  }

  if (dst_format != SAMPLE_FLOAT) {  // Skip final copy, if samples are to be float
	//if ((env->GetCPUFlags() & CPUF_SSE2)) {
	  //convertFromFloat_SSE2(tmp_fb, buf, dst_format, (int)count*channels);
	//} else if ((env->GetCPUFlags() & CPUF_SSE)) {
	  //convertFromFloat_SSE(tmp_fb, buf, dst_format, (int)count*channels);
	//} else {
	  convertFromFloat(tmp_fb, buf, dst_format, (int)count*channels);
	//}
  }
}

/* SAMPLE_INT16 <-> SAMPLE_INT32

 * S32 = (S16 << 16) + (unsigned short)(S16 + 32768)
 *
 *       0x7fff -> 0x7fffffff
 *       0x8000 -> 0x80000000
 *
 * short *d=dest, *s=src;
 *
 *   *d++ = *s + 32768;
 *   *d++ = *s++;
 *
 * for (i=0; i< count*ch; i++) {
 *   d[i*2]   = s[i] + 32768;
 *   d[i*2+1] = s[i];
 * }

 *   movq       mm7,[=32768]
 
 *   movq       mm0,[s]
 *    movq      mm1,mm7
 *   movq       mm2,mm7
 *    paddw     mm1,mm0
 *   paddw      mm2,mm0
 *    punpcklwd mm1,mm0
 *   d+=16
 *    punpckhwd mm2,mm0
 *   movq       [d-16],mm1
 *    s+=8
 *   movq       [d-8],mm2
 *   
 
 * S16 = (S32 + 0x8000) >> 16
 *
 * short *d=dest;
 * int   *s=src;
 * 
 * for (i=0; i< count*ch; i++) {
 *   d[i] = (s[i]+0x00008000) >> 16;
 * }

 *   movq    mm7,[=0x0000800000008000]
 * 
 *   movq     mm0,[s]
 *    movq    mm1,[s+8]
 *   paddd    mm0,mm7
 *    paddd   mm1,mm7
 *   psrad    mm0,16
 *    psrad   mm1,16
 *   d+=8
 *    packsdw mm0,mm1
 *   s+=16
 *    movq    [d-8],mm0

*/

//================
// convertToFloat
//================

void ConvertAudio::convertToFloat(char* inbuf, float* outbuf, char sample_type, int count) {
  int i;
  switch (sample_type) {
    case SAMPLE_INT8: {
      const float divisor = float(1.0 / 128);
      unsigned char* samples = (unsigned char*)inbuf;
      for (i=0;i<count;i++) 
        outbuf[i]=(samples[i]-128) * divisor;
      break;
      }
    case SAMPLE_INT16: {
      const float divisor = float(1.0 / 32768);
      signed short* samples = (signed short*)inbuf;
      for (i=0;i<count;i++) 
        outbuf[i]=samples[i] * divisor;
      break;
      }

    case SAMPLE_INT32: {
      const float divisor = float(1.0 / (unsigned)(1<<31));
      signed int* samples = (signed int*)inbuf;
      for (i=0;i<count;i++) 
        outbuf[i]=samples[i] * divisor;
      break;     
    }
    case SAMPLE_FLOAT: {
      SFLOAT* samples = (SFLOAT*)inbuf;
      for (i=0;i<count;i++) 
        outbuf[i]=samples[i];
      break;     
    }
    case SAMPLE_INT24: {
      const float divisor = float(1.0 / (unsigned)(1<<31));
      unsigned char* samples = (unsigned char*)inbuf;
      for (i=0;i<count;i++) {
        signed int tval = (samples[i*3]<<8) | (samples[i*3+1] << 16) | (samples[i*3+2] << 24); 
        outbuf[i] = tval * divisor;
      }
      break;
    }
    default: { 
      for (i=0;i<count;i++) 
        outbuf[i]=0.0f;
      break;     
    }
  }
}


void ConvertAudio::convertFromFloat(float* inbuf,void* outbuf, char sample_type, int count) {
  int i;
  switch (sample_type) {
    case SAMPLE_INT8: {
      unsigned char* samples = (unsigned char*)outbuf;
      for (i=0;i<count;i++) 
        samples[i]=(unsigned char)Saturate_int8(inbuf[i] * 128.0f)+128;
      break;
      }
    case SAMPLE_INT16: {
      signed short* samples = (signed short*)outbuf;
      for (i=0;i<count;i++) {
        samples[i]=Saturate_int16(inbuf[i] * 32768.0f);
      }
      break;
      }

    case SAMPLE_INT32: {
      signed int* samples = (signed int*)outbuf;
      for (i=0;i<count;i++) 
        samples[i]= Saturate_int32(inbuf[i] * (float)((unsigned)(1<<31)));
      break;     
    }
    case SAMPLE_INT24: {
      unsigned char* samples = (unsigned char*)outbuf;
      for (i=0;i<count;i++) {
        signed int tval = Saturate_int24(inbuf[i] * (float)(1<<23));
        samples[i*3] = tval & 0xff;
        samples[i*3+1] = (tval & 0xff00)>>8;
        samples[i*3+2] = (tval & 0xff0000)>>16;
      }
      break;
    }
    case SAMPLE_FLOAT: {
      SFLOAT* samples = (SFLOAT*)outbuf;      
      for (i=0;i<count;i++) {
        samples[i]=inbuf[i];
      }
      break;     
    }
    default: { 
    }
  }
}

__inline int ConvertAudio::Saturate_int8(float n) {
    if (n <= -128.0f) return -128;
    if (n >=  127.0f) return  127;
    return (int)(n+0.5f);
}


__inline short ConvertAudio::Saturate_int16(float n) {
    if (n <= -32768.0f) return -32768;
    if (n >=  32767.0f) return  32767;
    return (short)(n+0.5f);
}

__inline int ConvertAudio::Saturate_int24(float n) {
    if (n <= (float)-(1<<23)   ) return -(1<<23);
    if (n >= (float)((1<<23)-1)) return ((1<<23)-1);
    return (int)(n+0.5f);
}

__inline int ConvertAudio::Saturate_int32(float n) {
    if (n <= -2147483648.0f) return 0x80000000;  
    if (n >=  2147483647.0f) return 0x7fffffff;
    return (int)(n+0.5f);
}


