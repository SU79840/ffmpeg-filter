#include "libavutil/opt.h"
#include "internal.h"
#include "libswscale/swscale.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <GLFW/glfw3.h>

#define PIXEL_FORMAT GL_RGB


static void frame_yuv_alloc_by_wh(AVFrame* dst, int width, int height, int type)
{
    av_log(NULL, AV_LOG_ERROR, "alloc \n");

    dst->width = width;
    dst->height = height;
    dst->format = type;
    av_frame_get_buffer(dst, 0);
}



static int frame_format_scale(AVFrame* dst, int width, int height, AVFrame* src, int type) {

    av_log(NULL, AV_LOG_ERROR, "scale \n");

    struct SwsContext* sws_ctx = sws_getContext(src->width, src->height, (enum AVPixelFormat)src->format,
                                                dst->width, dst->height, (enum AVPixelFormat)type,
                                                SWS_POINT, NULL, NULL, NULL);
    if(sws_ctx == NULL){
        av_log(NULL, AV_LOG_ERROR, "sws ctx alloc failed");
        return -1;
    }
    sws_scale(sws_ctx, (const uint8_t * const*)src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    sws_freeContext(sws_ctx);
    return 0;
}



static int yuv_to_rgb(AVFrame* dst, AVFrame* src) {
    enum AVPixelFormat dstFormat = AV_PIX_FMT_RGB24;
    frame_yuv_alloc_by_wh(dst, src->width, src->height, dstFormat);
    int ret = frame_format_scale(dst, src->width, src->height, src, dstFormat);
    dst->pts = src->pts;
    return ret;
}


static const float position[24] = {
   -1.0f, -1.0f,  0.0f, 0.0f,
   1.0f, -1.0f,   2.0f, 0.0f,
   -1.0f, 1.0f,   0.0f, 2.0f,
   -1.0f, 1.0f,   0.0f, 2.0f,
   1.0f, -1.0f,   2.0f, 0.0f,
   1.0f, 1.0f,    2.0f, 2.0f
};

static const GLchar *v_shader_source =
  "attribute vec2 position;\n"
  "attribute vec2 inputTex;\n"
  "varying vec2 texCoord;\n"

  "void main(void) {\n"
  "  gl_Position = vec4(position, 0, 1);\n"
  "  texCoord = inputTex;\n"
  "}\n";

/*
 * x0.5+ 0.5 是将位置坐标position直计算变为了纹理的坐标？
 * 将frame data以纹理的贴在画面上了

*/
static const GLchar *f_shader_source =
  "uniform sampler2D tex;\n"
  "varying vec2 texCoord;\n"
  "void main() {\n"
  "  gl_FragColor = texture2D(tex, texCoord);\n"
  "}\n";


//static const GLchar *f_shader_source =
//  "uniform sampler2D tex;\n"
//  "varying vec2 texCoord;\n"
//  "const mediump vec3 luminanceWeighting = vec3(0.2125, 0.7154, 0.0721);\n"
//  "void main() {\n"
//  "  lowp vec4 textureColor = texture2D(tex, texCoord);\n"
//  "  lowp float luminance = dot(textureColor.rgb, luminanceWeighting);\n"
//  "  lowp vec3 greyScaleColor = vec3(luminance);\n"
//  "  gl_FragColor = vec4(mix(greyScaleColor, textureColor.rgb, 0.5), textureColor.w);\n"
//  "}\n";


//varying highp vec2 textureCoordinate;

// uniform sampler2D inputImageTexture;
// uniform lowp float saturation;

// // Values from "Graphics Shaders: Theory and Practice" by Bailey and Cunningham
// const mediump vec3 luminanceWeighting = vec3(0.2125, 0.7154, 0.0721);

// void main()
// {
//    lowp vec4 textureColor = texture2D(inputImageTexture, textureCoordinate);
//    lowp float luminance = dot(textureColor.rgb, luminanceWeighting);
//    lowp vec3 greyScaleColor = vec3(luminance);

//    gl_FragColor = vec4(mix(greyScaleColor, textureColor.rgb, saturation), textureColor.w);

// }

typedef struct {
  const AVClass *class;
  GLuint        program;
  GLuint        frame_tex;
  GLFWwindow    *window;
  GLuint        pos_buf;
} GenericShaderContext;

#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption genericshader_options[] = {{}, {NULL}};

AVFILTER_DEFINE_CLASS(genericshader);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type) {
  GLuint shader = glCreateShader(type);
  if (!shader || !glIsShader(shader)) {
    return 0;
  }
  glShaderSource(shader, 1, &shader_source, 0);
  glCompileShader(shader);
  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  return status == GL_TRUE ? shader : 0;
}

static void vbo_setup(GenericShaderContext *gs) {
  glGenBuffers(1, &gs->pos_buf);
  glBindBuffer(GL_ARRAY_BUFFER, gs->pos_buf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

  //得到着色器“position”的句柄
  GLint loc = glGetAttribLocation(gs->program, "position");
  //使数据在着色器中可用 只要在glDraw*系列函数前调用即可
  glEnableVertexAttribArray(loc);

  /*
   * 句柄id
   * 每次取的数量
   * 类型
   * 是否归一化
   * 应为位置坐标和顶点坐标可能放在一起 这里指的是与下一个顶点左边的距离
   * 偏移量

  */
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);


  GLint loc2 = glGetAttribLocation(gs->program, "inputTex");
  //使数据在着色器中可用 只要在glDraw*系列函数前调用即可
  glEnableVertexAttribArray(loc2);
  glVertexAttribPointer(loc2, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

}

static void tex_setup(AVFilterLink *inlink) {
  AVFilterContext     *ctx = inlink->dst;
  GenericShaderContext *gs = ctx->priv;

  glGenTextures(1, &gs->frame_tex);
  glActiveTexture(GL_TEXTURE0);

  glBindTexture(GL_TEXTURE_2D, gs->frame_tex);
  /*
    GL_REPEAT	对纹理的默认行为。重复纹理图像。
    GL_MIRRORED_REPEAT	和GL_REPEAT一样，但每次重复图片是镜像放置的。
    GL_CLAMP_TO_EDGE	纹理坐标会被约束在0到1之间，超出的部分会重复纹理坐标的边缘，产生一种边缘被拉伸的效果。
    GL_CLAMP_TO_BORDER	超出的坐标为用户指定的边缘颜色。
  */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);

  glUniform1i(glGetUniformLocation(gs->program, "tex"), 0);
}

