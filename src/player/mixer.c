/* Extended Module Player
 * Copyright (C) 1997-2007 Claudio Matsuoka and Hipolito Carraro Jr
 *
 * $Id: mixer.c,v 1.23 2007-10-20 14:25:53 cmatsuoka Exp $
 *
 * This file is part of the Extended Module Player and is distributed
 * under the terms of the GNU General Public License. See doc/COPYING
 * for more information.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mixer.h"
#include "synth.h"

#define FLAG_ITPT	0x01
#define FLAG_16_BITS	0x02
#define FLAG_STEREO	0x04
#define FLAG_FILTER	0x08
#define FLAG_REVLOOP	0x10
#define FLAG_ACTIVE	0x20
#define FLAG_SYNTH	0x40
#define FIDX_FLAGMASK	(FLAG_ITPT | FLAG_16_BITS | FLAG_STEREO | FLAG_FILTER)

#define LIM_FT		 12
#define LIM8_HI		 127
#define LIM8_LO		-127
#define LIM12_HI	 4095
#define LIM12_LO	-4096
#define LIM16_HI	 32767
#define LIM16_LO	-32768

#define TURN_OFF	0
#define TURN_ON		1


static char** smix_buffer = NULL;	/* array of output buffers */
static int* smix_buf32b = NULL;		/* temp buffer for 32 bit samples */
static int smix_numvoc;			/* default softmixer voices number */
static int smix_numbuf;			/* number of buffer to output */
static int smix_mode;			/* mode = 0:OFF, 1:MONO, 2:STEREO */
static int smix_resol;			/* resolution output 0:8bit, 1:16bit */
static int smix_ticksize;
int **xmp_mix_buffer = &smix_buf32b;

static int smix_dtright;		/* anticlick control, right channel */
static int smix_dtleft;			/* anticlick control, left channel */

static int echo_msg;

void    smix_mn8norm      (struct voice_info *, int *, int, int, int, int);
void    smix_mn8itpt      (struct voice_info *, int *, int, int, int, int);
void    smix_mn16norm     (struct voice_info *, int *, int, int, int, int);
void    smix_mn16itpt     (struct voice_info *, int *, int, int, int, int);
void    smix_st8norm      (struct voice_info *, int *, int, int, int, int);
void    smix_st8itpt      (struct voice_info *, int *, int, int, int, int);
void    smix_st16norm     (struct voice_info *, int *, int, int, int, int);
void    smix_st16itpt	  (struct voice_info *, int *, int, int, int, int);

void    smix_mn8itpt_flt  (struct voice_info *, int *, int, int, int, int);
void    smix_mn16itpt_flt (struct voice_info *, int *, int, int, int, int);
void    smix_st8itpt_flt  (struct voice_info *, int *, int, int, int, int);
void    smix_st16itpt_flt (struct voice_info *, int *, int, int, int, int);

void    smix_synth	  (struct voice_info *, int *, int, int, int, int);

static void     out_su8norm     (char*, int*, int, int);
static void     out_su16norm    (int16*, int*, int, int);
static void     out_u8ulaw      (char*, int*, int, int);

/* Array index:
 *
 * bit 0: 0=non-interpolated, 1=interpolated
 * bit 1: 0=8 bit, 1=16 bit
 * bit 2: 0=mono, 1=stereo
 */

static void (*mix_fn[])() = {
    /* unfiltered */
    smix_mn8norm,
    smix_mn8itpt,
    smix_mn16norm,
    smix_mn16itpt,
    smix_st8norm,
    smix_st8itpt,
    smix_st16norm,
    smix_st16itpt,

    /* filtered */
    smix_mn8norm,
    smix_mn8itpt_flt,
    smix_mn16norm,
    smix_mn16itpt_flt,
    smix_st8norm,
    smix_st8itpt_flt,
    smix_st16norm,
    smix_st16itpt_flt
};

#define OUT_U8ULAW	0
#define OUT_SU8NORM	1
#define OUT_SU16NORM	2

static void (*out_fn[])() = {
    out_u8ulaw,
    out_su8norm,
    out_su16norm
};

