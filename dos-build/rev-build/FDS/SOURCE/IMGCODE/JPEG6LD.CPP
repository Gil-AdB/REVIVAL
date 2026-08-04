/*
   FDS_JPEG6 -- LoadJPEG on top of IJG jpeg-6b (FDS/JPEG6), replacing the prebuilt 1998
   JPEGLIBD.LIB (IJG v5).

   Why: measured on 86Box, decoding the demo's 42 textures costs ~940 Mcy (~4s at 233MHz),
   ~280 cycles/pixel -- 2-3x what libjpeg normally runs on a Pentium. The v5 lib's LoadJPEG
   lives in its DJPEG.C member and drives decode through jinit_write_targa/put_pixel_rows,
   i.e. the djpeg TARGA FILE WRITER, so every pixel goes through the file-format writer into
   an intermediate buffer. It also opens the file itself, which is why setvbuf was never
   reachable on that path.

   This version:
     - reads the whole file in ONE fread and decodes from memory (no 4KB refill chunks),
     - hands jpeg_read_scanlines the destination rows directly (no Targa writer, no copy),
     - selects the IDCT at build time; v5 shipped ONLY the float IDCT (jidctflt), 6b also
       has the integer fast one (FDS_JPEG6_IDCT=ifast|islow|float, default ifast).

   Output contract is byte-for-byte what the old lib returned, so Load_Texture_Impl is
   unchanged: [0..1]=width, [2..3]=height (unsigned short), then W*H*3 bytes of R,G,B.
   Allocated with new[] because the caller does `delete Txd`.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#ifdef FDS_JPEG6

extern "C" {
#include "jpeglib.h"
#include "jerror.h"
}

extern FILE *LogFile;

/* ---- error manager: the default one calls exit(); we want a NULL return ---- */
struct fds_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

extern "C" void fds_jpeg_error_exit(j_common_ptr cinfo)
{
  struct fds_error_mgr *err = (struct fds_error_mgr *)cinfo->err;
  char buf[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo,buf);
  if (LogFile) { fprintf(LogFile,"JPEG6 ERROR: %s\n",buf); fflush(LogFile); }
  longjmp(err->setjmp_buffer,1);
}

extern "C" void fds_jpeg_emit_message(j_common_ptr cinfo,int msg_level)
{
  /* swallow warnings (corrupt-but-recoverable markers); the 1998 assets trip a few */
  (void)cinfo; (void)msg_level;
}

/* ---- source manager over a memory buffer (the whole file, read in one go) ---- */
struct fds_src_mgr {
  struct jpeg_source_mgr pub;
  JOCTET *eoi;          /* 2-byte synthetic EOI handed out if the stream runs short */
};

extern "C" void fds_init_source(j_decompress_ptr cinfo) { (void)cinfo; }

extern "C" boolean fds_fill_input_buffer(j_decompress_ptr cinfo)
{
  /* The whole file is already in the buffer, so getting here means it was truncated.
     Feed a synthetic EOI so the decoder terminates cleanly instead of reading past it. */
  struct fds_src_mgr *src = (struct fds_src_mgr *)cinfo->src;
  WARNMS(cinfo,JWRN_JPEG_EOF);
  src->eoi[0] = (JOCTET)0xFF;
  src->eoi[1] = (JOCTET)JPEG_EOI;
  src->pub.next_input_byte = src->eoi;
  src->pub.bytes_in_buffer = 2;
  return TRUE;
}

extern "C" void fds_skip_input_data(j_decompress_ptr cinfo,long num_bytes)
{
  struct jpeg_source_mgr *src = cinfo->src;
  if (num_bytes <= 0) return;
  if ((unsigned long)num_bytes > (unsigned long)src->bytes_in_buffer)
  {
    src->next_input_byte += src->bytes_in_buffer;
    src->bytes_in_buffer  = 0;
    (void)fds_fill_input_buffer(cinfo);
  }
  else
  {
    src->next_input_byte += num_bytes;
    src->bytes_in_buffer  -= num_bytes;
  }
}

extern "C" void fds_term_source(j_decompress_ptr cinfo) { (void)cinfo; }

/* ---------------------------------------------------------------------------- */

