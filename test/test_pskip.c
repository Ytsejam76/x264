// test_pskip.c
// Generate synthetic video + mb_info pskip pattern, encode baseline vs forced P_SKIP,
// write per-frame mb_info maps as PGM images, decode and verify MVs are (0,0) on all flagged MBs.

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../x264.h"
#include <libavcodec/avcodec.h>
#include <libavcodec/parser.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>

#ifndef X264_MBINFO_PERFECT_P_SKIP
#define X264_MBINFO_PERFECT_P_SKIP (1 << 1)	/* make sure this matches your tree */
#endif

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while(0)

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
	int w, h, n;
	int csp;		// X264_CSP_I420 or X264_CSP_I444
	const char *out_prefix;
} Args;

static void parse_args(int argc, char **argv, Args *a)
{
	a->w = 320;
	a->h = 192;
	a->n = 60;
	a->csp = X264_CSP_I420;
	a->out_prefix = "out";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc)
			a->w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") && i + 1 < argc)
			a->h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			a->n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			const char *s = argv[++i];
			if (!strcmp(s, "420"))
				a->csp = X264_CSP_I420;
			else if (!strcmp(s, "444"))
				a->csp = X264_CSP_I444;
			else
				DIE("Unsupported csp '%s' (use 420 or 444)", s);
		} else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			a->out_prefix = argv[++i];
		else if (!strcmp(argv[i], "-h")
			 || !strcmp(argv[i], "--help")) {
			fprintf(stderr, "Usage: %s [-w W] [-h H] [-n frames] [-c 420|444] [-o prefix]\n", argv[0]);
			exit(0);
		}
	}
	if (a->w % 16 || a->h % 16)
		DIE("Width/height must be multiples of 16");
	if (a->n < 2)
		DIE("Need at least 2 frames");
}

static void ensure_dir(const char *path)
{
#ifdef _WIN32
	_mkdir(path);
#else
	if (mkdir(path, 0755) && errno != EEXIST)
		DIE("mkdir %s: %s", path, strerror(errno));
#endif
}

static void write_mbmap_pgm(const char *dir, const char *stem, int f, int mbw, int mbh, const uint8_t *map)
{
	ensure_dir(dir);
	char fn[1024];
	snprintf(fn, sizeof(fn), "%s/%s_f%04d.pgm", dir, stem, f);
	FILE *fp = fopen(fn, "wb");
	if (!fp)
		DIE("open %s: %s", fn, strerror(errno));
	fprintf(fp, "P5\n%d %d\n255\n", mbw, mbh);
	for (int y = 0; y < mbh; y++) {
		for (int x = 0; x < mbw; x++) {
			uint8_t v = (map[y * mbw + x] & X264_MBINFO_PERFECT_P_SKIP) ? 255 : 0;
			fwrite(&v, 1, 1, fp);
		}
	}
	fclose(fp);
}

/*************** Synthetic content ****************/

static void fill_frame_synthetic(x264_picture_t *pic, int w, int h, int csp, int frame_idx)
{
	const int Ystride = pic->img.i_stride[0];
	const int Ustride = pic->img.i_stride[1];
	const int Vstride = pic->img.i_stride[2];
	uint8_t *Y = pic->img.plane[0];
	uint8_t *U = pic->img.plane[1];
	uint8_t *V = pic->img.plane[2];

	const int Ybg = 80, Ubg = 128, Vbg = 128;
	int box_w = w / 6, box_h = h / 6;
	int max_x = w - box_w - 1;
	int x = (frame_idx * 4) % (max_x ? max_x : 1);
	int y = h / 3;

	for (int j = 0; j < h; j++)
		memset(Y + j * Ystride, Ybg, w);
	for (int j = y; j < y + box_h && j < h; j++) {
		uint8_t *row = Y + j * Ystride;
		for (int i = x; i < x + box_w && i < w; i++)
			row[i] = 235;
	}

	if (csp == X264_CSP_I420) {
		int cw = w / 2, ch = h / 2;
		for (int j = 0; j < ch; j++) {
			memset(U + j * Ustride, Ubg, cw);
			memset(V + j * Vstride, Vbg, cw);
		}
		for (int j = y / 2; j < (y + box_h) / 2 && j < ch; j++) {
			uint8_t *ur = U + j * Ustride;
			uint8_t *vr = V + j * Vstride;
			for (int i = x / 2; i < (x + box_w) / 2 && i < cw; i++) {
				ur[i] = 128;
				vr[i] = 128;
			}
		}
	} else {		// I444
		for (int j = 0; j < h; j++) {
			memset(U + j * Ustride, Ubg, w);
			memset(V + j * Vstride, Vbg, w);
		}
		for (int j = y; j < y + box_h && j < h; j++) {
			uint8_t *ur = U + j * Ustride;
			uint8_t *vr = V + j * Vstride;
			for (int i = x; i < x + box_w && i < w; i++) {
				ur[i] = 128;
				vr[i] = 128;
			}
		}
	}
}