/* Downmix 32bit samples to 8bit, signed or unsigned, mono or stereo output */
static void out_su8norm(char *dest, int *src, int num, int cod)
{
    int smp, lhi, llo, offs;

    offs = (cod & XMP_FMT_UNS) ? 0x80 : 0;

    lhi = LIM8_HI + offs;
    llo = LIM8_LO + offs;

    for (; num--; ++src, ++dest) {
	smp = *src >> (LIM_FT + 8);
	smp = smp > LIM8_HI ? lhi : smp < LIM8_LO ? llo : smp + offs;
	*dest = smp;
    }
}


/* Downmix 32bit samples to 16bit, signed or unsigned, mono or stereo output */
static void out_su16norm(int16 *dest, int *src, int num, int cod)
{
    int smp, lhi, llo, offs;

    offs = (cod & XMP_FMT_UNS) ? 0x8000 : 0;

    lhi = LIM16_HI + offs;
    llo = LIM16_LO + offs;

    for (; num--; ++src, ++dest) {
	smp = *src >> LIM_FT;
	smp = smp > LIM16_HI ? lhi : smp < LIM16_LO ? llo : smp + offs;
	*dest = smp;
    }
}


/* Downmix 32bit samples to 8bit, unsigned ulaw, mono or stereo output */
static void out_u8ulaw(char *dest, int *src, int num, int cod)
{
    int smp;

    for (; num--; ++src, ++dest) {
	smp = *src >> (LIM_FT + 4);
	smp = smp > LIM12_HI ? ulaw_encode(LIM12_HI) :
	      smp < LIM12_LO ? ulaw_encode(LIM12_LO) : ulaw_encode (smp);
	*dest = smp;
    }
}


/* Prepare the mixer for the next tick */
inline static void smix_resetvar(struct xmp_context *ctx)
{
    struct xmp_player_context *p = &ctx->p;
    struct xmp_mod_context *m = &p->m;
    struct xmp_options *o = &ctx->o;

    smix_ticksize = m->fetch & XMP_CTL_MEDBPM ?
	o->freq * m->rrate * 33 / p->xmp_bpm / 12500 :
    	o->freq * m->rrate / p->xmp_bpm / 100;

    if (smix_buf32b) {
	smix_dtright = smix_dtleft = TURN_OFF;
	memset(smix_buf32b, 0, smix_ticksize * smix_mode * sizeof (int));
    }
}


/* Hipolito's rampdown anticlick */
static void smix_rampdown(int voc, int32 *buf, int cnt)
{
    int smp_l, smp_r;
    int dec_l, dec_r;

    if (voc < 0) {
	smp_r = smix_dtright;
	smp_l = smix_dtleft;
    } else {
	smp_r = voice_array[voc].sright;
	smp_l = voice_array[voc].sleft;
	voice_array[voc].sright = voice_array[voc].sleft = 0;
    }

    if (!smp_l && !smp_r)
	return;

    if (!buf) {
	buf = smix_buf32b;
	cnt = 16; //smix_ticksize;
    }
    if (!cnt)
	return;

    dec_r = smp_r / cnt;
    dec_l = smp_l / cnt;

    while ((smp_r || smp_l) && cnt--) {
	if (dec_r > 0)
	    *(buf++) += smp_r > dec_r ? (smp_r -= dec_r) : (smp_r = 0);
	else
	    *(buf++) += smp_r < dec_r ? (smp_r -= dec_r) : (smp_r = 0);

	if (dec_l > 0)
	    *(buf++) += smp_l > dec_l ? (smp_l -= dec_l) : (smp_l = 0);
	else
	    *(buf++) += smp_l < dec_l ? (smp_l -= dec_l) : (smp_l = 0);
    }
}


/* Ok, it's messy, but it works :-) Hipolito */
static void smix_anticlick(int voc, int vol, int pan, int *buf, int cnt)
{
    int oldvol, newvol;
    struct voice_info *vi = &voice_array[voc];

    if (extern_drv)
	return;			/* Anticlick is useful for softmixer only */

    if (vi->vol) {
	oldvol = vi->vol * (0x80 - vi->pan);
	newvol = vol * (0x80 - pan);
	vi->sright -= vi->sright / oldvol * newvol;

	oldvol = vi->vol * (0x80 + vi->pan);
	newvol = vol * (0x80 + pan);
	vi->sleft -= vi->sleft / oldvol * newvol;
    }

    if (!buf) {
	smix_dtright += vi->sright;
	smix_dtleft += vi->sleft;
	vi->sright = vi->sleft = TURN_OFF;
    } else {
	smix_rampdown(voc, buf, cnt);
    }
}