char *LoadJPEG(char *argv1,unsigned int SeekPos)
{
  FILE *F;
  long fsz;
  /* volatile: both are assigned AFTER setjmp, and a non-volatile local modified between
     setjmp and longjmp has an indeterminate value once the jump lands -- with -oxl+ these
     live in registers, so the error path would free a garbage pointer. */
  unsigned char * volatile fbuf = 0;
  char * volatile out = 0;
  struct jpeg_decompress_struct cinfo;
  struct fds_error_mgr jerr;
  struct fds_src_mgr src;
  JOCTET eoibuf[2];

  F = fopen(argv1,"rb");
  if (!F)
  {
    if (LogFile) { fprintf(LogFile,"JPEG6: cannot open %s\n",argv1); fflush(LogFile); }
    return 0;
  }
  fseek(F,0,SEEK_END);
  fsz = ftell(F);
  fseek(F,0,SEEK_SET);
  if (fsz <= (long)SeekPos) { fclose(F); return 0; }

  fbuf = new unsigned char[fsz];
  if (!fbuf) { fclose(F); return 0; }
  if (fread((unsigned char *)fbuf,1,fsz,F) != (size_t)fsz) { fclose(F); delete [] (unsigned char *)fbuf; return 0; }
  fclose(F);

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit   = fds_jpeg_error_exit;
  jerr.pub.emit_message = fds_jpeg_emit_message;
  if (setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&cinfo);
    if (fbuf) { delete [] (unsigned char *)fbuf; fbuf = 0; }
    if (out)  { delete [] (char *)out;           out  = 0; }
    return 0;
  }

  jpeg_create_decompress(&cinfo);

  src.pub.init_source       = fds_init_source;
  src.pub.fill_input_buffer = fds_fill_input_buffer;
  src.pub.skip_input_data   = fds_skip_input_data;
  src.pub.resync_to_restart = jpeg_resync_to_restart;
  src.pub.term_source       = fds_term_source;
  src.pub.next_input_byte   = (const JOCTET *)((unsigned char *)fbuf + SeekPos);
  src.pub.bytes_in_buffer   = (size_t)(fsz - SeekPos);
  src.eoi                   = eoibuf;
  cinfo.src = (struct jpeg_source_mgr *)&src;

  jpeg_read_header(&cinfo,TRUE);

  cinfo.out_color_space = JCS_RGB;
#if defined(FDS_JPEG6_ISLOW)
  cinfo.dct_method = JDCT_ISLOW;
#elif defined(FDS_JPEG6_FLOAT)
  cinfo.dct_method = JDCT_FLOAT;
#else
  cinfo.dct_method = JDCT_IFAST;      /* v5 had no integer IDCT at all */
#endif
  /* Upsampling is left at libjpeg's default (fancy ON) so the decoded image matches what
     the v5 lib produced -- this swap is meant to be a speed change, not a look change.
     FDS_JPEG6_FASTUP trades the triangle filter for pixel replication if that's wanted. */
#ifdef FDS_JPEG6_FASTUP
  cinfo.do_fancy_upsampling = FALSE;
#endif

  jpeg_start_decompress(&cinfo);

  if (cinfo.output_components != 3)
  {
    if (LogFile) { fprintf(LogFile,"JPEG6: %s has %d components, expected 3\n",
                           argv1,(int)cinfo.output_components); fflush(LogFile); }
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    delete [] (unsigned char *)fbuf;
    return 0;
  }

  out = new char[4 + (long)cinfo.output_width * cinfo.output_height * 3];
  if (!out)
  {
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    delete [] (unsigned char *)fbuf;
    return 0;
  }
  *(unsigned short *)((char *)out)   = (unsigned short)cinfo.output_width;
  *(unsigned short *)((char *)out+2) = (unsigned short)cinfo.output_height;

  {
    /* Decode straight into the destination: one row pointer per call, no writer module,
       no intermediate image. The old path wrote every pixel through put_pixel_rows first. */
    long stride = (long)cinfo.output_width * 3;
    while (cinfo.output_scanline < cinfo.output_height)
    {
      JSAMPROW row = (JSAMPROW)((char *)out + 4 + (long)cinfo.output_scanline * stride);
      jpeg_read_scanlines(&cinfo,&row,1);
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  delete [] (unsigned char *)fbuf;
  return (char *)out;
}

#endif /* FDS_JPEG6 */