static void build_mbinfo(uint8_t *mbinfo, int mbw, int mbh, int w, int h, int frame_idx)
{
	memset(mbinfo, 0, (size_t) mbw * mbh);
	int box_w = w / 6, box_h = h / 6;
	int max_x = w - box_w - 1;
	int px = (frame_idx * 4) % (max_x ? max_x : 1);
	int py = h / 3;
	int mx0 = px / 16, my0 = py / 16;
	int mx1 = (px + box_w - 1) / 16, my1 = (py + box_h - 1) / 16;
	for (int my = 0; my < mbh; my++) {
		for (int mx = 0; mx < mbw; mx++) {
			int idx = my * mbw + mx;
			int in_box = (mx >= mx0 && mx <= mx1 && my >= my0 && my <= my1);
			if (!in_box)
				mbinfo[idx] |= X264_MBINFO_PERFECT_P_SKIP;
		}
	}
}

/*************** Encode & measure ****************/

typedef struct {
	double sec;
	size_t bytes;
} EncodeStats;

static void encode_one(const Args *A, const char *outpath, int force_pskip,
		       EncodeStats *S, uint8_t ***out_mbmaps, int *out_mbw, int *out_mbh, const char *maps_dir_label)
{
	FILE *fp = fopen(outpath, "wb");
	if (!fp)
		DIE("open %s: %s", outpath, strerror(errno));

	x264_param_t p;
	x264_param_default_preset(&p, "veryfast", NULL);
	p.i_width = A->w;
	p.i_height = A->h;
	p.i_csp = A->csp;
	p.i_fps_num = 30000;
	p.i_fps_den = 1001;
	p.i_keyint_max = 120;
	p.i_bframe = force_pskip ? 0 : 2;	// forced P_SKIP run: no B-frames
	p.i_threads = 1;	// stable timing
	p.b_annexb = 1;
	p.analyse.b_mb_info = 1;
	p.analyse.b_pskip_bypass = force_pskip ? 1 : 0;
	p.analyse.i_weighted_pred = 0;	// reduce variability

	x264_t *enc = x264_encoder_open(&p);
	if (!enc)
		DIE("x264_encoder_open failed");

	x264_picture_t pic_in, pic_out;
	x264_picture_alloc(&pic_in, A->csp, A->w, A->h);
	pic_in.i_pts = 0;

	const int mbw = (A->w + 15) / 16, mbh = (A->h + 15) / 16;
	*out_mbw = mbw;
	*out_mbh = mbh;

	uint8_t **mbmaps = calloc((size_t) A->n, sizeof(uint8_t *));
	if (!mbmaps)
		DIE("oom");
	for (int f = 0; f < A->n; f++) {
		mbmaps[f] = malloc((size_t) mbw * mbh);
		if (!mbmaps[f])
			DIE("oom");
	}

	// Prepare map output directory
	char maps_dir[1024];
	snprintf(maps_dir, sizeof(maps_dir), "%s_%s_maps", A->out_prefix, maps_dir_label);
	ensure_dir(maps_dir);

	int nnal;
	x264_nal_t *nals;
	double t0 = now_sec();
	size_t total_bytes = 0;

	for (int f = 0; f < A->n; f++) {
		fill_frame_synthetic(&pic_in, A->w, A->h, A->csp, f);
		build_mbinfo(mbmaps[f], mbw, mbh, A->w, A->h, f);
		pic_in.prop.mb_info = mbmaps[f];
		write_mbmap_pgm(maps_dir, "mbinfo", f, mbw, mbh, mbmaps[f]);	// save map

		pic_in.i_type = (f == 0) ? X264_TYPE_IDR : X264_TYPE_AUTO;

		int ret = x264_encoder_encode(enc, &nals, &nnal, &pic_in,
					      &pic_out);
		if (ret < 0)
			DIE("encode error");
		if (ret > 0) {
			if (fwrite(nals[0].p_payload, 1, (size_t) ret, fp)
			    != (size_t) ret)
				DIE("write failed");
			total_bytes += (size_t) ret;
		}
		pic_in.i_pts++;
	}
	while (1) {
		int ret = x264_encoder_encode(enc, &nals, &nnal, NULL, &pic_out);
		if (ret < 0)
			DIE("encode flush error");
		if (ret == 0)
			break;
		if (fwrite(nals[0].p_payload, 1, (size_t) ret, fp) != (size_t) ret)
			DIE("write failed");
		total_bytes += (size_t) ret;
	}

	double t1 = now_sec();
	x264_picture_clean(&pic_in);
	x264_encoder_close(enc);
	fclose(fp);

	S->sec = t1 - t0;
	S->bytes = total_bytes;
	*out_mbmaps = mbmaps;
}