/* Fill the output buffer calling one of the handlers. The buffer contains
 * sound for one tick (a PAL frame or 1/50s for standard vblank-timed mods)
 */
static int softmixer(struct xmp_context *ctx)
{
    struct xmp_driver_context *d = &ctx->d;
    struct voice_info* vi;
    struct patch_info* pi;
    int smp_cnt, tic_cnt, lps, lpe;
    int vol_l, vol_r, itp_inc, voc;
    int prv_l, prv_r;
    int synth = 1;
    int* buf_pos;

    if (!extern_drv)
	smix_rampdown (-1, NULL, 0);		/* Anti-click */

    for (voc = numvoc; voc--; ) {
	vi = &voice_array[voc];

	if (vi->chn < 0)
	    continue;

	if (vi->period < 1) {
	    drv_resetvoice(ctx, voc, 1);
	    continue;
	}

	buf_pos = smix_buf32b;
	vol_r = (vi->vol * (0x80 - vi->pan)) >> 4;
	vol_l = (vi->vol * (0x80 + vi->pan)) >> 4;

	if (vi->fidx & FLAG_SYNTH) {
	    if (synth) {
		smix_synth(vi, buf_pos, smix_ticksize, vol_l, vol_r,
						vi->fidx & FLAG_STEREO);
	        synth = 0;
	    }
	    continue;
	}

	itp_inc = ((long long)vi->pbase << SMIX_SHIFT) / vi->period;

	pi = d->patch_array[vi->smp];

	/* This is for bidirectional sample loops */
	if (vi->fidx & FLAG_REVLOOP)
	    itp_inc = -itp_inc;

	/* Sample loop processing. Offsets in samples, not bytes */
	if (vi->fidx & FLAG_16_BITS) {
	    lps = pi->loop_start >> 1;
	    lpe = pi->loop_end >> 1;
	} else {
	    lps = pi->loop_start;
	    lpe = pi->loop_end;
	}

	for (tic_cnt = smix_ticksize; tic_cnt; ) {
	    /* How many samples we can write before the loop break or
	     * sample end... */
	    smp_cnt = 1 + (((long long)(vi->end - vi->pos) << SMIX_SHIFT)
		- vi->itpt) / itp_inc;

	    if (itp_inc > 0) {
		if (vi->end < vi->pos)
		    smp_cnt = 0;
	    } else {
		if (vi->end > vi->pos)
		    smp_cnt = 0;
	    }

	    /* Ok, this shouldn't happen, but in 'Languede.mod'... */
	    if (smp_cnt < 0)
		smp_cnt = 0;
	    
	    /* ...inside the tick boundaries */
	    if (smp_cnt > tic_cnt)
		smp_cnt = tic_cnt;

	    if (vi->vol) {
		int mixer = vi->fidx & FIDX_FLAGMASK;

		/* Something for Hipolito's anticlick routine */
		prv_r = buf_pos[smix_mode * smp_cnt - 2];
		prv_l = buf_pos[smix_mode * smp_cnt - 1];

		/* "Beautiful Ones" apparently uses 0xfe as 'no filter' :\ */
		if (vi->cutoff >= 0xfe)
		    mixer &= ~FLAG_FILTER;

		/* Call the output handler */
		mix_fn[mixer](vi, buf_pos, smp_cnt, vol_l, vol_r, itp_inc);
		buf_pos += smix_mode * smp_cnt;

		/* More stuff for Hipolito's anticlick routine */
		vi->sright = buf_pos[-2] - prv_r;
		vi->sleft = buf_pos[-1] - prv_l;
	    }

	    vi->itpt += itp_inc * smp_cnt;
	    vi->pos += vi->itpt >> SMIX_SHIFT;
	    vi->itpt &= SMIX_MASK;

	    /* No more samples in this tick */
	    if (!(tic_cnt -= smp_cnt))
		continue;

	    vi->fidx ^= vi->fxor;

	    /* Single sample loop run */
            if (vi->fidx == 0 || lps >= lpe) {
		smix_anticlick(voc, 0, 0, buf_pos, tic_cnt);
		drv_resetvoice(ctx, voc, 0);
		tic_cnt = 0;
		continue;
	    }

	    /* FIXME: sample size in bidirectional loops still not OK
	     *        test with jt_xmas.xm
	     */
	    if ((~vi->fidx & FLAG_REVLOOP) && vi->fxor == 0) {
		vi->pos -= lpe - lps;			/* forward loop */
	    } else {
		itp_inc = -itp_inc;			/* invert dir */
		vi->itpt += itp_inc;
		/* keep bidir loop at the same size of forward loop */
		vi->pos += (vi->itpt >> SMIX_SHIFT) + 1;
		vi->itpt &= SMIX_MASK;
		vi->end = itp_inc > 0 ? lpe : lps;
	    }
	}
    }

    return smix_ticksize * smix_mode * smix_resol;
}