static int build_program(AVFilterContext *ctx) {
  GLuint v_shader, f_shader;
  GenericShaderContext *gs = ctx->priv;

  if (!((v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER)) &&
        (f_shader = build_shader(ctx, f_shader_source, GL_FRAGMENT_SHADER)))) {
    return -1;
  }

  gs->program = glCreateProgram();
  glAttachShader(gs->program, v_shader);
  glAttachShader(gs->program, f_shader);
  glLinkProgram(gs->program);

  GLint status;
  glGetProgramiv(gs->program, GL_LINK_STATUS, &status);
  return status == GL_TRUE ? 0 : -1;
}

static av_cold int init(AVFilterContext *ctx) {
  return glfwInit() ? 0 : -1;
}

static int config_props(AVFilterLink *inlink) {
  AVFilterContext     *ctx = inlink->dst;
  GenericShaderContext *gs = ctx->priv;

  glfwWindowHint(GLFW_VISIBLE, 0);
  gs->window = glfwCreateWindow(inlink->w, inlink->h, "", NULL, NULL);

  glfwMakeContextCurrent(gs->window);

  #ifndef __APPLE__
  glewExperimental = GL_TRUE;
  glewInit();
  #endif

  glViewport(0, 0, inlink->w, inlink->h);

  int ret;
  if((ret = build_program(ctx)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "build shader error!");
    return ret;
  }

  glUseProgram(gs->program);
  vbo_setup(gs);
  tex_setup(inlink);
  return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
  AVFilterContext *ctx     = inlink->dst;
  AVFilterLink    *outlink = ctx->outputs[0];


  av_log(NULL, AV_LOG_ERROR, "in frame format %d \n", in->format);
  if(in->format != AV_PIX_FMT_YUV420P) {
      av_log(NULL, AV_LOG_ERROR, "format not yuv420 \n");

  }
  av_log(NULL, AV_LOG_ERROR, "convert yuv420 tp rgb24 \n");
  AVFrame *tmp = av_frame_alloc();
  int ret = yuv_to_rgb(tmp, in);
  if(ret != 0) {
      av_frame_free(&in);
      return AVERROR(ENOMEM);
  }

  av_log(NULL, AV_LOG_ERROR, "format success tmp format %d\n", tmp->format);


  AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
  if (!out) {
     av_frame_free(&tmp);
     return AVERROR(ENOMEM);
  }

  av_frame_copy_props(out, in);


  AVFrame* outtmp = av_frame_alloc();

  frame_yuv_alloc_by_wh(outtmp, tmp->width, tmp->height, tmp->format);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, tmp->data[0]);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glReadPixels(0, 0, outlink->w, outlink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (GLvoid *)outtmp->data[0]);

  //out tmp to out
  enum AVPixelFormat dstFormat = AV_PIX_FMT_YUV420P;
//  frame_yuv_alloc_by_wh(dst, src->width, src->height, dstFormat);
  int ret2 = frame_format_scale(out, outtmp->width, outtmp->height, outtmp, dstFormat);
  av_frame_free(&outtmp);

  av_frame_free(&tmp);
  av_frame_free(&in);
  av_log(NULL, AV_LOG_ERROR, "format success2 ret %d  format %d\n", ret2, out->format);

  return ff_filter_frame(outlink, out);

//  AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
//  if (!out) {
//    av_frame_free(&in);
//    return AVERROR(ENOMEM);
//  }
//  av_frame_copy_props(out, in);

//  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, in->data[0]);
//  glDrawArrays(GL_TRIANGLES, 0, 6);
//  glReadPixels(0, 0, outlink->w, outlink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (GLvoid *)out->data[0]);

//  av_frame_free(&in);
//  return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx) {
  GenericShaderContext *gs = ctx->priv;
  glDeleteTextures(1, &gs->frame_tex);
  glDeleteProgram(gs->program);
  glDeleteBuffers(1, &gs->pos_buf);
  glfwDestroyWindow(gs->window);
}

static int query_formats(AVFilterContext *ctx) {
  static const enum AVPixelFormat formats[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
  return ff_set_common_formats(ctx, ff_make_format_list(formats));
}

static const AVFilterPad genericshader_inputs[] = {
  {.name = "default",
   .type = AVMEDIA_TYPE_VIDEO,
   .config_props = config_props,
   .filter_frame = filter_frame},
  {NULL}};

static const AVFilterPad genericshader_outputs[] = {
  {.name = "default", .type = AVMEDIA_TYPE_VIDEO}, {NULL}};

AVFilter ff_vf_genericshader = {
  .name          = "genericshader",
  .description   = NULL_IF_CONFIG_SMALL("Generic OpenGL shader filter"),
  .priv_size     = sizeof(GenericShaderContext),
  .init          = init,
  .uninit        = uninit,
  .query_formats = query_formats,
  .inputs        = genericshader_inputs,
  .outputs       = genericshader_outputs,
  .priv_class    = &genericshader_class,
  .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC};