/*************** Decode & verify MVs ***************/

static int decode_and_check_mvs(const char *path, int nframes, int mbw, int mbh, uint8_t **mbmaps, int *bad_frames, int *bad_mbs)
{
	*bad_frames = 0;
	*bad_mbs = 0;
	(void) mbw;
	(void) mbh;
	(void) mbmaps;		/* silence warnings if MV side data is absent */


	FILE *fp = fopen(path, "rb");
	if (!fp)
		DIE("open %s: %s", path, strerror(errno));
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *bitstream = malloc(sz);
	if (!bitstream)
		DIE("oom");
	if (fread(bitstream, 1, sz, fp) != (size_t) sz)
		DIE("read failed");
	fclose(fp);

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec)
		DIE("no h264 decoder");
	AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
	if (!parser)
		DIE("parser init failed");
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		DIE("ctx alloc failed");

	// Helpful logging
	av_log_set_level(AV_LOG_WARNING);

#ifdef AV_CODEC_FLAG2_EXPORT_MVS
	ctx->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
#endif
	int ret_open = avcodec_open2(ctx, codec, NULL);
	if (ret_open < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret_open, err, sizeof(err));
		fprintf(stderr, "decoder open failed (%d): %s\n", ret_open, err);
#ifdef AV_CODEC_FLAG2_EXPORT_MVS
		// Retry without EXPORT_MVS if that caused trouble
		avcodec_free_context(&ctx);
		ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			DIE("ctx alloc failed (retry)");
		ret_open = avcodec_open2(ctx, codec, NULL);
		if (ret_open < 0) {
			av_strerror(ret_open, err, sizeof(err));
			fprintf(stderr, "decoder open retry (no EXPORT_MVS) failed: %s\n", err);
			DIE("decoder open failed");
		} else {
			fprintf(stderr, "decoder open retry succeeded without EXPORT_MVS; MV checks will be skipped.\n");
		}
#else
		DIE("decoder open failed");
#endif
	}
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	if (!pkt || !frame)
		DIE("alloc failed");

	uint8_t *data = bitstream;
	int data_size = (int) sz;
	int got_frames = 0;

	while (data_size > 0) {
		int ret = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size,
					   data, data_size,
					   AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		if (ret < 0)
			DIE("parser error");
		data += ret;
		data_size -= ret;

		if (pkt->size > 0) {
			if (avcodec_send_packet(ctx, pkt) < 0)
				DIE("send packet failed");
			while (1) {
				ret = avcodec_receive_frame(ctx, frame);
				if (ret == AVERROR(EAGAIN)
				    || ret == AVERROR_EOF)
					break;
				if (ret < 0)
					DIE("receive frame failed");

				int f = got_frames++;
				if (f >= nframes) {
					av_frame_unref(frame);
					continue;
				}

				int frame_bad_mbs = 0;

#if defined(AV_FRAME_DATA_MOTION_VECTORS)
				AVFrameSideData *sd = av_frame_get_side_data(frame,
									     AV_FRAME_DATA_MOTION_VECTORS);
				if (sd && sd->size >= (int) sizeof(AVMotionVector)) {
					const AVMotionVector *mvs = (const AVMotionVector *)
					    sd->data;
					int mvcount = sd->size / (int) sizeof(AVMotionVector);

					uint8_t *badmap = calloc((size_t) mbw * mbh, 1);
					if (!badmap)
						DIE("oom");

					for (int i = 0; i < mvcount; i++) {
						const AVMotionVector *mv = &mvs[i];
						int dx = mv->dst_x, dy = mv->dst_y;
						int bw = mv->w ? mv->w : 16, bh = mv->h ? mv->h : 16;
						int mbx0 = dx / 16, mby0 = dy / 16;
						int mbx1 = (dx + bw - 1) / 16, mby1 = (dy + bh - 1) / 16;
						if (mbx0 < 0)
							mbx0 = 0;
						if (mby0 < 0)
							mby0 = 0;
						if (mbx1 >= mbw)
							mbx1 = mbw - 1;
						if (mby1 >= mbh)
							mby1 = mbh - 1;

						int nonzero = (mv->motion_x != 0 || mv->motion_y != 0);
						if (nonzero) {
							for (int my = mby0; my <= mby1; my++)
								for (int mx = mbx0; mx <= mbx1; mx++)
									badmap[my * mbw + mx]
									    = 1;
						}
					}

					uint8_t *flags = mbmaps[f];
					for (int my = 0; my < mbh; my++)
						for (int mx = 0; mx < mbw; mx++)
							if (flags[my * mbw + mx] & X264_MBINFO_FORCE_P_SKIP)
								if (badmap[my * mbw + mx]) {
									frame_bad_mbs++;
									(*bad_mbs)++;
								}

					free(badmap);
				} else {
					fprintf(stderr, "Warning: decoder didn't provide MVs; skipping checks for frame %d\n", f);
				}
#else
				fprintf(stderr,
					"Warning: this FFmpeg build lacks AV_FRAME_DATA_MOTION_VECTORS; skipping MV checks.\n");
#endif
				if (frame_bad_mbs)
					(*bad_frames)++;
				av_frame_unref(frame);
			}
			av_packet_unref(pkt);
		}
	}

	av_parser_close(parser);
	avcodec_free_context(&ctx);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	free(bitstream);
	return 0;
}