static void smix_voicepos(struct xmp_context *ctx, int voc, int pos, int itp)
{
    struct xmp_driver_context *d = &ctx->d;
    struct voice_info *vi = &voice_array[voc];
    struct patch_info *pi = d->patch_array[vi->smp];
    int lpe, res, mode;

    if (pi->len == XMP_PATCH_FM)
	return;

    res = !!(pi->mode & WAVE_16_BITS);
    mode = (pi->mode & WAVE_LOOPING) && !(pi->mode & WAVE_BIDIR_LOOP);
    mode = (mode << res) + res + 1;	/* see xmp_cvt_anticlick */

    lpe = pi->len - mode;
    if (pi->mode & WAVE_LOOPING)
	lpe = lpe > pi->loop_end ? pi->loop_end : lpe;
    lpe >>= res;

    if (pos >= lpe)			/* Happens often in MED synth */
	pos = 0;

    vi->pos = pos;
    vi->itpt = itp;
    vi->end = lpe;

    if (vi->fidx & FLAG_REVLOOP)
	vi->fidx ^= vi->fxor;
}


static void smix_setpatch(struct xmp_context *ctx, int voc, int smp)
{
    struct xmp_player_context *p = &ctx->p;
    struct xmp_driver_context *d = &ctx->d;
    struct xmp_mod_context *m = &p->m;
    struct xmp_options *o = &ctx->o;
    struct voice_info *vi = &voice_array[voc];
    struct patch_info *pi = d->patch_array[smp];

    vi->smp = smp;
    vi->vol = 0;
    vi->freq = (long long) C4_FREQ * pi->base_freq / o->freq;
    
    if (pi->len == XMP_PATCH_FM) {
	vi->fidx = FLAG_SYNTH;
	if (o->outfmt & XMP_FMT_MONO)
	    vi->pan = TURN_OFF;
	else {
	    vi->pan = pi->panning;
	    vi->fidx |= FLAG_STEREO;
	}

	synth_setpatch(voc, (uint8 *)pi->data);

	return;
    }
    
    xmp_smix_setvol(voc, 0);

    vi->sptr = extern_drv ? NULL : pi->data;
    vi->fidx = m->fetch & XMP_CTL_ITPT ? FLAG_ITPT | FLAG_ACTIVE : FLAG_ACTIVE;

    if (o->outfmt & XMP_FMT_MONO) {
	vi->pan = TURN_OFF;
    } else {
	vi->pan = pi->panning;
	vi->fidx |= FLAG_STEREO;
    }

    if (pi->mode & WAVE_16_BITS)
	vi->fidx |= FLAG_16_BITS;

    if (m->fetch & XMP_CTL_FILTER)
	vi->fidx |= FLAG_FILTER;

    if (pi->mode & WAVE_LOOPING)
	vi->fxor = pi->mode & WAVE_BIDIR_LOOP ? FLAG_REVLOOP : 0;
    else
	vi->fxor = vi->fidx;

    smix_voicepos(ctx, voc, 0, 0);
}


static void smix_setnote(struct xmp_context *ctx, int voc, int note)
{
    struct xmp_driver_context *d = &ctx->d;
    struct voice_info *vi = &voice_array[voc];

    vi->period = note_to_period2 (vi->note = note, 0);
    vi->pbase = SMIX_C4NOTE * vi->freq / d->patch_array[vi->smp]->base_note;
}


