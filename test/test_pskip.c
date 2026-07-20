#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "../x264.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/motion_vector.h>
#include <libavutil/pixdesc.h>

#ifndef X264_MBINFO_PERFECT_P_SKIP
#define X264_MBINFO_PERFECT_P_SKIP (1u << 1)
#endif

#define FAIL(...) do { fprintf( stderr, __VA_ARGS__ ); fprintf( stderr, "\n" ); goto fail; } while( 0 )
#ifndef X264_MIN
#define X264_MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef X264_MAX
#define X264_MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

typedef struct
{
    int w;
    int h;
    int n;
    int csp;
    const char *scenario;
    const char *out_prefix;
    int bench;       /* encode-only timing, no file/decode I/O */
    int wrong_hint;  /* negative test: hint a changing MB as skippable */
} args_t;

typedef struct
{
    double sec;
    size_t bytes;
} encode_stats_t;

typedef struct
{
    int w;
    int h;
    int format;
    uint64_t hash;
} frame_hash_t;

static double now_sec( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void ensure_dir( const char *path )
{
#ifdef _WIN32
    _mkdir( path );
#else
    if( mkdir( path, 0755 ) && errno != EEXIST )
    {
        fprintf( stderr, "mkdir %s: %s\n", path, strerror( errno ) );
        exit( 1 );
    }
#endif
}

static void parse_args( int argc, char **argv, args_t *a )
{
    a->w = 320;
    a->h = 192;
    a->n = 60;
    a->csp = X264_CSP_I420;
    a->scenario = "all";
    a->out_prefix = "out";
    a->bench = 0;
    a->wrong_hint = 0;

    for( int i = 1; i < argc; i++ )
    {
        if( !strcmp( argv[i], "-w" ) && i + 1 < argc )
            a->w = atoi( argv[++i] );
        else if( !strcmp( argv[i], "-H" ) && i + 1 < argc )
            a->h = atoi( argv[++i] );
        else if( !strcmp( argv[i], "-n" ) && i + 1 < argc )
            a->n = atoi( argv[++i] );
        else if( !strcmp( argv[i], "-c" ) && i + 1 < argc )
        {
            const char *s = argv[++i];
            if( !strcmp( s, "420" ) )
                a->csp = X264_CSP_I420;
            else if( !strcmp( s, "444" ) )
                a->csp = X264_CSP_I444;
            else
            {
                fprintf( stderr, "Unsupported csp '%s' (use 420 or 444)\n", s );
                exit( 1 );
            }
        }
        else if( !strcmp( argv[i], "-o" ) && i + 1 < argc )
            a->out_prefix = argv[++i];
        else if( !strcmp( argv[i], "-s" ) && i + 1 < argc )
            a->scenario = argv[++i];
        else if( !strcmp( argv[i], "-B" ) )
            a->bench = 1;
        else if( !strcmp( argv[i], "-N" ) )
            a->wrong_hint = 1;
        else if( !strcmp( argv[i], "-h" ) || !strcmp( argv[i], "--help" ) )
        {
            fprintf( stderr, "Usage: %s [-w W] [-H H] [-n frames] [-c 420|444] [-s scenario] [-o prefix] [-B] [-N]\n", argv[0] );
            fprintf( stderr, "Scenarios: all, none, edge-boxes, interior-boxes, strips, chessboard,\n" );
            fprintf( stderr, "           block3, block4, bands, cols, solid-interior, isolated,\n" );
            fprintf( stderr, "           lshape, toprow, leftcol, corner, combo\n" );
            fprintf( stderr, "  -B  bench mode (encode-only, no file/decode I/O)\n" );
            fprintf( stderr, "  -N  negative test (hint a changing MB as skippable; proves the fast path engaged)\n" );
            exit( 0 );
        }
    }

    if( a->w % 16 || a->h % 16 )
    {
        fprintf( stderr, "Width/height must be multiples of 16\n" );
        exit( 1 );
    }
    if( a->n < 2 )
    {
        fprintf( stderr, "Need at least 2 frames\n" );
        exit( 1 );
    }
}

static int streq( const char *a, const char *b )
{
    return !strcmp( a, b );
}

static int in_rect( int x, int y, int x0, int y0, int x1, int y1 )
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

/* A macroblock is "static" (bit-identical to the previous frame, hence a
 * truthful PERFECT_P_SKIP candidate) if this returns nonzero. Scenarios are
 * chosen to exercise both the interior P_SKIP path (a hinted MB whose raster
 * left AND top neighbors are also hinted) and the boundary P_L0-mv0 path. */
static int mb_is_static( const char *scenario, int mb_x, int mb_y, int mbw, int mbh )
{
    if( streq( scenario, "all" ) )
        return 1;
    if( streq( scenario, "none" ) )
        return 0;
    if( streq( scenario, "edge-boxes" ) )
        return in_rect( mb_x, mb_y, 0, 0, X264_MIN( 5, mbw ), X264_MIN( 4, mbh ) ) ||
               in_rect( mb_x, mb_y, X264_MAX( mbw - 6, 0 ), X264_MAX( mbh - 5, 0 ), mbw, mbh ) ||
               in_rect( mb_x, mb_y, X264_MAX( mbw / 2 - 2, 0 ), 0, X264_MIN( mbw / 2 + 3, mbw ), X264_MIN( 3, mbh ) ) ||
               in_rect( mb_x, mb_y, 0, X264_MAX( mbh / 2 - 2, 0 ), X264_MIN( 3, mbw ), X264_MIN( mbh / 2 + 3, mbh ) );
    if( streq( scenario, "interior-boxes" ) )
        return in_rect( mb_x, mb_y, mbw / 5, mbh / 4, X264_MIN( mbw / 5 + 5, mbw ), X264_MIN( mbh / 4 + 4, mbh ) ) ||
               in_rect( mb_x, mb_y, mbw * 3 / 5, mbh / 3, X264_MIN( mbw * 3 / 5 + 6, mbw ), X264_MIN( mbh / 3 + 5, mbh ) );
    if( streq( scenario, "strips" ) )
        return mb_x == 0 || mb_y == 0 || mb_x == mbw / 2 || mb_y == mbh / 2 ||
               mb_x == mbw - 1 || mb_y == mbh - 1;
    if( streq( scenario, "chessboard" ) )
        return ((mb_x ^ mb_y) & 1) == 0;
    /* Rectangular tilings: block-chessboards produce genuine interior P_SKIP
     * MBs (unlike the per-MB chessboard, which is almost all boundary). */
    if( streq( scenario, "block3" ) )
        return (((mb_x / 3) ^ (mb_y / 3)) & 1) == 0;
    if( streq( scenario, "block4" ) )
        return (((mb_x / 4) ^ (mb_y / 4)) & 1) == 0;
    if( streq( scenario, "bands" ) )        /* wide static bands: stress top-seam classification */
        return (((mb_x / 6) ^ (mb_y / 1)) & 1) == 0;
    if( streq( scenario, "cols" ) )         /* tall static columns: stress left-seam classification */
        return (((mb_x / 1) ^ (mb_y / 6)) & 1) == 0;
    if( streq( scenario, "solid-interior" ) ) /* a solid interior block: pure interior P_SKIP core */
        return in_rect( mb_x, mb_y, X264_MIN( 5, mbw - 1 ), X264_MIN( 5, mbh - 1 ),
                        X264_MIN( 5 + X264_MAX( mbw / 3, 3 ), mbw ),
                        X264_MIN( 5 + X264_MAX( mbh / 3, 3 ), mbh ) );
    if( streq( scenario, "concentrated" ) ) /* one solid block covering ~half the frame */
        return in_rect( mb_x, mb_y, 0, 0, mbw, mbh / 2 );
    if( streq( scenario, "isolated" ) )     /* one interior hinted MB in moving content: boundary path */
        return mb_x == mbw / 2 && mb_y == mbh / 2;
    if( streq( scenario, "lshape" ) )       /* hinted left column + top row of an interior block */
        return (mb_x == mbw / 3 && mb_y >= mbh / 3 && mb_y < mbh * 2 / 3) ||
               (mb_y == mbh / 3 && mb_x >= mbw / 3 && mb_x < mbw * 2 / 3);
    if( streq( scenario, "toprow" ) )       /* only the top row (mb_y==0) hinted */
        return mb_y == 0;
    if( streq( scenario, "leftcol" ) )      /* only the left column (mb_x==0) hinted */
        return mb_x == 0;
    if( streq( scenario, "corner" ) )       /* only MB(0,0) hinted */
        return mb_x == 0 && mb_y == 0;
    if( streq( scenario, "combo" ) )
        return mb_is_static( "edge-boxes", mb_x, mb_y, mbw, mbh ) ||
               mb_is_static( "interior-boxes", mb_x, mb_y, mbw, mbh ) ||
               (((mb_x + 2 * mb_y) % 7) == 0);

    fprintf( stderr, "Unknown scenario '%s'\n", scenario );
    exit( 1 );
}

/* Frame-invariant pseudo-random texture: a pure function of (x, y), so static
 * content is bit-identical across frames (the PERFECT_P_SKIP hint stays
 * truthful and lossless-skip stays exact -> no false positives). Being a
 * high-entropy global field, motion-compensating from ANY nonzero displacement
 * yields different pixels, so a wrongly nonzero decoded MV corrupts the picture
 * and is caught -- unlike the old flat fill, which masked such errors. */
static uint8_t tex8( int x, int y, uint32_t salt )
{
    uint32_t h = (uint32_t)(x + 1) * 2654435761u;
    h ^= (uint32_t)(y + 1) * 2246822519u;
    h ^= salt;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return (uint8_t)h;
}

/* Resolve the per-MB static classification once (mbw*mbh string compares)
 * instead of calling mb_is_static (with its strcmp chain) for every pixel --
 * otherwise the harness's per-pixel strcmp dominates any encode profile. */
static uint8_t *build_static_map( const char *scenario, int mbw, int mbh )
{
    uint8_t *map = malloc( (size_t)mbw * mbh );
    if( !map ) { fprintf( stderr, "out of memory\n" ); exit( 1 ); }
    for( int my = 0; my < mbh; my++ )
        for( int mx = 0; mx < mbw; mx++ )
            map[my * mbw + mx] = (uint8_t)!!mb_is_static( scenario, mx, my, mbw, mbh );
    return map;
}

static void fill_frame_synthetic( x264_picture_t *pic, int w, int h, int csp, const char *scenario, int frame_idx )
{
    const int y_stride = pic->img.i_stride[0];
    const int u_stride = pic->img.i_stride[1];
    const int v_stride = pic->img.i_stride[2];
    uint8_t *Y = pic->img.plane[0];
    uint8_t *U = pic->img.plane[1];
    uint8_t *V = pic->img.plane[2];
    const int mbw = (w + 15) / 16;
    const int mbh = (h + 15) / 16;
    uint8_t *smap = build_static_map( scenario, mbw, mbh );

    for( int y = 0; y < h; y++ )
        for( int x = 0; x < w; x++ )
        {
            const int is_static = smap[(y / 16) * mbw + (x / 16)];
            Y[y * y_stride + x] = is_static ? tex8( x, y, 0x9E3779B1u )
                                            : (uint8_t)(16 + ((x * 3 + y * 5 + frame_idx * 17) & 223));
        }

    if( csp == X264_CSP_I420 )
    {
        for( int y = 0; y < h / 2; y++ )
            for( int x = 0; x < w / 2; x++ )
            {
                const int is_static = smap[(y / 8) * mbw + (x / 8)];
                U[y * u_stride + x] = is_static ? tex8( x, y, 0x85EBCA77u )
                                                : (uint8_t)(32 + ((x * 7 + y * 3 + frame_idx * 11) & 191));
                V[y * v_stride + x] = is_static ? tex8( x, y, 0xC2B2AE3Du )
                                                : (uint8_t)(224 - ((x * 5 + y * 9 + frame_idx * 13) & 191));
            }
    }
    else
    {
        for( int y = 0; y < h; y++ )
            for( int x = 0; x < w; x++ )
            {
                const int is_static = smap[(y / 16) * mbw + (x / 16)];
                U[y * u_stride + x] = is_static ? tex8( x, y, 0x85EBCA77u )
                                                : (uint8_t)(32 + ((x * 7 + y * 3 + frame_idx * 11) & 191));
                V[y * v_stride + x] = is_static ? tex8( x, y, 0xC2B2AE3Du )
                                                : (uint8_t)(224 - ((x * 5 + y * 9 + frame_idx * 13) & 191));
            }
    }

    free( smap );
}

static void build_mbinfo( uint8_t *mbinfo, int mbw, int mbh, const char *scenario, int frame_idx, int wrong_hint )
{
    (void)frame_idx;

    /* Negative test: deliberately lie -- mark EVERY MB as a perfect skip while
     * the content actually changes every frame (paired with scenario "none").
     * Hinting the whole frame makes every MB satisfy an interior rule, so all
     * become P_SKIP (cbp forced to 0, no residual). A correct feed of this hint
     * therefore copies the previous, different block, so the decode diverges
     * visibly from the source -- the proof that the fast path executed. */
    if( wrong_hint )
    {
        memset( mbinfo, X264_MBINFO_PERFECT_P_SKIP, (size_t)mbw * mbh );
        return;
    }

    for( int y = 0; y < mbh; y++ )
        for( int x = 0; x < mbw; x++ )
            mbinfo[y * mbw + x] = mb_is_static( scenario, x, y, mbw, mbh ) ? X264_MBINFO_PERFECT_P_SKIP : 0;
}

static void write_mbmap_pgm( const char *dir, const char *stem, int f, int mbw, int mbh, const uint8_t *map )
{
    ensure_dir( dir );

    char fn[2048];
    snprintf( fn, sizeof(fn), "%s/%s_f%04d.pgm", dir, stem, f );
    FILE *fp = fopen( fn, "wb" );
    if( !fp )
    {
        fprintf( stderr, "open %s: %s\n", fn, strerror( errno ) );
        exit( 1 );
    }

    fprintf( fp, "P5\n%d %d\n255\n", mbw, mbh );
    for( int y = 0; y < mbh; y++ )
        for( int x = 0; x < mbw; x++ )
        {
            uint8_t v = (map[y * mbw + x] & X264_MBINFO_PERFECT_P_SKIP) ? 255 : 0;
            fwrite( &v, 1, 1, fp );
        }
    fclose( fp );
}

static void write_frame_yuv( FILE *fp, const x264_picture_t *pic, int w, int h, int csp )
{
    const int cw = csp == X264_CSP_I420 ? w / 2 : w;
    const int ch = csp == X264_CSP_I420 ? h / 2 : h;

    for( int y = 0; y < h; y++ )
        fwrite( pic->img.plane[0] + y * pic->img.i_stride[0], 1, (size_t)w, fp );
    for( int y = 0; y < ch; y++ )
        fwrite( pic->img.plane[1] + y * pic->img.i_stride[1], 1, (size_t)cw, fp );
    for( int y = 0; y < ch; y++ )
        fwrite( pic->img.plane[2] + y * pic->img.i_stride[2], 1, (size_t)cw, fp );
}

static void write_source_sequence( const args_t *A, const char *outpath )
{
    FILE *fp = fopen( outpath, "wb" );
    if( !fp )
    {
        fprintf( stderr, "open %s: %s\n", outpath, strerror( errno ) );
        exit( 1 );
    }

    x264_picture_t pic;
    if( x264_picture_alloc( &pic, A->csp, A->w, A->h ) < 0 )
    {
        fprintf( stderr, "x264_picture_alloc failed\n" );
        exit( 1 );
    }

    for( int f = 0; f < A->n; f++ )
    {
        fill_frame_synthetic( &pic, A->w, A->h, A->csp, A->scenario, f );
        write_frame_yuv( fp, &pic, A->w, A->h, A->csp );
    }

    x264_picture_clean( &pic );
    fclose( fp );
}

/* Configure an encoder for the tests: veryfast, P-only or with B-frames,
 * lossless CQP (so a correct skip reconstructs the previous block exactly and
 * the decode is bit-comparable to the source). */
static void set_params( x264_param_t *p, const args_t *A, int force_pskip, int p_only )
{
    /* Bench mode targets ultrafast (CAVLC, no deblock, single-ref -- the
     * intended low-CPU operating point); the correctness suite stays on
     * veryfast for broader path coverage. PSKIP_PRESET overrides either. */
    const char *preset = getenv( "PSKIP_PRESET" );
    if( !preset )
        preset = A->bench ? "ultrafast" : "veryfast";
    x264_param_default_preset( p, preset, NULL );
    p->i_width = A->w;
    p->i_height = A->h;
    p->i_csp = A->csp;
    p->i_bitdepth = 8;
    p->i_fps_num = 30000;
    p->i_fps_den = 1001;
    p->i_keyint_max = 120;
    p->i_bframe = p_only ? 0 : 2;
    p->i_threads = 1;
    p->b_annexb = 1;
    p->analyse.b_mb_info = 1;
    p->analyse.b_mb_info_update = 1;
    p->analyse.b_pskip_bypass = force_pskip ? 1 : 0;
    p->analyse.i_weighted_pred = 0;
    p->rc.i_rc_method = X264_RC_CQP;
    p->rc.i_qp_constant = 0;
    p->rc.i_qp_min = 0;
    p->rc.i_qp_max = 51;
    p->rc.i_lookahead = 0;
    p->rc.b_mb_tree = 0;
}

static void encode_one( const args_t *A, const char *outpath, int force_pskip, int p_only,
                        encode_stats_t *S, uint8_t ***out_mbmaps, int *out_mbw, int *out_mbh,
                        const char *maps_label )
{
    FILE *fp = fopen( outpath, "wb" );
    if( !fp )
    {
        fprintf( stderr, "open %s: %s\n", outpath, strerror( errno ) );
        exit( 1 );
    }

    x264_param_t p;
    set_params( &p, A, force_pskip, p_only );

    x264_t *enc = x264_encoder_open( &p );
    if( !enc )
    {
        fprintf( stderr, "x264_encoder_open failed\n" );
        exit( 1 );
    }

    x264_picture_t pic_in;
    x264_picture_t pic_out;
    if( x264_picture_alloc( &pic_in, A->csp, A->w, A->h ) < 0 )
    {
        fprintf( stderr, "x264_picture_alloc failed\n" );
        exit( 1 );
    }
    x264_picture_init( &pic_out );

    const int mbw = (A->w + 15) / 16;
    const int mbh = (A->h + 15) / 16;
    const size_t map_size = (size_t)mbw * mbh;
    *out_mbw = mbw;
    *out_mbh = mbh;

    uint8_t **mbmaps = calloc( (size_t)A->n, sizeof(*mbmaps) );
    if( !mbmaps )
    {
        fprintf( stderr, "out of memory\n" );
        exit( 1 );
    }
    for( int f = 0; f < A->n; f++ )
    {
        mbmaps[f] = malloc( map_size );
        if( !mbmaps[f] )
        {
            fprintf( stderr, "out of memory\n" );
            exit( 1 );
        }
    }

    char maps_dir[1024];
    snprintf( maps_dir, sizeof(maps_dir), "%s_%s_maps", A->out_prefix, maps_label );
    ensure_dir( maps_dir );

    double t0 = now_sec();
    size_t total_bytes = 0;
    int nnal = 0;
    x264_nal_t *nals = NULL;

    for( int f = 0; f < A->n; f++ )
    {
        fill_frame_synthetic( &pic_in, A->w, A->h, A->csp, A->scenario, f );
        build_mbinfo( mbmaps[f], mbw, mbh, A->scenario, f, A->wrong_hint );

        uint8_t *expected = malloc( map_size );
        if( !expected )
        {
            fprintf( stderr, "out of memory\n" );
            exit( 1 );
        }
        memcpy( expected, mbmaps[f], map_size );

        pic_in.prop.mb_info = mbmaps[f];
        pic_in.prop.mb_info_free = NULL;
        pic_in.i_pts = f;
        pic_in.i_type = f == 0 ? X264_TYPE_IDR : X264_TYPE_AUTO;

        int ret = x264_encoder_encode( enc, &nals, &nnal, &pic_in, &pic_out );
        if( ret < 0 )
        {
            free( expected );
            FAIL( "encode error" );
        }

        if( memcmp( mbmaps[f], expected, map_size ) != 0 )
        {
            free( expected );
            FAIL( "mb_info was mutated unexpectedly on frame %d", f );
        }
        free( expected );

        write_mbmap_pgm( maps_dir, "mbinfo", f, mbw, mbh, mbmaps[f] );

        if( ret > 0 )
        {
            if( fwrite( nals[0].p_payload, 1, (size_t)ret, fp ) != (size_t)ret )
                FAIL( "write failed" );
            total_bytes += (size_t)ret;
        }
    }

    while( 1 )
    {
        int ret = x264_encoder_encode( enc, &nals, &nnal, NULL, &pic_out );
        if( ret < 0 )
            FAIL( "encode flush error" );
        if( ret == 0 )
            break;
        if( fwrite( nals[0].p_payload, 1, (size_t)ret, fp ) != (size_t)ret )
            FAIL( "write failed" );
        total_bytes += (size_t)ret;
    }

    double t1 = now_sec();
    x264_picture_clean( &pic_in );
    x264_encoder_close( enc );
    fclose( fp );

    S->sec = t1 - t0;
    S->bytes = total_bytes;
    *out_mbmaps = mbmaps;
    return;

fail:
    x264_picture_clean( &pic_in );
    x264_encoder_close( enc );
    fclose( fp );
    exit( 1 );
}

static uint64_t hash_frame_pixels( const AVFrame *frame )
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( frame->format );
    if( !desc || desc->nb_components < 3 )
    {
        fprintf( stderr, "unsupported decoded pixel format %d\n", frame->format );
        exit( 1 );
    }

    uint64_t hash = 1469598103934665603ULL;
    for( int p = 0; p < 3; p++ )
    {
        const int plane_w = p ? AV_CEIL_RSHIFT( frame->width, desc->log2_chroma_w ) : frame->width;
        const int plane_h = p ? AV_CEIL_RSHIFT( frame->height, desc->log2_chroma_h ) : frame->height;
        for( int y = 0; y < plane_h; y++ )
        {
            const uint8_t *row = frame->data[p] + y * frame->linesize[p];
            for( int x = 0; x < plane_w; x++ )
            {
                hash ^= row[x];
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

static uint64_t hash_picture_pixels( const x264_picture_t *pic, int w, int h, int csp )
{
    uint64_t hash = 1469598103934665603ULL;
    const int cw = csp == X264_CSP_I420 ? w / 2 : w;
    const int ch = csp == X264_CSP_I420 ? h / 2 : h;

    for( int y = 0; y < h; y++ )
        for( int x = 0; x < w; x++ )
        {
            hash ^= pic->img.plane[0][y * pic->img.i_stride[0] + x];
            hash *= 1099511628211ULL;
        }
    for( int y = 0; y < ch; y++ )
        for( int x = 0; x < cw; x++ )
        {
            hash ^= pic->img.plane[1][y * pic->img.i_stride[1] + x];
            hash *= 1099511628211ULL;
        }
    for( int y = 0; y < ch; y++ )
        for( int x = 0; x < cw; x++ )
        {
            hash ^= pic->img.plane[2][y * pic->img.i_stride[2] + x];
            hash *= 1099511628211ULL;
        }

    return hash;
}

static void build_pristine_hashes( const args_t *A, frame_hash_t *hashes )
{
    x264_picture_t pic;
    if( x264_picture_alloc( &pic, A->csp, A->w, A->h ) < 0 )
    {
        fprintf( stderr, "x264_picture_alloc failed\n" );
        exit( 1 );
    }

    for( int f = 0; f < A->n; f++ )
    {
        fill_frame_synthetic( &pic, A->w, A->h, A->csp, A->scenario, f );
        hashes[f].w = A->w;
        hashes[f].h = A->h;
        hashes[f].format = A->csp == X264_CSP_I420 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV444P;
        hashes[f].hash = hash_picture_pixels( &pic, A->w, A->h, A->csp );
    }

    x264_picture_clean( &pic );
}

static void decode_hashes( const char *path, int nframes, frame_hash_t **out_hashes, int *out_count )
{
    *out_hashes = NULL;
    *out_count = 0;

    FILE *fp = fopen( path, "rb" );
    if( !fp )
    {
        fprintf( stderr, "open %s: %s\n", path, strerror( errno ) );
        exit( 1 );
    }
    fseek( fp, 0, SEEK_END );
    long sz = ftell( fp );
    fseek( fp, 0, SEEK_SET );
    if( sz <= 0 )
    {
        fprintf( stderr, "empty or unreadable stream %s\n", path );
        exit( 1 );
    }
    uint8_t *bitstream = malloc( (size_t)sz );
    if( !bitstream )
    {
        fprintf( stderr, "out of memory\n" );
        exit( 1 );
    }
    if( fread( bitstream, 1, (size_t)sz, fp ) != (size_t)sz )
    {
        fprintf( stderr, "read failed\n" );
        exit( 1 );
    }
    fclose( fp );

    const AVCodec *codec = avcodec_find_decoder( AV_CODEC_ID_H264 );
    AVCodecParserContext *parser = av_parser_init( AV_CODEC_ID_H264 );
    AVCodecContext *ctx = codec ? avcodec_alloc_context3( codec ) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    frame_hash_t *hashes = calloc( (size_t)nframes, sizeof(*hashes) );
    if( !codec || !parser || !ctx || !pkt || !frame || !hashes )
    {
        fprintf( stderr, "decoder allocation failed\n" );
        exit( 1 );
    }

    av_log_set_level( AV_LOG_WARNING );
    int ret_open = avcodec_open2( ctx, codec, NULL );
    if( ret_open < 0 )
    {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror( ret_open, err, sizeof(err) );
        fprintf( stderr, "decoder open failed (%d): %s\n", ret_open, err );
        exit( 1 );
    }

    uint8_t *data = bitstream;
    int data_size = (int)sz;
    int got_frames = 0;

    while( data_size > 0 )
    {
        int ret = av_parser_parse2( parser, ctx, &pkt->data, &pkt->size,
                                    data, data_size,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0 );
        if( ret < 0 )
        {
            fprintf( stderr, "parser error\n" );
            exit( 1 );
        }
        data += ret;
        data_size -= ret;

        if( pkt->size > 0 )
        {
            if( avcodec_send_packet( ctx, pkt ) < 0 )
            {
                fprintf( stderr, "send packet failed\n" );
                exit( 1 );
            }
            av_packet_unref( pkt );

            while( 1 )
            {
                ret = avcodec_receive_frame( ctx, frame );
                if( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
                    break;
                if( ret < 0 )
                {
                    fprintf( stderr, "receive frame failed\n" );
                    exit( 1 );
                }
                if( got_frames < nframes )
                {
                    hashes[got_frames].w = frame->width;
                    hashes[got_frames].h = frame->height;
                    hashes[got_frames].format = frame->format;
                    hashes[got_frames].hash = hash_frame_pixels( frame );
                }
                got_frames++;
                av_frame_unref( frame );
            }
        }
    }

    while( 1 )
    {
        int ret = av_parser_parse2( parser, ctx, &pkt->data, &pkt->size,
                                    NULL, 0,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0 );
        if( ret < 0 )
        {
            fprintf( stderr, "parser flush error\n" );
            exit( 1 );
        }
        if( pkt->size <= 0 )
            break;

        if( avcodec_send_packet( ctx, pkt ) < 0 )
        {
            fprintf( stderr, "send flushed packet failed\n" );
            exit( 1 );
        }
        av_packet_unref( pkt );

        while( 1 )
        {
            ret = avcodec_receive_frame( ctx, frame );
            if( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
                break;
            if( ret < 0 )
            {
                fprintf( stderr, "receive flushed frame failed\n" );
                exit( 1 );
            }
            if( got_frames < nframes )
            {
                hashes[got_frames].w = frame->width;
                hashes[got_frames].h = frame->height;
                hashes[got_frames].format = frame->format;
                hashes[got_frames].hash = hash_frame_pixels( frame );
            }
            got_frames++;
            av_frame_unref( frame );
        }
    }

    avcodec_send_packet( ctx, NULL );
    while( 1 )
    {
        int ret = avcodec_receive_frame( ctx, frame );
        if( ret == AVERROR_EOF || ret == AVERROR( EAGAIN ) )
            break;
        if( ret < 0 )
        {
            fprintf( stderr, "receive frame failed during flush\n" );
            exit( 1 );
        }
        if( got_frames < nframes )
        {
            hashes[got_frames].w = frame->width;
            hashes[got_frames].h = frame->height;
            hashes[got_frames].format = frame->format;
            hashes[got_frames].hash = hash_frame_pixels( frame );
        }
        got_frames++;
        av_frame_unref( frame );
    }

    av_parser_close( parser );
    avcodec_free_context( &ctx );
    av_frame_free( &frame );
    av_packet_free( &pkt );
    free( bitstream );

    *out_hashes = hashes;
    *out_count = got_frames;
}

static void compare_decoded_outputs( const char *reference, const char *candidate, int nframes )
{
    frame_hash_t *ref_hashes = NULL;
    frame_hash_t *cand_hashes = NULL;
    int ref_count = 0;
    int cand_count = 0;

    decode_hashes( reference, nframes, &ref_hashes, &ref_count );
    decode_hashes( candidate, nframes, &cand_hashes, &cand_count );

    if( ref_count != cand_count )
        FAIL( "decoded frame count mismatch: reference=%d candidate=%d", ref_count, cand_count );
    if( ref_count != nframes )
        FAIL( "decoded frame count mismatch: expected=%d actual=%d", nframes, ref_count );

    for( int f = 0; f < ref_count; f++ )
    {
        if( ref_hashes[f].w != cand_hashes[f].w ||
            ref_hashes[f].h != cand_hashes[f].h ||
            ref_hashes[f].format != cand_hashes[f].format ||
            ref_hashes[f].hash != cand_hashes[f].hash )
            FAIL( "decoded frame mismatch at frame %d", f );
    }

    free( ref_hashes );
    free( cand_hashes );
    return;

fail:
    free( ref_hashes );
    free( cand_hashes );
    exit( 1 );
}

static void compare_decoded_to_pristine( const args_t *A, const char *candidate )
{
    frame_hash_t *expected = calloc( (size_t)A->n, sizeof(*expected) );
    frame_hash_t *decoded = NULL;
    int decoded_count = 0;
    if( !expected )
    {
        fprintf( stderr, "out of memory\n" );
        exit( 1 );
    }

    build_pristine_hashes( A, expected );
    decode_hashes( candidate, A->n, &decoded, &decoded_count );

    if( decoded_count != A->n )
        FAIL( "decoded frame count mismatch against pristine: expected=%d actual=%d", A->n, decoded_count );

    for( int f = 0; f < A->n; f++ )
    {
        if( decoded[f].w != expected[f].w ||
            decoded[f].h != expected[f].h ||
            decoded[f].format != expected[f].format ||
            decoded[f].hash != expected[f].hash )
            FAIL( "decoded pristine mismatch at frame %d", f );
    }

    free( expected );
    free( decoded );
    return;

fail:
    free( expected );
    free( decoded );
    exit( 1 );
}

#ifdef AV_CODEC_FLAG2_EXPORT_MVS
/* Feed one received frame's MV side-data into a per-MB "nonzero motion" map,
 * using the correct block-CENTER convention (ffmpeg exports dst_x/dst_y as the
 * block center; each entry maps to exactly one MB). We only ever RECORD a
 * positive observation of nonzero motion; an absent entry (intra MB) is never
 * treated as a violation, so this cannot false-positive. */
static void accumulate_nonzero_mvs( const AVFrame *frame, int mbw, int mbh, uint8_t *nonzero )
{
    AVFrameSideData *sd = av_frame_get_side_data( frame, AV_FRAME_DATA_MOTION_VECTORS );
    if( !sd || sd->size < (int)sizeof(AVMotionVector) )
        return;

    const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
    const int mvcount = sd->size / (int)sizeof(AVMotionVector);

    for( int i = 0; i < mvcount; i++ )
    {
        const AVMotionVector *mv = &mvs[i];
        if( mv->motion_x == 0 && mv->motion_y == 0 )
            continue;
        const int bw = mv->w ? mv->w : 16;
        const int bh = mv->h ? mv->h : 16;
        const int cx = mv->dst_x - bw / 2;   /* block-center -> top-left corner */
        const int cy = mv->dst_y - bh / 2;
        const int mbx = cx / 16;
        const int mby = cy / 16;
        if( mbx >= 0 && mbx < mbw && mby >= 0 && mby < mbh )
            nonzero[mby * mbw + mbx] = 1;
    }
}

/* Decode the stream and, for every P-frame, assert that no hinted
 * (PERFECT_P_SKIP) macroblock resolved to a nonzero motion vector. This is the
 * direct test of the causality invariant. Fatal on violation; returns the
 * number of violating (frame,MB) observations. */
static int check_hinted_mvs_zero( const char *path, int nframes, int mbw, int mbh, uint8_t **mbmaps )
{
    int violations = 0;

    FILE *fp = fopen( path, "rb" );
    if( !fp ) { fprintf( stderr, "open %s: %s\n", path, strerror( errno ) ); exit( 1 ); }
    fseek( fp, 0, SEEK_END );
    long sz = ftell( fp );
    fseek( fp, 0, SEEK_SET );
    if( sz <= 0 ) { fprintf( stderr, "empty stream %s\n", path ); exit( 1 ); }
    uint8_t *bitstream = malloc( (size_t)sz );
    if( !bitstream || fread( bitstream, 1, (size_t)sz, fp ) != (size_t)sz )
    { fprintf( stderr, "read failed\n" ); exit( 1 ); }
    fclose( fp );

    const AVCodec *codec = avcodec_find_decoder( AV_CODEC_ID_H264 );
    AVCodecParserContext *parser = av_parser_init( AV_CODEC_ID_H264 );
    AVCodecContext *ctx = codec ? avcodec_alloc_context3( codec ) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if( !codec || !parser || !ctx || !pkt || !frame )
    { fprintf( stderr, "decoder alloc failed\n" ); exit( 1 ); }

    av_log_set_level( AV_LOG_WARNING );
    ctx->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    { fprintf( stderr, "decoder open failed\n" ); exit( 1 ); }

    uint8_t *badmap = calloc( (size_t)mbw * mbh, 1 );
    if( !badmap ) { fprintf( stderr, "out of memory\n" ); exit( 1 ); }

    uint8_t *data = bitstream;
    int data_size = (int)sz;
    int got_frames = 0;
    int mvs_seen = 0;

#define HANDLE_FRAME() do {                                                      \
        int f = got_frames++;                                                    \
        if( f < nframes && frame->pict_type != AV_PICTURE_TYPE_I ) {             \
            memset( badmap, 0, (size_t)mbw * mbh );                              \
            AVFrameSideData *sd = av_frame_get_side_data( frame, AV_FRAME_DATA_MOTION_VECTORS ); \
            if( sd ) mvs_seen = 1;                                               \
            accumulate_nonzero_mvs( frame, mbw, mbh, badmap );                   \
            const uint8_t *flags = mbmaps[f];                                    \
            for( int my = 0; my < mbh; my++ )                                    \
                for( int mx = 0; mx < mbw; mx++ )                                \
                    if( (flags[my*mbw+mx] & X264_MBINFO_PERFECT_P_SKIP) &&       \
                        badmap[my*mbw+mx] ) {                                     \
                        violations++;                                            \
                        if( violations <= 8 )                                    \
                            fprintf( stderr, "CAUSALITY VIOLATION: frame %d hinted MB(%d,%d) decoded with nonzero MV\n", f, mx, my ); \
                    }                                                            \
        }                                                                        \
        av_frame_unref( frame );                                                 \
    } while( 0 )

    while( data_size > 0 )
    {
        int ret = av_parser_parse2( parser, ctx, &pkt->data, &pkt->size,
                                    data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0 );
        if( ret < 0 ) { fprintf( stderr, "parser error\n" ); exit( 1 ); }
        data += ret;
        data_size -= ret;
        if( pkt->size > 0 )
        {
            if( avcodec_send_packet( ctx, pkt ) < 0 ) { fprintf( stderr, "send failed\n" ); exit( 1 ); }
            av_packet_unref( pkt );
            while( avcodec_receive_frame( ctx, frame ) >= 0 )
                HANDLE_FRAME();
        }
    }

    /* Drain the decoder so trailing frames are checked too. */
    avcodec_send_packet( ctx, NULL );
    while( avcodec_receive_frame( ctx, frame ) >= 0 )
        HANDLE_FRAME();

#undef HANDLE_FRAME

    if( !mvs_seen )
        fprintf( stderr, "WARNING: decoder exported no motion-vector side-data; MV invariant not checked "
                         "(pixel checks still enforced)\n" );

    free( badmap );
    av_parser_close( parser );
    avcodec_free_context( &ctx );
    av_frame_free( &frame );
    av_packet_free( &pkt );
    free( bitstream );
    return violations;
}
#endif /* AV_CODEC_FLAG2_EXPORT_MVS */

/* Negative test: prove the fast path actually engaged. We lie about one
 * changing interior MB (mark it PERFECT_P_SKIP). With the bypass ON the encoder
 * must force that MB to skip, so its decoded pixels equal the PREVIOUS frame's
 * block, not the (different) current source -> decode diverges from source.
 * With the bypass OFF the (lossless) encode reproduces the source exactly.
 * If the "ON" decode still matches source, the fast path did NOT run. */
static void run_negative_test( args_t *A )
{
    args_t na = *A;
    na.scenario = "none";     /* everything changes frame to frame */
    na.wrong_hint = 1;

    char out_on[1024], out_off[1024];
    snprintf( out_on,  sizeof(out_on),  "%s_neg_on.h264",  A->out_prefix );
    snprintf( out_off, sizeof(out_off), "%s_neg_off.h264", A->out_prefix );

    encode_stats_t s;
    uint8_t **maps_on = NULL, **maps_off = NULL;
    int mbw = 0, mbh = 0;

    /* feature ON: must diverge from source (proves engagement). */
    encode_one( &na, out_on, 1, 1, &s, &maps_on, &mbw, &mbh, "neg_on" );
    /* feature OFF control: must match source exactly. */
    encode_one( &na, out_off, 0, 1, &s, &maps_off, &mbw, &mbh, "neg_off" );

    frame_hash_t *pristine = calloc( (size_t)na.n, sizeof(*pristine) );
    frame_hash_t *don = NULL, *doff = NULL;
    int con = 0, coff = 0;
    if( !pristine ) { fprintf( stderr, "out of memory\n" ); exit( 1 ); }
    build_pristine_hashes( &na, pristine );
    decode_hashes( out_on, na.n, &don, &con );
    decode_hashes( out_off, na.n, &doff, &coff );

    if( con != na.n || coff != na.n )
    {
        fprintf( stderr, "negative test: decoded frame count mismatch (on=%d off=%d expected=%d)\n", con, coff, na.n );
        exit( 1 );
    }

    /* control: OFF must be lossless-exact against source on every frame. */
    for( int f = 0; f < na.n; f++ )
        if( doff[f].hash != pristine[f].hash )
        {
            fprintf( stderr, "negative test control FAILED: feature-off decode differs from source at frame %d\n", f );
            exit( 1 );
        }

    /* ON must diverge from source on at least one P-frame (frame >= 1). */
    int diverged = 0;
    for( int f = 1; f < na.n; f++ )
        if( don[f].hash != pristine[f].hash )
            diverged = 1;

    free( pristine ); free( don ); free( doff );
    for( int f = 0; f < na.n; f++ ) { free( maps_on[f] ); free( maps_off[f] ); }
    free( maps_on ); free( maps_off );

    if( !diverged )
    {
        fprintf( stderr, "ENGAGEMENT FAILED: wrong hint had no effect -> the P_SKIP bypass did not run\n" );
        exit( 1 );
    }
    printf( "Engagement: OK (wrong hint forced a visible skip -> fast path executed)\n" );
}

/* Bench mode: encode-only, no file or decode I/O, feature off then on over the
 * same in-memory input. Wrap the whole binary in `perf stat -r N taskset -c C`
 * for stable CPU measurement (wall-clock alone is noisy). */
static double bench_encode( const args_t *A, int force_pskip )
{
    x264_param_t p;
    set_params( &p, A, force_pskip, 1 );
    x264_t *enc = x264_encoder_open( &p );
    if( !enc ) { fprintf( stderr, "x264_encoder_open failed\n" ); exit( 1 ); }

    x264_picture_t pic_in, pic_out;
    if( x264_picture_alloc( &pic_in, A->csp, A->w, A->h ) < 0 )
    { fprintf( stderr, "x264_picture_alloc failed\n" ); exit( 1 ); }
    x264_picture_init( &pic_out );

    const int mbw = (A->w + 15) / 16, mbh = (A->h + 15) / 16;
    uint8_t *map = malloc( (size_t)mbw * mbh );
    if( !map ) { fprintf( stderr, "out of memory\n" ); exit( 1 ); }

    x264_nal_t *nals = NULL;
    int nnal = 0;
    double t0 = now_sec();
    for( int f = 0; f < A->n; f++ )
    {
        fill_frame_synthetic( &pic_in, A->w, A->h, A->csp, A->scenario, f );
        build_mbinfo( map, mbw, mbh, A->scenario, f, A->wrong_hint );
        pic_in.prop.mb_info = map;
        pic_in.prop.mb_info_free = NULL;
        pic_in.i_pts = f;
        pic_in.i_type = f == 0 ? X264_TYPE_IDR : X264_TYPE_AUTO;
        if( x264_encoder_encode( enc, &nals, &nnal, &pic_in, &pic_out ) < 0 )
        { fprintf( stderr, "encode error\n" ); exit( 1 ); }
    }
    while( x264_encoder_encode( enc, &nals, &nnal, NULL, &pic_out ) > 0 )
        ;
    double t1 = now_sec();

    free( map );
    x264_picture_clean( &pic_in );
    x264_encoder_close( enc );
    return t1 - t0;
}

static void run_bench( const args_t *A )
{
    /* PSKIP_BENCH_MODE=off|on runs a single configuration, so an external
     * profiler (perf stat -r N) can attribute counters to exactly one mode. */
    const char *only = getenv( "PSKIP_BENCH_MODE" );
    if( only && !strcmp( only, "off" ) )
    {
        printf( "Bench OFF %s %dx%d frames:%d : %.4f s\n", A->scenario, A->w, A->h, A->n, bench_encode( A, 0 ) );
        return;
    }
    if( only && !strcmp( only, "on" ) )
    {
        printf( "Bench ON  %s %dx%d frames:%d : %.4f s\n", A->scenario, A->w, A->h, A->n, bench_encode( A, 1 ) );
        return;
    }

    double off = bench_encode( A, 0 );
    double on  = bench_encode( A, 1 );
    printf( "Bench scenario:%s %dx%d %s frames:%d\n", A->scenario, A->w, A->h,
            (A->csp == X264_CSP_I420 ? "I420" : "I444"), A->n );
    printf( "  feature OFF: %.4f s\n", off );
    printf( "  feature ON : %.4f s\n", on );
    if( on > 0 )
        printf( "  ratio OFF/ON: %.2fx\n", off / on );
}

int main( int argc, char **argv )
{
    args_t A;
    parse_args( argc, argv, &A );

    if( A.bench )
    {
        run_bench( &A );
        return 0;
    }

    if( A.wrong_hint )
    {
        run_negative_test( &A );
        return 0;
    }

    char out_baseline[1024];
    char out_reference[1024];
    char out_pskip[1024];
    char out_source[1024];
    snprintf( out_baseline, sizeof(out_baseline), "%s_baseline.h264", A.out_prefix );
    snprintf( out_reference, sizeof(out_reference), "%s_reference.h264", A.out_prefix );
    snprintf( out_pskip, sizeof(out_pskip), "%s_pskip.h264", A.out_prefix );
    snprintf( out_source, sizeof(out_source), "%s_source.yuv", A.out_prefix );

    encode_stats_t Sb = { 0 }, Sp = { 0 };
    encode_stats_t Sr = { 0 };
    uint8_t **mbmaps_pskip = NULL;
    int mbw = 0, mbh = 0;

    write_source_sequence( &A, out_source );

    {
        uint8_t **tmp_maps = NULL;
        int tmp_mbw = 0, tmp_mbh = 0;
        encode_one( &A, out_baseline, 0, 0, &Sb, &tmp_maps, &tmp_mbw, &tmp_mbh, "baseline" );
        for( int f = 0; f < A.n; f++ )
            free( tmp_maps[f] );
        free( tmp_maps );

        tmp_maps = NULL;
        encode_one( &A, out_reference, 0, 1, &Sr, &tmp_maps, &tmp_mbw, &tmp_mbh, "reference" );
        for( int f = 0; f < A.n; f++ )
            free( tmp_maps[f] );
        free( tmp_maps );
    }

    encode_one( &A, out_pskip, 1, 1, &Sp, &mbmaps_pskip, &mbw, &mbh, "pskip" );
    compare_decoded_outputs( out_reference, out_pskip, A.n );
    compare_decoded_to_pristine( &A, out_reference );
    compare_decoded_to_pristine( &A, out_pskip );

    int violations = 0;
#ifdef AV_CODEC_FLAG2_EXPORT_MVS
    violations = check_hinted_mvs_zero( out_pskip, A.n, mbw, mbh, mbmaps_pskip );
#endif

    for( int f = 0; f < A.n; f++ )
        free( mbmaps_pskip[f] );
    free( mbmaps_pskip );

    printf( "Scenario:   %s\n", A.scenario );
    printf( "Resolution: %dx%d  CSP:%s  Frames:%d  MBs:%dx%d  QP:lossless\n",
            A.w, A.h, (A.csp == X264_CSP_I420 ? "I420" : "I444"), A.n, mbw, mbh );
    printf( "Baseline:  time = %.3f s, size = %.1f kB\n", Sb.sec, Sb.bytes / 1024.0 );
    printf( "P_SKIP:    time = %.3f s, size = %.1f kB\n", Sp.sec, Sp.bytes / 1024.0 );
    printf( "Source:     %s\n", out_source );
    printf( "Decode check: OK against P-only reference and pristine generated sequence\n" );

    if( violations )
    {
        fprintf( stderr, "FAIL: %d hinted MB(s) decoded with nonzero motion vectors\n", violations );
        return 1;
    }
    printf( "MV invariant: OK (all hinted MBs decoded with MV=(0,0))\n" );

    return 0;
}