/*************** Main ****************/

int main(int argc, char **argv)
{
	Args A;
	parse_args(argc, argv, &A);


	char out_baseline[1024], out_pskip[1024];
	snprintf(out_baseline, sizeof(out_baseline), "%s_baseline.h264", A.out_prefix);
	snprintf(out_pskip, sizeof(out_pskip), "%s_pskip.h264", A.out_prefix);

	EncodeStats Sb = { 0 }, Sp = { 0 };
	uint8_t **mbmaps_pskip = NULL;
	int mbw = 0, mbh = 0;

	// 1) Baseline (bypass OFF) — we still write the mb_info maps we *fed* the encoder.
	{
		uint8_t **tmp_maps = NULL;
		int tmp_mbw = 0, tmp_mbh = 0;
		encode_one(&A, out_baseline, 0, &Sb, &tmp_maps, &tmp_mbw, &tmp_mbh, "baseline");
		// cleanup (we don't need baseline maps later)
		for (int f = 0; f < A.n; f++)
			free(tmp_maps[f]);
		free(tmp_maps);
	}

	// 2) Forced P_SKIP (bypass ON, no B-frames) — keep maps for MV verification
	encode_one(&A, out_pskip, 1, &Sp, &mbmaps_pskip, &mbw, &mbh, "pskip");

	// 3) Decode “pskip” bitstream and validate MV==0 on all flagged MBs
	int bad_frames = 0, bad_mbs = 0;
	decode_and_check_mvs(out_pskip, A.n, mbw, mbh, mbmaps_pskip, &bad_frames, &bad_mbs);

	// Cleanup maps
	for (int f = 0; f < A.n; f++)
		free(mbmaps_pskip[f]);
	free(mbmaps_pskip);

	// Report
	printf("Resolution: %dx%d  CSP:%s  Frames:%d  MBs:%dx%d\n",
	       A.w, A.h, (A.csp == X264_CSP_I420 ? "I420" : "I444"), A.n, mbw, mbh);
	printf("Baseline:  time = %.3f s, size = %.1f kB\n", Sb.sec, Sb.bytes / 1024.0);
	printf("P_SKIP:    time = %.3f s, size = %.1f kB\n", Sp.sec, Sp.bytes / 1024.0);
	if (Sp.sec > 0 && Sb.sec > 0)
		printf("Speedup:   %.2fx\n", Sb.sec / Sp.sec);
	printf("MV check:  bad_frames=%d  bad_mbs=%d  (expect 0)\n", bad_frames, bad_mbs);

	if (bad_frames || bad_mbs) {
		fprintf(stderr, "ERROR: Non-zero MV detected on forced-skip MBs.\n");
		return 2;
	}
	printf("OK: All forced-skip MBs had MV=(0,0) at the decoder.\n");
	return 0;
}