static inline void smix_setbend(int voc, int bend)
{
    struct voice_info *vi = &voice_array[voc];

    vi->period = note_to_period2 (vi->note, bend);

    if (vi->fidx & FLAG_SYNTH)
	synth_setnote(voc, vi->note, bend);
}


void xmp_smix_setvol(int voc, int vol)
{
    struct voice_info *vi = &voice_array[voc];
 
    smix_anticlick(voc, vol, vi->pan, NULL, 0);
    vi->vol = vol;
}


void xmp_smix_seteffect(int voc, int type, int val)
{
    switch (type) {
    case XMP_FX_CUTOFF:
        voice_array[voc].cutoff = val;
	break;
    case XMP_FX_RESONANCE:
        voice_array[voc].resonance = val;
	break;
    case XMP_FX_FILTER_B0:
        voice_array[voc].flt_B0 = val;
	break;
    case XMP_FX_FILTER_B1:
        voice_array[voc].flt_B1 = val;
	break;
    case XMP_FX_FILTER_B2:
        voice_array[voc].flt_B2 = val;
	break;
    }
}


void xmp_smix_setpan(int voc, int pan)
{
    voice_array[voc].pan = pan;
}


void xmp_smix_echoback(int msg)
{
    xmp_event_callback(echo_msg = msg);
}


int xmp_smix_getmsg()
{
    return echo_msg;
}


int xmp_smix_numvoices(int num)
{
    return num > smix_numvoc ? smix_numvoc : num;
}

/* WARNING! Output samples must have the same byte order of the host machine!
 * (That's what happens in most cases anyway)
 */
int xmp_smix_writepatch(struct xmp_context *ctx, struct patch_info *patch)
{
    if (patch) {
	if (patch->len == XMP_PATCH_FM)
	    return XMP_OK;

	if (patch->len <= 0)
	    return XMP_ERR_PATCH;

	if (patch->mode & WAVE_UNSIGNED)
	    xmp_cvt_sig2uns (patch->len, patch->mode & WAVE_16_BITS,
		patch->data);
    }

    return XMP_OK;
}


int xmp_smix_on(struct xmp_context *ctx)
{
    struct xmp_driver_context *d = &ctx->d;
    int cnt;

    if (smix_numbuf)
	return XMP_OK;

    if (d->numbuf < 1)
	d->numbuf = 1;
    cnt = smix_numbuf = d->numbuf;

    smix_buffer = calloc(sizeof (void *), cnt);
    smix_buf32b = calloc(sizeof (int), OUT_MAXLEN);
    if (!(smix_buffer && smix_buf32b))
	return XMP_ERR_ALLOC;

    while (cnt--) {
	if (!(smix_buffer[cnt] = calloc(SMIX_RESMAX, OUT_MAXLEN)))
	    return XMP_ERR_ALLOC;
    }

    smix_numvoc = SMIX_NUMVOC;
    extern_drv = TURN_OFF;

    //synth_init (o->freq);

    return XMP_OK;
}


void xmp_smix_off()
{
    while (smix_numbuf)
	free(smix_buffer[--smix_numbuf]);

    //synth_deinit();

    free(smix_buf32b);
    free(smix_buffer);
    smix_buf32b = NULL;
    smix_buffer = NULL;
    extern_drv = TURN_ON;
}


void *xmp_smix_buffer(struct xmp_context *ctx)
{
    static int outbuf;
    int act;
    struct xmp_options *o = &ctx->o;

    if (!o->resol)
	act = OUT_U8ULAW;
    else if (o->resol > 8)
	act = OUT_SU16NORM;
    else
	act = OUT_SU8NORM;

    /* The mixer works with multiple buffers -- this is useless when using
     * multi-buffered sound output (e.g. OSS fragments) but can be useful for
     * DMA transfers in DOS.
     */
    if (++outbuf >= smix_numbuf)
	outbuf = 0;

    out_fn[act](smix_buffer[outbuf], smix_buf32b, smix_mode * smix_ticksize,
							o->outfmt);

    smix_resetvar(ctx);

    return smix_buffer[outbuf]; 
}

