/*
    suxunquan test ffmpeg filter
*/
#include "pthread.h"
#include "sys/syscall.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avassert.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"


pid_t gettid() {
    return SYS_gettid;
}

typedef struct SufilterContext {
    const AVClass *class;
    int backUp;
    //add some private data if you want
} SufilterContext;

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

static void image_copy_plane(uint8_t *dst, int dst_linesize,
                         const uint8_t *src, int src_linesize,
                         int bytewidth, int height)
{
    if (!dst || !src)
        return;
    av_assert0(abs(src_linesize) >= bytewidth);
    av_assert0(abs(dst_linesize) >= bytewidth);
    for (;height > 0; height--) {
        memcpy(dst, src, bytewidth);
        dst += dst_linesize;
        src += src_linesize;
    }
}

//for YUV data, frame->data[0] save Y, frame->data[1] save U, frame->data[2] save V
static int frame_copy_video(AVFrame *dst, const AVFrame *src)
{
    int i, planes;

    if (dst->width  > src->width ||
        dst->height > src->height)
        return AVERROR(EINVAL);

    planes = av_pix_fmt_count_planes(dst->format);
    //make sure data is valid
    for (i = 0; i < planes; i++)
        if (!dst->data[i] || !src->data[i])
            return AVERROR(EINVAL);

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(dst->format);
    int planes_nb = 0;
    for (i = 0; i < desc->nb_components; i++)
        planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);

    for (i = 0; i < planes_nb; i++) {
        int h = dst->height;
        int bwidth = av_image_get_linesize(dst->format, dst->width, i);
        if (bwidth < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_image_get_linesize failed\n");
            return;
        }
        if (i == 1 || i == 2) {
            h = AV_CEIL_RSHIFT(dst->height, desc->log2_chroma_h);
        }
        image_copy_plane(dst->data[i], dst->linesize[i],
                            src->data[i], src->linesize[i],
                            bwidth, h);
    }
    return 0;
}

static int do_conversion(AVFilterContext *ctx, void *arg, int jobnr,
                        int nb_jobs)
{
    av_log(NULL, AV_LOG_INFO, "do_conversion nb_jobs %d \n", nb_jobs);

//    char tname[16];
//    const int getname_rv = pthread_getname_np(pthread_self(), tname, 16);
//    if(getname_rv) {
//        av_log(NULL, AV_LOG_INFO, "thread id %s \n", tname);
//    } else {
//        av_log(NULL, AV_LOG_INFO, "thread id : unknow \n");
//    }
    av_log(NULL, AV_LOG_INFO, "thread id (getid): %u \n", gettid() );
    av_log(NULL, AV_LOG_INFO, "thread id (pthread_self): %lu \n", pthread_self());

    SufilterContext *privCtx = ctx->priv;
    ThreadData *td = arg;
    AVFrame *dst = td->out;
    AVFrame *src = td->in;

    frame_copy_video(dst, src);
    return 0;
}

static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    av_log(NULL, AV_LOG_WARNING, "filter_frame, link %x, frame %x \n", link, in);
    AVFilterContext *avctx = link->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *out;

    //allocate a new buffer, data is null
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    //the new output frame, property is the same as input frame, only width/height is different
    av_frame_copy_props(out, in);
    out->width  = outlink->w;
    out->height = outlink->h;

    ThreadData td;
    td.in = in;
    td.out = out;
    int res;
    /*
     * 开启子线程
     * 1. AVFilterContext
     * 2. 要执行的函数
     * 3. 返回值相关？
     * 4.线程数  默认的execute是把一个frame分块 然后for循环遍历 并没有多线程
     *          如果没有自己去实现多线程 而且do_conversion()没有分块逻辑 会导致执行多次
       //default_execute 只能用了一个for循环去调用do_conversion(...) : func(ctx, arg, i, nb_jobs);
       //
    */
    if(res = avctx->internal->execute(avctx, do_conversion, &td, NULL, FFMIN(outlink->h, avctx->graph->nb_threads))) {
        return res;
    }

    av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static av_cold int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    SufilterContext *privCtx = ctx->priv;

    //you can modify output width/height here
    outlink->w = ctx->inputs[0]->w/2;
    outlink->h = ctx->inputs[0]->h/2;
    av_log(NULL, AV_LOG_DEBUG, "configure output, w h = (%d %d), format %d \n", outlink->w, outlink->h, outlink->format);

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    av_log(NULL, AV_LOG_DEBUG, "init \n");
    SufilterContext *privCtx = ctx->priv;
    //init something here if you want
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    av_log(NULL, AV_LOG_DEBUG, "uninit \n");
    SufilterContext *privCtx = ctx->priv;
    //uninit something here if you want
}

//currently we just support the most common YUV420, can add more if needed
static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}


/*
 * OFFSET offsetof用来确定该成员变量在Class中的位置
 * AVOption 是init滤镜时可能需要设置的参数 这里为空
*/
#define OFFSET(x) offsetof(SufilterContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption sufilter_options[] = {
    { "backUp",         "a backup parameters, NOT use so far",          OFFSET(backUp),    AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { NULL }

};// TODO: add something if needed

static const AVClass sufilter_class = {
    .class_name       = "sufilter",
    .item_name        = av_default_item_name,
    .option           = sufilter_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad avfilter_vf_sufilter_inputs[] = {
    {
        .name         = "sufilter_inputpad",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_sufilter_outputs[] = {
    {
        .name = "sufilter_outputpad",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_sufilter = {
    .name           = "sufilter",
    .description    = NULL_IF_CONFIG_SMALL("cut a part of video"),
    .priv_size      = sizeof(SufilterContext),
    .priv_class     = &sufilter_class,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs         = avfilter_vf_sufilter_inputs,
    .outputs        = avfilter_vf_sufilter_outputs,
};
